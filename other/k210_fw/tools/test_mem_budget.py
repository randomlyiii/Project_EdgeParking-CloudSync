# -*- coding: utf-8 -*-
"""Host test for park_app.py's memory pre-flight (_mem_budget / _kpu_mem_verdict).

Board facts this encodes (2026-09-13 probe, see k210_fw/mem_probe.py):
    M0 boot     sys_free 2203648   (2.10 MB, GC heap 768432 = 750 KB)
    M1 cam      sys_free 1814528   (camera buffers cost 389 KB)
    K1 det      sys_free 1351680   (lp_detect.kmodel: 501 KB resident)
    K1b yolo2   sys_free 1282048   (init_yolo2: +68 KB)
    K3 fail     sys_free  581632   (recog kmodel loaded, weight still FAILED)
    weight file 1498500 (1.43 MB)  -- zlib only reaches 90%, so not compressible
=> need ~3.16 MB vs total 2.10 MB: this firmware cannot run the dual model.
The pre-flight must say TIGHT instead of letting the C layer print
"model size ..." and hang the board.
"""
import collections
import os
import re
import sys
import types

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "park_app.py")

BOARD_FREE_BOOT = 2203648
BOARD_FREE_CAM = 1814528
BOARD_GC_HEAP = 768432
DET = 460456
RECOG = 697512
WEIGHT = 1498500


def extract():
    """Pull _mem_budget + _kpu_mem_verdict out of park_app.py (no hardware import)."""
    src = open(SRC, encoding="utf-8").read()
    a = src.index("def _mem_budget(")
    b = src.index("def _maix_utils():", a)
    return src[a:b]


ns = {}
exec(compile(extract(), "park_app.py", "exec"), ns)
budget = ns["_mem_budget"]
verdict = ns["_kpu_mem_verdict"]


