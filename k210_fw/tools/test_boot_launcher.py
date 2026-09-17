# -*- coding: utf-8 -*-
"""Host test for how k210_fw/main.py is allowed to look - the boot script.

There are exactly TWO valid deployments, and this file accepts both:

  A. two-stage (2026-09-16):  k210_fw/main.py = tiny launcher (<4096 B),
                              k210_fw/park_app.py = the application
  B. single-file (2026-09-17, user's choice): k210_fw/main.py IS the
                              application, byte-identical to park_app.py

Anything else is refused, and that is the point of the byte-identity check in
case B:

Board fact this file encodes (2026-09-16): the application is ~135KB and
MicroPython must COMPILE it before a single line of it runs.  The compile-time
peak is far above its resident footprint, so when the GC heap has been shrunk the
compiler dies INSIDE the file - the user's cold boot was "it runs, but prints
nothing": no [MEM] line, no MemoryError, no traceback at all.

That is why deployment A exists: the repair must live in a file SMALL ENOUGH TO
COMPILE ON A BROKEN HEAP, i.e. the boot script itself.  Same cap and same reason
as gc_restore.py (< 4096 B).  And gc_heap_size(N) only writes freq.conf - it
applies on the NEXT power-up (Maix_utils.c) - so "repair, print POWER-CYCLE,
stop" is the correct shape and a deferred repair must be LOUD.

Why case B still has a hard check (2026-09-17 board session): the board raised
`NameError: name 'LCD_PREVIEW' isn't defined` for a constant that park_app.py
defines as a plain module-level assignment - and `SCREEN_STATUS` (line 843) and
`C_YELLOW` (line 850) resolved in the very same run while `STATUS_SCALE` (845)
did not.  A module body cannot skip one line between two that ran, so the file
EXECUTED on the board was not the file in this repo (hand-edited variants keep
the line numbers if you comment a line out, which is exactly what the traceback
showed).  Byte-identity is the cheap gate against that class of bug: the file you
flash must be the file the tests looked at.

Run:  python k210_fw/tools/test_boot_launcher.py
"""

import ast
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LAUNCHER = os.path.join(REPO, "main.py")
APP = os.path.join(REPO, "park_app.py")

MAX_LAUNCHER = 4096          # same cap (and same reason) as gc_restore.py in
                            # test_heap_tune.py: it has to compile on a broken heap

_fails = []


def check(what, got, want):
    if got == want:
        print("[pass] %s" % what)
    else:
        print("[FAIL] %s: got %r want %r" % (what, got, want))
        _fails.append(what)


def note(msg):
    print("[note] %s" % msg)


def check_launcher(src, raw):
    """Deployment A: the boot script is the small, dependency-free launcher."""
    print("== 1. the launcher is small, ASCII and compiles ==")
    check("launcher is pure ASCII", all(b < 128 for b in raw), True)
    n = len(raw)
    check("launcher <= %d B (compiles on a broken heap)" % MAX_LAUNCHER,
          n <= MAX_LAUNCHER, True)
    try:
        compile(src, LAUNCHER, "exec")
        ok = True
    except SyntaxError as e:
        ok = repr(e)
    check("launcher compiles", ok, True)
    print("   %d B (%.1f%% of the cap)" % (n, 100.0 * n / MAX_LAUNCHER))

    print()
    print("== 2. it stays a launcher: no app / SD-driver code inlined ==")
    # The whole point is that it compiles when nothing else will.  If any of this
    # appears here, someone has grown the boot script into a second application.
    for needle in ("PROVINCE_ZH", "PROVINCES", "_recog_crop", "readblocks",
                   "0x1021", "class BlockDev", "Fat32", "lp_recog",
                   "read_progress", "_sys_heap_free", "mbr_partitions"):
        check("launcher does not inline %r" % needle, needle in src, False)

    print()
    print("== 3. imports are limited to gc/sys ==")
    tree = ast.parse(src)
    tops = set()
    for node in tree.body:
        if isinstance(node, ast.Import):
            for a in node.names:
                tops.add(a.name.split(".")[0])
        elif isinstance(node, ast.ImportFrom):
            tops.add((node.module or "").split(".")[0])
    check("no heavyweight top-level import", sorted(tops), ["gc", "sys"])

    print()
    print("== 4. repair-then-run shape ==")
    check("names the application path", 'APP = "/flash/park_app.py"' in src, True)
    check("heap floor is 384KB", "GC_MIN = 384 * 1024" in src, True)
    # 2026-09-16 board run: 512KB was NOT enough.  The ROI cut / resize / pix_to_ai
    # buffers (~26KB each) are allocated from THIS heap and MicroPython's GC never
    # compacts, so on 512KB the largest free run settles at ~26KB and recognition dies
    # on whichever allocation comes second.  The launcher must aim higher than the
    # value this board shipped with (the system heap pays: the Lite firmware leaves
    # 1.6MB of it free once the models are loaded).
    check("aims at 768KB, not the old 512KB", "GC_SET = 768 * 1024" in src, True)
    check("reads the heap through maix.utils", "gc_heap_size" in src, True)
    check("never picks a value by arithmetic", "GC_SET -" not in src, True)
    check("both magic dirs are added to sys.path",
          ('"/flash", "/sd"' in src), True)
    check("runs the application through main()", "park_app.main()" in src, True)
    check("tells the user to power-cycle", "POWER-CYCLE" in src, True)
    check("reports a non-sticking write", "did not stick" in src, True)

    # The first line on the wire must be the launcher's own, and it must come out
    # BEFORE the application is imported - that line is the only evidence left
    # when the application dies inside its own compile.
    first_print = src.index('print("[BOOT] launcher:')
    imp = src.index("import park_app")
    check("the heap line is printed before the app is imported",
          first_print < imp, True)

    # Both failure branches must say what to DO, not just what broke.
    check("import failure names the fix",
          "save k210_fw/park_app.py to the device as park_app.py" in src, True)
    check("write failure names the fallback",
          "k210_fw/gc_restore.py" in src, True)

    # SD pre-flight (2026-09-16): the launcher settles the card, and park_app only
    # keeps a BOUNDED retry as a fallback.  Order is the whole point - nothing is up
    # while the launcher retries, whereas by the time park_app boots there is a driver,
    # a mount and a model load behind it.
    sd_imp = src.index("import sd_probe2")
    sd_wait = src.index("sd_probe2.wait()")
    check("launcher pre-flights the SD card", "import sd_probe2" in src, True)
    check("pre-flight runs before the app is imported",
          sd_imp < imp and sd_wait < imp, True)
    check("a missing probe file is not fatal (park_app still tries)",
          "sd_probe2 unavailable" in src and "park_app mounts by itself" in src, True)
    # ...and then the launcher must GIVE THE HEAP BACK (2026-09-16 board run: the probe's
    # SPI object + classes + bytecode otherwise stay alive for the whole session, i.e. a
    # long-lived block inside a heap MicroPython cannot compact - the same heap the
    # application needs ~26KB contiguous runs from).  Release, drop the module, collect.
    sd_rel = src.index("sd_probe2.release()")
    check("the probe's SPI object is released", "sd_probe2.release()" in src, True)
    check("the probe module is dropped", 'del sys.modules["sd_probe2"]' in src, True)
    check("the hand-over happens before the app is imported", sd_rel < imp, True)
    check("the hand-over collects the freed heap",
          "gc.collect()" in src[sd_rel:imp], True)


