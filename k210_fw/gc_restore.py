# gc_restore.py - one-shot recovery for a K210 whose saved GC heap got shrunk
# too far.  PURE ASCII ON PURPOSE (this file is saved to the board).
#
# SYMPTOM it fixes
#     Cold boot prints nothing but a bare "MemoryError:" and then the MicroPython
#     banner + ">>>".  No [BOOT]/[MEM] line at all.
#     Cause: gc_heap_size(N) is PERSISTED in flash.  An older build shrank the GC
#     heap to 65536/49152 while hunting for memory for lp_weight.bin, and at that
#     size MicroPython cannot even COMPILE the application any more - so the
#     program dies before its first print (this is exactly what happened on
#     2026-09-13).
#     NOTE (2026-09-16): /flash/main.py is now the 3.7KB launcher, which checks the
#     heap itself and restores 512KB when it is too small - so this file is the
#     LAST RESORT (no launcher on the board, or a heap so broken that even 3.7KB
#     will not compile).
#
# HOW TO USE (same one-file procedure as always: no REPL commands)
#     1. CanMV IDE: open THIS file, press "save file to device" (it becomes
#        /flash/main.py; it is tiny, so it compiles even on the broken heap).
#     2. POWER-CYCLE the board (cold boot, not reset) and read the serial log.
#     3. Ide: open k210_fw/park_app.py again save it as park_app.py, plus
#        k210_fw/main.py (the launcher) as main.py, then cold boot again.
#
# EXPECTED SERIAL OUTPUT
#     [GC] before: gc_heap=... sys_free=...
#     [GC] set gc_heap=524288 -> after: gc_heap=524288 sys_free=...
#     [GC] POWER-CYCLE, then save k210_fw/park_app.py + main.py again
# If "after" still shows the old value, the setting only applies on the next
# power-up - that is fine, just cold boot once more and check the first line.

import gc

TARGET = 524288                 # the firmware's own default: 512KB.  RESTORE ONLY:
                                # never compute anything smaller - that is how we got
                                # here (253952 was written, and the application
                                # stopped compiling).  Raising the heap does NOT shrink
                                # the system heap on this firmware, so this is free.


def utils():
    """maix.utils on this firmware (lowercase maix; MaixPy's uppercase does not exist)."""
    for name in ("maix", "Maix"):
        try:
            u = getattr(__import__(name), "utils", None)
            if u is not None and hasattr(u, "gc_heap_size"):
                return u
        except Exception:
            pass
    return None


def show(u, tag):
    try:
        print("[GC] %s: gc_heap=%d sys_free=%d gc_free=%d"
              % (tag, u.gc_heap_size(), u.heap_free(), gc.mem_free()))
    except Exception as e:
        print("[GC] %s: cannot read the heap: %r" % (tag, e))


u = utils()
if u is None:
    print("[GC] no maix.utils on this firmware - nothing I can do here")
else:
    show(u, "before")
    try:
        u.gc_heap_size(TARGET)
        print("[GC] set gc_heap=%d" % TARGET)
    except Exception as e:
        print("[GC] gc_heap_size(%d) failed: %r" % (TARGET, e))
    show(u, "after ")
    print("[GC] POWER-CYCLE (cold boot), then save k210_fw/park_app.py + main.py again")