def main():
    print("== 1. this board's real numbers -> TIGHT ==")
    need = budget((DET, RECOG, WEIGHT))
    assert need == DET + RECOG + WEIGHT + 512 * 1024, need
    n, total, tight = verdict(BOARD_FREE_CAM, BOARD_GC_HEAP, (DET, RECOG, WEIGHT))
    assert tight is True, (n, total)
    assert total == BOARD_FREE_CAM + BOARD_GC_HEAP
    print("   need=%dKB total=%dKB -> tight=%s"
          % (n // 1024, total // 1024, tight))
    assert n // 1024 == 3106, n // 1024

    print("== 2. even with the camera never opened it is still TIGHT ==")
    n, total, tight = verdict(BOARD_FREE_BOOT, BOARD_GC_HEAP, (DET, RECOG, WEIGHT))
    assert tight is True, "boot-time free is not enough either"
    print("   boot free=%dKB + gc=%dKB vs need=%dKB -> tight"
          % (BOARD_FREE_BOOT // 1024, BOARD_GC_HEAP // 1024, n // 1024))

    print("== 3. detection model alone fits (that is why it loads) ==")
    n, total, tight = verdict(BOARD_FREE_BOOT, BOARD_GC_HEAP, (DET,))
    assert tight is False, (n, total)
    print("   det only: need=%dKB total=%dKB -> not tight" % (n // 1024, total // 1024))

    print("== 4. known limit of the file-size rule (documented, not a bug) ==")
    # recog+weight on a fresh boot: the rule says "fits" (2.66MB vs 2.84MB) but the
    # board says otherwise -- the recog kmodel costs ~700KB for a 681KB file, and
    # lp_recog_load_weight_data needs extra staging on top of the 1.43MB file.
    # The rule is deliberately a first-order filter: it reliably rejects the full
    # three-file combo (the case that hung the board) and never promises success.
    n, total, tight = verdict(BOARD_FREE_BOOT, BOARD_GC_HEAP, (RECOG, WEIGHT))
    print("   recog+weight: rule says need=%dKB total=%dKB -> tight=%s"
          % (n // 1024, total // 1024, tight))
    assert tight is False, "documenting the rule's optimism, board reality differs"
    n3, t3, tight3 = verdict(BOARD_FREE_BOOT, BOARD_GC_HEAP, (DET, RECOG, WEIGHT))
    assert tight3 is True, "the three-file combo (the one that hung) must be rejected"

    print("== 5. shrinking the GC heap is NOT a free fix (and needs a reboot) ==")
    # gc 750KB -> 128KB would buy 622KB, but the shortfall here is bigger than that
    # AND the write only lands on the next boot (gc_heap_size() just stores the number
    # in freq.conf), so the saved value must also stay big enough to compile park_app.py.
    # The realistic fix for the FULL image is on the system-heap side: the Lite build.
    n, total, tight = verdict(BOARD_FREE_BOOT, 131072, (DET, RECOG, WEIGHT))
    assert tight is True, (n, total)
    print("   full combo with gc=128KB: need=%dKB total=%dKB -> still tight"
          % (n // 1024, total // 1024))
    n, total, tight = verdict(BOARD_FREE_BOOT, 131072, (RECOG, WEIGHT))
    assert tight is True, (n, total)
    print("   recog+weight with gc=128KB: need=%dKB total=%dKB -> still tight"
          % (n // 1024, total // 1024))

    print("== 6. a Lite-style split passes (bigger system heap, small GC) ==")
    n, total, tight = verdict(3600000, 131072, (DET, RECOG, WEIGHT))
    assert tight is False, (n, total)
    print("   sys=3.43MB gc=128KB -> plenty")

    print("== 7. degraded inputs do not raise ==")
    assert budget((0, 0, 0)) == 512 * 1024
    assert budget((-1, DET)) == DET + 512 * 1024
    n, total, tight = verdict(-1, 0, (DET,))
    assert total == 0 and tight is True
    assert budget(()) == 512 * 1024
    print("   zero/negative/empty all handled")

    print("== 8. the boot sequence is the one the board measurements demand ==")
    src = open(SRC, encoding="utf-8").read()
    # 2026-09-13 board measurements replaced the old "pre-flight verdict" policy:
    # trying directly and adapting (shrink the GC heap, drop the detect model) beats
    # guessing from file sizes.  What must hold now:
    #   ① mount SD  ② recog model + weight  ③ camera  ④ optional detect model
    # The detect model moved AFTER the camera on purpose: it is the optional 460KB+
    # 68KB, and if it grabbed memory first the 389KB camera buffer would fail instead
    # - leaving a black screen, which is the worst possible outcome.
    i_sd = src.index("sd_wait_until_mounted()")
    i_kpu = src.index("kpu_load()", i_sd)
    i_cam = src.index("cam_init()", i_kpu)
    i_det = src.index("kpu_load_detect()", i_cam)
    assert i_sd < i_kpu < i_cam < i_det, \
        "order: mount SD -> recog+weight -> camera -> optional detect model"
    # ⛔ and the weight must NOT be able to shrink the GC heap any more (2026-09-13):
    # shrinking buys no system heap here, and a small saved value makes park_app.py
    # uncompilable on the next boot.
    assert "K210_WEIGHT_GC_HEAP" not in src, \
        "the weight retry must not shrink gc_heap (that bricked the board)"
    assert "_apply_gc_heap(HEAP_TUNE_GC_RESTORE)" in src, \
        "the only gc_heap write allowed is raising it back to the default"
    assert "def _load_weight(" in src and "def _load_recog_api(" in src, \
        "recog+weight are their own steps so the heap can be juggled between them"
    assert "def _load_detect_cleanup(" in src, \
        "the detect model must be optional (released if it does not fit)"
    assert "[BOOT] firmware:" in src, "boot banner should report the firmware id"
    i_recog = src.index("rec = _load_recog_api(paths)")
    i_weight = src.index("_load_weight(rec, weight_path, sizes[2])")
    assert i_recog < i_weight, "the fat weight allocation comes after the recog model"
    # kpu_load() must NOT pull in the detect model any more: that is kpu_load_detect()'s
    # job, and it runs after the camera has taken its buffer.
    i_load = src.index("def kpu_load():")
    i_next = src.index("def _extend_box(", i_load)
    assert "_load_detect_cleanup(" not in src[i_load:i_next], \
        "kpu_load must not try the detect model (it runs after the camera)"
    print("   order ok; recog -> weight -> camera -> optional det")

    print("== 8b. camera/LCD memory rules (each one cost a board crash) ==")
    import ast as _ast
    tree = _ast.parse(src)
    # (a) NEVER lcd.deinit() then re-configure the camera: on 2026-09-13 that hard-faulted
    #     the board (EPC 0x8006d722 / Cause 0x0).  Releasing the KPU object (det.deinit())
    #     and releasing our own SPI object (self.spi.deinit(), added 2026-09-14 because a
    #     second SPI object on the same peripheral hung the card) are both fine.
    # 2026-09-16: `d.spi.deinit()` is the same release, done by the SD retry gate
    #     (`_sd_release()`) before it builds the next SDSPI - the retry loop made the
    #     "old object still open" hazard reachable, so this one MUST be allowed.
    ALLOWED_DEINIT = ("det.deinit", "self.spi.deinit", "d.spi.deinit")
    bad = [(n.lineno, _ast.unparse(n.func)) for n in _ast.walk(tree)
           if isinstance(n, _ast.Call) and isinstance(n.func, _ast.Attribute)
           and n.func.attr == "deinit"
           and _ast.unparse(n.func) not in ALLOWED_DEINIT]
    assert not bad, ("only %s may be deinit'd - lcd.deinit() + camera re-config"
                     " hard-faults the board: %s" % (ALLOWED_DEINIT, bad))
    # (b) the frame buffer count must be set BEFORE set_framesize (that call is what
    #     allocates the buffers - and what raised OSError(12) ENOMEM on the board)
    fn = next(n for n in _ast.walk(tree)
              if isinstance(n, _ast.FunctionDef) and n.name == "_sensor_setup")
    framesize = sorted(n.lineno for n in _ast.walk(fn) if isinstance(n, _ast.Call)
                       and isinstance(n.func, _ast.Attribute)
                       and n.func.attr == "set_framesize")
    fbuf = sorted(n.lineno for n in _ast.walk(fn) if isinstance(n, _ast.Call)
                  and isinstance(n.func, _ast.Name) and n.func.id == "fn")
    assert fbuf and framesize and min(fbuf) < min(framesize), \
        "set_framebuffernum must run before set_framesize"
    # (c) the default must be the split that actually fits: recog 697512B + weight
    #     1498500B leave ~300KB, and LCD(152KB)+QVGA camera(~300KB) does not fit
    #     (on the FULL image's 2.99MB pool; the Lite image's 4.31MB pool does fit).
    assert re.search(r"^LCD_PREVIEW = False", src, re.M), \
        "LCD + QVGA camera + core models cannot fit in the FULL image's pool"
    # (d) the sensor flip calls must come AFTER set_framesize (2026-09-14).  The vendor
    #     example is set_framesize -> set_pixformat -> set_vflip, and set_framesize
    #     rewrites the sensor window - which is the most likely reason the old
    #     "sensor-layer flips do nothing on this firmware" note was ever true.
    flips = sorted(n.lineno for n in _ast.walk(fn) if isinstance(n, _ast.Call)
                   and isinstance(n.func, _ast.Attribute)
                   and n.func.attr in ("set_vflip", "set_hmirror"))
    assert flips and min(flips) > max(framesize), \
        "set_vflip/set_hmirror must run AFTER set_framesize (vendor order)"
    assert "[CAM]" in src, "the boot log must record the orientation config"
    # (e) the preview JPEG path must not fail silently: `[stat] jpeg=0B` with no reason
    #     cost a board round trip on 2026-09-14.
    jf = src[src.index("def jpeg_from("):src.index("def capture_jpeg(")]
    assert "_dbg_once(" in jf, "a failing compress() must say so"
    assert "except Exception:" not in jf.split("_dbg_once")[0].rsplit("try:", 1)[-1], \
        "no bare silent except before the diagnostic"
    assert "comp if comp is not None else img" not in src, \
        "raw RGB565 must never be sent as if it were a JPEG"
    # (f) ORIENT_PROBE must still send the preview: the whole point is to LOOK at the
    #     four flip combinations, and the probe's own comment says "IDE frame buffer".
    assert "(ORIENT_PROBE or not CONSOLE_PREVIEW)" not in src, \
        "the orientation probe must not mute the preview it exists to show"
    print("   no lcd.deinit(); framebuffers first; flips after set_framesize;"
          " no silent jpeg failure; probe keeps the preview")

    print("== 9. firmware id helper is careful ==")
    a = src.index("def _fw_ver():")
    b = src.index("def heap_report(", a)
    ns2 = {"sys": sys}
    exec(compile(src[a:b], "park_app.py", "exec"), ns2)
    v = ns2["_fw_ver"]()
    assert isinstance(v, str) and v and len(v) <= 64, v
    print("   no uos -> falls back to sys.version: %r" % v)
    # 2026-09-14: with the official Lite firmware the banner read just "3.4.0" - that is
    # this port's whole sys.version, and it cannot tell v1.0.4 from v1.0.5-4 or FULL from
    # Lite.  os.uname() is where the build string lives, so it must be preferred.
    stub = types.SimpleNamespace()
    # MicroPython's os.uname() is a namedtuple with .release/.version (what the board
    # printed: "1.0.7; v1.0.5-4-g42c54b5 on 2024-04-11"); a plain tuple must also work.
    UName = collections.namedtuple("uname_result",
                                   "sysname nodename release version machine")
    stub.uname = lambda: UName("k210", "k210", "1.0.7",
                               "v1.0.5-4-g42c54b5 on 2024-04-11",
                               "CanMV_Board with kendryte-k210")
    ns3 = {"sys": sys, "uos": stub}
    exec(compile(src[a:b], "park_app.py", "exec"), ns3)
    w = ns3["_fw_ver"]()
    assert w == "1.0.7; v1.0.5-4-g42c54b5 on 2024-04-11", w
    stub.uname = lambda: ("k210", "k210", "1.0.5-4-g42c54b5",
                          "v1.0.5-4-g42c54b5 on 2024-04-11", "CanMV_Board")
    assert "1.0.5-4" in ns3["_fw_ver"]() and len(ns3["_fw_ver"]()) <= 64
    print("   with uos.uname() -> %r" % w)

    print("\nALL MEM-BUDGET CHECKS PASSED")


main()
