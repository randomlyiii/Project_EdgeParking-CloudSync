# -*- coding: utf-8 -*-
"""Negative tests for k210_fw/tools/build_main.py's safety guard.

The guard exists because two real bugs each cost a board round-trip:
  1. park_app.py had no `import machine` -> NameError when the inlined driver built the
     SPI object, so /sd never mounted.
  2. the constants collector regex started with [A-Z], so `_IOCTL_INIT` & friends were
     not inlined -> NameError inside BlockDev.ioctl, mount still failed.

Both must be REFUSED by the builder now.  Run:  python k210_fw/tools/test_build_main.py
"""
import os
import re
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILDER = os.path.join(REPO, "tools", "build_main.py")
MAIN = os.path.join(REPO, "park_app.py")
DRIVER = os.path.join(REPO, "tools", "sd_spi_fat.py")
TMP = tempfile.gettempdir()


def variant(name, driver_path, main_path):
    """A copy of the builder pointed at alternate driver/main paths."""
    src = open(BUILDER, encoding="utf-8").read()
    a = 'DRIVER = os.path.join(REPO, "tools", "sd_spi_fat.py")'
    b = 'MAIN = os.path.join(REPO, "park_app.py")'
    assert a in src and b in src, "builder path constants changed; update this test"
    src = src.replace(a, "DRIVER = %r" % driver_path).replace(
        b, "MAIN = %r" % main_path)
    p = os.path.join(TMP, name)
    open(p, "w", encoding="utf-8", newline="\n").write(src)
    return p


def run(builder):
    r = subprocess.run([sys.executable, "-X", "utf8", builder, "--check"],
                       capture_output=True, text=True)
    tail = [l for l in (r.stderr + r.stdout).strip().split("\n") if l.strip()]
    return r.returncode, (tail[-1] if tail else "")


def main():
    main_src = open(MAIN, encoding="utf-8").read()
    drv_src = open(DRIVER, encoding="utf-8").read()

    print("== 0. the real files must pass ==")
    code, msg = run(BUILDER)
    assert code == 0, (code, msg)
    print("   ok:", msg)

    print("== A. driver reads a constant it no longer defines -> must be refused ==")
    # (deleting just the definition is the exact mistake the old regex made: the name
    #  is still READ by BlockDev.ioctl, so the board would raise NameError)
    bad_drv = os.path.join(TMP, "sd_spi_fat_noioctl.py")
    kept = [l for l in drv_src.split("\n") if not l.startswith("_IOCTL_INIT")]
    assert len(kept) < len(drv_src.split("\n")), "_IOCTL_INIT definition not found"
    open(bad_drv, "w", encoding="utf-8", newline="\n").write("\n".join(kept))
    code, msg = run(variant("bm_a.py", bad_drv, MAIN))
    assert code != 0, "builder accepted a driver with a missing constant"
    assert "_IOCTL_INIT" in msg, msg
    print("   refused:", msg)

    print("== B. park_app.py loses 'import machine' -> must be refused ==")
    bad_main = os.path.join(TMP, "app_nomachine.py")
    i = main_src.index("import machine")
    open(bad_main, "w", encoding="utf-8", newline="\n").write(
        main_src[:i] + "# " + main_src[i:])
    code, msg = run(variant("bm_b.py", DRIVER, bad_main))
    assert code != 0, "builder accepted park_app.py without import machine"
    assert "machine" in msg, msg
    print("   refused:", msg)

    print("== D. splicing is IDEMPOTENT (a rebuild must not grow park_app.py) ==")
    # It used to append one blank line per build, so `--check` reported a phantom
    # "107706 -> 107707" diff forever and nobody could tell a real drift from the noise.
    import importlib.util
    spec = importlib.util.spec_from_file_location("bm_idem", BUILDER)
    mod = importlib.util.module_from_spec(spec)
    saved_argv = sys.argv
    sys.argv = ["build_main.py", "--check"]          # stop main() from writing
    try:
        spec.loader.exec_module(mod)
    except SystemExit:
        pass
    finally:
        sys.argv = saved_argv
    once = mod.splice(main_src.replace("\r\n", "\n"), mod.build_driver_text())
    twice = mod.splice(once, mod.build_driver_text())
    assert once == twice, "splice() is not idempotent: %d -> %d bytes" % (
        len(once.encode()), len(twice.encode()))
    # ACCEPTED DEVIATION (2026-09-17, user's call: leave it alone for now).
    # The inlined copy of the driver still says BAUD_FAST = 400000 while
    # tools/sd_spi_fat.py says 200000 - and 400kHz is the value we recorded as
    # "2 of 4 boots dead" (same hang signature as 200kHz: the second file open
    # never returns).  It is harmless ONLY because the same inlined copy pins
    # BAUD_TRY = 0, so the fast clock is never attempted.  So the allowance is
    # conditional on that, and the condition is checked HERE: flip BAUD_TRY to 1
    # in the app and this test fails instead of the board.
    # Why the deviation was invisible for so long: 400000 -> 200000 keeps the byte
    # count, and `build_main.py --check` only PRINTS "137441 -> 137441", so the
    # size looked idempotent while the content did not.
    app_norm = main_src.replace("\r\n", "\n")
    assert app_norm.count("BAUD_FAST = 400000") == 1, \
        "the accepted deviation changed shape; re-check the inlined driver"
    assert "BAUD_TRY = 0" in app_norm, \
        ("inlined driver no longer pins BAUD_TRY = 0 - the accepted BAUD_FAST=400000"
         " deviation is now live (400kHz hangs this board, see AGENTS.md)")
    allowed = re.sub(r"^BAUD_FAST = 400000", "BAUD_FAST = 200000", app_norm,
                     flags=re.M)
    assert once == allowed, \
        "the checked-in park_app.py is not what the builder produces (stale splice)"
    print("   NOTE accepted deviation: inlined BAUD_FAST=400000 (driver source says"
          " 200000), dormant because the inlined copy pins BAUD_TRY=0")
    assert once.count("# ==== END INLINE SD DRIVER") == 1
    print("   splice(splice(x)) == splice(x) == park_app.py (%d bytes)"
          % len(once.encode()))

    print("== E. the real park_app.py still carries what the driver needs ==")
    for needle in ("import machine", "import uos", "class SDSPI", "class Fat32",
                   "_IOCTL_INIT", "_IOCTL_BLOCK_SIZE", "BAUD =", "TOKEN_MS",
                   "BAUD_TRY", "BAUD_FAST"):
        assert needle in main_src, needle
    print("   imports + constants all present")

    print("\nALL BUILD-GUARD CHECKS PASSED")


main()
