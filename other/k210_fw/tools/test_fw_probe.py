# -*- coding: utf-8 -*-
"""Host test for k210_fw/tools/fw_probe.py - the one-cold-boot firmware decision probe.

Why this file needs guarding: the probe is the thing we hand to the board BEFORE we
know whether the firmware on it is any good.  If it is wrong, or if it is not
read-only, it costs a flash + a power cycle and can brick the saved GC-heap split.
So it must be:
  * pure ASCII and small (it is saved as /flash/main.py and must compile even when
    the saved GC heap is tight);
  * READ-ONLY - it may print gc_heap_size() but must NEVER write one (a bad saved
    value is what stopped park_app.py from compiling);
  * free of camera/LCD/network work at import time, so it runs on a board whose
    sensor or SD is broken;
  * armed with the real model byte counts (the same ones park_app.py budgets with),
    because its whole point is to compare them against sys_free + gc_heap.
"""
import contextlib
import io
import os
import re
import sys
import types

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "tools", "fw_probe.py")
MAIN = os.path.join(REPO, "park_app.py")
BUDGET_TEST = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "test_mem_budget.py")

K = 1024
# the exact vendor file sizes (also asserted in test_mem_budget.py)
DET, RECOG, WEIGHT = 460456, 697512, 1498500
# the board's real cold-boot numbers with the FULL (vendor standard) image
BOARD_SYS_FREE = 2514944
BOARD_GC = 475136


def read():
    return open(SRC, encoding="utf-8").read()


def run_probe(sys_free, gc_heap, present_modules=(), plate_api=True):
    """Exec the probe with a fake maix/uos and return everything it printed."""
    src = read()
    real_maix = sys.modules.get("maix")
    real_uos = sys.modules.get("uos")

    class FakeKPU(object):
        pass
    for a in ("load_kmodel", "init_yolo2", "run_with_output", "regionlayer_yolo2",
              "lp_recog_load_weight_data", "lp_recog", "pix_to_ai"):
        setattr(FakeKPU, a, (lambda self: None))
    if not plate_api:
        for a in ("init_yolo2", "lp_recog"):
            delattr(FakeKPU, a)

    maix = types.ModuleType("maix")
    maix.KPU = FakeKPU
    utils = types.SimpleNamespace()
    utils.heap_free = lambda: sys_free
    utils.gc_heap_size = lambda: gc_heap
    maix.utils = utils
    sys.modules["maix"] = maix

    uos = types.ModuleType("uos")
    uos.mount = lambda *a: None
    uos.umount = lambda *a: None
    uos.statvfs = lambda *a: ()
    uos.ilistdir = lambda *a: ()
    uos.listdir = lambda *a: []
    uos.stat = lambda *a: (_ for _ in ()).throw(OSError(2))
    sys.modules["uos"] = uos

    saved_hook = sys.modules.get("sys")
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(buf):
            ns = {"__name__": "fw_probe"}
            exec(compile(src, SRC, "exec"), ns)
    finally:
        if real_maix is None:
            sys.modules.pop("maix", None)
        else:
            sys.modules["maix"] = real_maix
        if real_uos is None:
            sys.modules.pop("uos", None)
        else:
            sys.modules["uos"] = real_uos
        assert sys.modules.get("sys") is saved_hook
    return buf.getvalue()


def line_with(out, needle):
    for l in out.split("\n"):
        if needle in l:
            return l
    raise AssertionError("no line containing %r in:\n%s" % (needle, out))