def check_single_file(src, raw, app_raw):
    """Deployment B: main.py IS the application - and must BE the repo's app."""
    print("== 1. single-file deployment: main.py is the application ==")
    check("main.py is byte-identical to park_app.py", raw == app_raw, True)
    if raw != app_raw:
        note("the file you flash must be the file the tests checked: copy"
             " k210_fw/park_app.py over k210_fw/main.py (or restore the launcher)")
    try:
        compile(src, LAUNCHER, "exec")
        ok = True
    except SyntaxError as e:
        ok = repr(e)
    check("main.py compiles", ok, True)
    check("it still carries the app entry point", "def main():" in src, True)
    check("it still runs under its own __main__ guard",
          'if __name__ == "__main__":' in src, True)
    check("it still carries the SD probe hook",
          "sd_wait_until_mounted" in src, True)
    print()
    # Not a failure - the user chose this deployment - but it must not be invisible:
    # GC_SET/GC_MIN/GC_SET=768KB lived in the launcher, and gc_heap_size() persists in
    # freq.conf, so after a firmware reflash (which wipes /flash) nothing restores
    # 768KB any more; the app's own tuner only aims at HEAP_TUNE_GC_RESTORE.
    note("single-file mode has no pre-compile launcher: the 768KB GC-heap floor is"
         " now only what freq.conf still remembers (the app's own tuner aims at"
         " HEAP_TUNE_GC_RESTORE, see park_app.py). A firmware reflash loses it.")


def main():
    check("k210_fw/main.py (boot script) exists", os.path.isfile(LAUNCHER), True)
    check("k210_fw/park_app.py (application) exists", os.path.isfile(APP), True)
    if _fails:
        print("BOOT-LAUNCHER CHECKS FAILED (%d)" % len(_fails))
        return 1

    raw = open(LAUNCHER, "rb").read()
    src = raw.decode("utf-8")
    app_raw = open(APP, "rb").read()

    if raw == app_raw:
        print("deployment: single-file (main.py == park_app.py)")
        print()
        check_single_file(src, raw, app_raw)
    elif len(raw) <= MAX_LAUNCHER:
        print("deployment: two-stage (main.py = launcher, park_app.py = app)")
        print()
        check_launcher(src, raw)
    else:
        print("[FAIL] k210_fw/main.py is neither the <=%d B launcher nor a copy of"
              " park_app.py (%d B) - the board would run a file no test has seen"
              % (MAX_LAUNCHER, len(raw)))
        _fails.append("main.py identity")

    print()
    print("== 5. the application stays independently bootable (rollback path) ==")
    app = app_raw.decode("utf-8")
    check("app still defines main()", "def main():" in app, True)
    check("app still runs under its own __main__ guard",
          'if __name__ == "__main__":' in app, True)
    check("app keeps its own heap tuner (second line of defence)",
          "auto_tune_gc_heap" in app, True)
    check("app still appends /flash and /sd to sys.path", '"/flash", "/sd"' in app,
          True)

    print()
    if _fails:
        print("BOOT-LAUNCHER CHECKS FAILED (%d)" % len(_fails))
        return 1
    print("ALL BOOT-LAUNCHER CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