def main():
    src = read()

    print("== 1. board-side file rules: ASCII, small, compiles ==")
    assert all(ord(c) < 128 for c in src), "board-side file: ASCII only"
    n = len(src.encode())
    # It is saved as /flash/main.py, so it has to COMPILE on whatever GC heap the
    # last experiment saved.  park_app.py (122KB) needed >248KB, so a file an order
    # of magnitude smaller is safe; keep it there and it can never be the reason a
    # probe run fails.
    assert n < 10240, "must stay small: it is saved as /flash/main.py (%d B)" % n
    assert n * 8 < os.path.getsize(MAIN), \
        "the probe must stay far smaller than park_app.py (%d vs %d B)" % (
            n, os.path.getsize(MAIN))
    compile(src, SRC, "exec")
    print("   %d B, pure ASCII, compiles (park_app.py is %d B)"
          % (n, os.path.getsize(MAIN)))

    print("== 2. it is READ-ONLY (a bad saved GC heap bricks the boot) ==")
    for m in re.finditer(r"gc_heap_size\(", src):
        rest = src[m.end():m.end() + 2]
        assert rest.startswith(")"), \
            "the probe must only READ gc_heap_size(): found %r" % rest
    assert "uos.mount(" not in src and "uos.umount(" not in src, \
        "the probe must not mount anything: SD stays the firmware's business"
    assert not re.search(r"open\([^)]*['\"]w", src), "no writes"
    print("   gc_heap_size() read-only; no mount/umount; no file writes")

    print("== 3. nothing heavy at import time (sensor/LCD/network) ==")
    for bad in ("import sensor", "import lcd", "import network"):
        assert not re.search(r"^\s*%s" % bad, src, re.M), \
            "%s would make the probe fail on a broken board" % bad
    assert "sensor" not in re.sub(r"#[^\n]*", "", src), \
        "no sensor reference outside comments"
    print("   no sensor/lcd/network imports")

    print("== 4. the model byte counts match the ones park_app.py budgets with ==")
    for name, want in (("MODEL_DET", DET), ("MODEL_RECOG", RECOG),
                       ("MODEL_WEIGHT", WEIGHT)):
        got = int(re.search(r"^%s = (\d+)" % name, src, re.M).group(1))
        assert got == want, (name, got, want)
    bsrc = open(BUDGET_TEST, encoding="utf-8").read()
    for name, want in (("DET", DET), ("RECOG", RECOG), ("WEIGHT", WEIGHT)):
        assert re.search(r"^%s = %d$" % (name, want), bsrc, re.M), \
            "test_mem_budget.py must agree on %s" % name
    msrc = open(MAIN, encoding="utf-8").read()
    assert '"lp_detect.kmodel"' in msrc and '"lp_recog.kmodel"' in msrc, \
        "park_app.py must list the same three file names"
    print("   det=%d recog=%d weight=%d (sum %d) - board budget test agrees"
          % (DET, RECOG, WEIGHT, DET + RECOG + WEIGHT))

    print("== 5. it does the shared-pool arithmetic (sys_free + gc_heap) ==")
    assert re.search(r"total = SYSF \+ GCH", src), \
        "the pool total must be sys_free + gc_heap: that is the whole point"
    assert "SLACK = 512 * 1024" in src, \
        "the slack must be the same 512KB park_app.py's _mem_budget uses"
    print("   pool = sys_free + gc_heap; slack 512KB (same rule as park_app.py)")

    print("== 6. the FULL image's real numbers come out SHORT for QVGA ==")
    out = run_probe(BOARD_SYS_FREE, BOARD_GC)
    pool = line_with(out, "[BUD] pool")
    assert str(BOARD_SYS_FREE + BOARD_GC) in pool, pool
    assert "2990080" in pool, pool
    core_cam = line_with(out, "core models + QVGA camera")
    need = RECOG + WEIGHT + 2 * 320 * 240 * 2 + 512 * K
    assert "SHORT by %d" % (need - (BOARD_SYS_FREE + BOARD_GC)) in core_cam, core_cam
    assert "SHORT by 37420" in core_cam, core_cam
    print("   %s" % pool.strip())
    print("   %s" % core_cam.strip())

    print("== 7. a Lite-sized pool flips that line to OK ==")
    out = run_probe(3 * 1024 * 1024 + 400 * K, 131072)
    core_cam = line_with(out, "core models + QVGA camera")
    assert "OK" in core_cam, core_cam
    all3_cam = line_with(out, "all 3 models + QVGA camera")
    assert "OK" in all3_cam, all3_cam
    print("   %s" % core_cam.strip())

    print("== 8. it shouts about a soft reset and forbids IDE Run ==")
    out = run_probe(144 * K, 475136)
    assert "SOFT reset" in out, out
    assert "POWER-CYCLE" in out, out
    assert "NEVER use IDE" in read() or "NEVER use IDE" in out, \
        "the instructions must forbid IDE Run (it does not return KPU memory)"
    print("   %s" % line_with(out, "!! sys_free").strip())

    print("== 9. KPU API inventory distinguishes the old plate API ==")
    out = run_probe(BOARD_SYS_FREE, BOARD_GC)
    assert "plate API  : COMPLETE" in out, out
    assert "old API 7/7" in out, out
    out = run_probe(BOARD_SYS_FREE, BOARD_GC, plate_api=False)
    assert "MISSING" in line_with(out, "plate API  :"), out
    assert "old API 5/7" in out, out
    print("   complete vs missing both reported")

    print("== 10. degraded boards do not crash the probe ==")
    real = sys.modules.get("maix")
    try:
        sys.modules["maix"] = types.ModuleType("maix")   # no .utils, no .KPU
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            exec(compile(read(), SRC, "exec"), {"__name__": "fw_probe"})
        out = buf.getvalue()
    finally:
        if real is None:
            sys.modules.pop("maix", None)
        else:
            sys.modules["maix"] = real
    assert "no maix.utils" in out, out
    assert "cannot judge" in out, out
    assert "[END] send ALL of the above back" in out, out
    print("   no maix.utils -> still prints identity + [END] and never raises")

    print("\nALL FW-PROBE CHECKS PASSED")


main()
