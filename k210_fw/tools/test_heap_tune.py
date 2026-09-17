# -*- coding: utf-8 -*-
"""Host test for park_app.py's GC/system heap split tuner (_apply_gc_heap /
_gc_floor / auto_tune_gc_heap): the K210 memory-pool fix.

K210 has two pools that trade off against each other -- the GC heap (MicroPython
objects, INCLUDING park_app.py's own bytecode) and the system heap (images / AI / LCD /
models / weights).  lp_detect 460KB + lp_recog 697KB + lp_weight 1498500B + the
389KB camera buffer all come out of the system heap, so when the split is left at
the firmware default the models cannot be loaded.

Board facts this file encodes (2026-09-13, cold-boot logs):
  * lowercase ``maix`` only (``import Maix`` -> ImportError);
  * ``/flash`` cannot create files -> the old ``/flash/.sdtuned`` marker was dead code;
  * THIS BOARD: ``sys_free=2514944 gc_heap=475136`` -> the pool total is only 2990080
    (2.85MB), so "models+weight+camera" (2660KB) leaves just 260KB for the GC heap;
  * ``gc_heap_size(N)`` did **not** take effect live in that log (status=deferred)
    -> a deferred boot must still run the app (preview + red on-screen reason),
    NEVER return to a bare REPL that looks like a dead board;
  * the old build shrank the GC heap to 65536/49152 while hunting for weight memory;
    at that size park_app.py cannot even compile -> bare "MemoryError:" at boot.

So the contract now is READ-ONLY + RAISE-ONLY, and it rests on the 2026-09-13 findings:
  * the GC heap and the system heap are carved out of ONE pool at boot
    (``maixpy_main.c``: ``gc_heap = malloc(gc_heap_size)``), so moving the split moves
    memory 1:1 -- and ``gc_heap_size(N)`` only writes freq.conf, so it needs a REBOOT
    before it does anything (``Maix_utils.c``).  "deferred" is by design, not a bug.
    (The older note "sys_free was byte-for-byte identical across two cold boots, so the
    two pools do not trade" was a mis-read: that write had not taken effect yet.)
  * shrinking it can make the board unbootable: after 253952 was written the boot
    printed only "MemoryError: memory allocation failed, allocating 160 bytes",
    because COMPILING a ~104KB park_app.py needs far more than its resident 108KB.
    Known-good: 404 / 464 / 512KB.  Known-bad: 248KB.  The real floor is UNMEASURED.
So: report the numbers, detect soft resets, and raise only what is below
HEAP_TUNE_GC_MIN back to the firmware default.  Shrinking by hand is allowed as an
experiment, but it must be measured on the board first (k210_fw/fw_probe.py reports
the pool total that the measurement has to beat) - never done unattended.

2026-09-17: a 768KB target was tried here for one day and REVERTED - the 768KB cold boot
died in ~2 s with a bare TypeError inside the ``[stat]`` format (GC heap corrupted) while
the 512KB boot ran 12.2 min / 233 recognitions cleanly.  768KB also costs the system heap
256KB+ (cam_init sys_free 1101824 -> 839680).  Do not raise this again until the heap
corruption has an identified writer.
"""
import os
import re
import sys
import types

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(REPO, "park_app.py")

K = 1024
BOARD_FREE = 2514944              # 2456KB, from the real cold-boot log
BOARD_GC = 475136                 # 464KB
BOARD_TOTAL = BOARD_FREE + BOARD_GC


def extract():
    """Pull the maix.utils lookup + heap helpers + the tuner out of park_app.py."""
    src = open(SRC, encoding="utf-8").read()
    a = src.index("def _maix_utils():")
    b = src.index("def heap_report(", a)
    helpers = src[a:b]
    c = src.index("HEAP_TUNE = 1")
    d = src.index("def main():", c)
    return helpers + "\n" + src[c:d]


def build(free, gc_size=512 * K, used=64 * K, live=True, have_utils=True,
          tune_on=True, setter_raises=False, heap_free_readable=True):
    """Wire a fake maix/gc and return the tuner namespace plus its logs.

    ``used``    models how much GC heap park_app.py itself has eaten (code + globals) --
                it is what the self-measuring floor is derived from.
    ``live``    True: gc_heap_size(N) moves the system heap right away (as the older
                probe session showed); False: the call succeeds but nothing moves
                (what the 2026-09-13 cold boot actually reported).
    """
    logs = []
    calls = {"gc_heap_size": []}
    board = {"free": free, "gc": gc_size, "gc_free": max(0, gc_size - used)}

    ns = {"print": lambda m: logs.append(m)}
    exec(compile(extract(), "park_app.py", "exec"), ns)
    ns["HEAP_TUNE"] = 1 if tune_on else 0
    # module globals park_app.py defines that the tuner writes (extract() only takes the
    # tuner itself, so seed them the same way park_app.py's head does)
    ns["_KPU_SKIP_REASON"] = ""

    class FakeGC(object):
        def mem_free(self):
            return board["gc_free"]

        def collect(self):
            return 0
    ns["gc"] = FakeGC()

    utils = types.SimpleNamespace()
    utils.heap_free = (lambda: board["free"]) if heap_free_readable else \
        (lambda: (_ for _ in ()).throw(OSError(5)))

    def gc_heap_size(n=None):
        if n is None:
            return board["gc"]
        calls["gc_heap_size"].append(n)
        if setter_raises:
            raise OSError(1)
        if live:
            board["free"] += board["gc"] - n
            # the GC heap is re-carved: keep the same amount of live data in it
            board["gc_free"] = max(0, n - (gc_size - board["gc_free"]))
        board["gc"] = n
        return n

    if not have_utils:
        sys.modules.pop("maix", None)      # no maix at all -> _maix_utils() gives up
        sys.modules.pop("Maix", None)
    else:
        utils.gc_heap_size = gc_heap_size
        maix = types.ModuleType("maix")
        maix.utils = utils
        sys.modules["maix"] = maix
        sys.modules.pop("Maix", None)      # this firmware has no uppercase Maix

    ns["_calls"] = calls
    ns["_logs"] = logs
    ns["_board"] = board
    return ns


def main():
    print("== 1. room enough already -> no touch, keep booting ==")
    ns = build(free=0)
    need = ns["HEAP_TUNE_NEED"]
    ns = build(free=need + 64 * K)
    assert ns["auto_tune_gc_heap"]() is False
    assert not ns["_calls"]["gc_heap_size"], ns["_calls"]
    assert any("NOT touching it" in m for m in ns["_logs"]), ns["_logs"]
    print("   free=%dKB >= need=%dKB -> %s"
          % (ns["_board"]["free"] // K, need // K, ns["_logs"][-1]))

    print("== 2. a sane GC heap is NOT touched (a bad saved value bricks the boot) ==")
    ns = build(free=BOARD_FREE, gc_size=BOARD_GC)
    assert ns["auto_tune_gc_heap"]() is False
    assert not ns["_calls"]["gc_heap_size"], "must never shrink the GC heap"
    assert ns["_logs"][0].startswith("[MEM] boot: sys_free=2514944 gc_heap=475136")
    assert any("park_app.py holds" in m for m in ns["_logs"]), ns["_logs"]
    assert any("NOT touching it" in m for m in ns["_logs"]), ns["_logs"]
    assert any("short by" in m for m in ns["_logs"]), ns["_logs"]
    print("   gc=464KB -> '%s'" % ns["_logs"][-1])


    print("== 3. THE 2026-09-13 BRICK: 248KB was written and park_app.py stopped booting ==")
    # Cold boot showed only: MemoryError: memory allocation failed, allocating 160 bytes
    # -> the GC heap was too small to COMPILE park_app.py.  The lesson: never shrink, and
    # raise anything below the target back to it (this case is also below the 384KB
    # compile floor, which the message names explicitly).
    ns = build(free=BOARD_FREE, gc_size=248 * K, used=100 * K, live=False)
    assert ns["auto_tune_gc_heap"]() is True, "a raise that is not live needs a cold boot"
    assert ns["_calls"]["gc_heap_size"] == [ns["HEAP_TUNE_GC_RESTORE"]], ns["_calls"]
    assert any("may fail to COMPILE" in m for m in ns["_logs"]), ns["_logs"]
    assert any("POWER-CYCLE" in m for m in ns["_logs"]), ns["_logs"]
    print("   gc=248KB -> raised to %dKB ('%s')"
          % (ns["HEAP_TUNE_GC_RESTORE"] // K, ns["_logs"][-1]))

    print("== 3b. the raise can also take effect immediately ==")
    ns = build(free=BOARD_FREE, gc_size=248 * K, live=True)
    assert ns["auto_tune_gc_heap"]() is False, "a live raise keeps booting"
    assert ns["_board"]["gc"] == ns["HEAP_TUNE_GC_RESTORE"], ns["_board"]
    assert any("GC heap raised" in m for m in ns["_logs"]), ns["_logs"]
    print("   gc=248KB -> %dKB live, booting" % (ns["_board"]["gc"] // K))

    print("== 3c. exactly at the minimum -> left alone ==")
    ns = build(free=BOARD_FREE, gc_size=ns["HEAP_TUNE_GC_MIN"])
    assert ns["auto_tune_gc_heap"]() is False
    assert not ns["_calls"]["gc_heap_size"], ns["_calls"]
    print("   gc=%dKB (== min) -> not touched" % (ns["HEAP_TUNE_GC_MIN"] // K))

    print("== 4. reports the real numbers, whether or not they fit ==")
    ns = build(free=need + 64 * K, gc_size=BOARD_GC)
    assert ns["auto_tune_gc_heap"]() is False
    assert any("-> OK" in m for m in ns["_logs"]), ns["_logs"]
    ns = build(free=BOARD_FREE, gc_size=BOARD_GC)
    ns["auto_tune_gc_heap"]()
    assert any("short by" in m for m in ns["_logs"]), ns["_logs"]
    print("   %s | %s" % (ns["_logs"][1], ns["_logs"][2]))

    print("== 6. the soft-boot detector must not fire on a real cold boot ==")
    for free in (BOARD_FREE, 2564096, COLD_BOOT_MIN := ns["COLD_BOOT_MIN_FREE"]):
        ns = build(free=free, gc_size=BOARD_GC)
        ns["auto_tune_gc_heap"]()
        assert not ns["_KPU_SKIP_REASON"], (free, ns["_logs"])
    print("   cold-boot sized readings all pass")

    print("== 7. degraded inputs do not break the boot ==")
    ns = build(free=100, have_utils=False)
    assert ns["auto_tune_gc_heap"]() is False
    assert any("maix.utils not found" in m for m in ns["_logs"]), ns["_logs"]

    ns = build(free=100, heap_free_readable=False)
    assert ns["auto_tune_gc_heap"]() is False
    assert any("heap_free unavailable" in m for m in ns["_logs"]), ns["_logs"]

    ns = build(free=BOARD_FREE, gc_size=248 * K, setter_raises=True)
    assert ns["auto_tune_gc_heap"]() is False, \
        "a setter that raises changed nothing -> boot anyway, do not ask for a reboot"
    assert any("could not raise the GC heap" in m for m in ns["_logs"]), ns["_logs"]

    ns = build(free=100, tune_on=False)
    assert ns["auto_tune_gc_heap"]() is False and ns["_logs"] == []
    print("   no utils / unreadable heap / setter raises / switch off: all handled")

    print("== 8. _apply_gc_heap reports live vs deferred vs failed ==")
    for live, want in ((True, "live"), (False, "deferred")):
        ns = build(free=2 * K * K, gc_size=512 * K, live=live)
        status, before, after = ns["_apply_gc_heap"](256 * K)
        assert status == want, (live, status, ns["_logs"])
        assert before == 2 * K * K, before
    # raising the heap moves sys_free the OTHER way - still "live", not "deferred"
    ns = build(free=2 * K * K, gc_size=48 * K)
    status, before, after = ns["_apply_gc_heap"](192 * K)
    assert status == "live", (status, ns["_logs"])
    assert after < before, "raising the GC heap must shrink the system heap"
    ns = build(free=2 * K * K, gc_size=512 * K, setter_raises=True)
    status, before, after = ns["_apply_gc_heap"](256 * K)
    assert status == "failed" and after == before, (status, before, after)
    print("   live / deferred / failed all distinguishable (both directions)")

    print("== 9. no dead board, no marker file, no stale assumptions ==")
    src = open(SRC, encoding="utf-8").read()
    # comments may still explain why the marker was dropped - only code matters here
    code = "\n".join(l for l in src.split("\n")
                     if not l.lstrip().startswith("#"))
    assert "HEAP_TUNE_MARK" not in code, \
        "/flash cannot create files: a marker-file guard can never work here"
    assert ".sdtuned" not in code, "that marker is dead code on this board"
    assert "auto reset" not in src and "reboot manually" not in src, \
        "gc_heap_size(N) does not reset the board on this firmware"
    assert 'for name in ("maix", "Maix")' in src, \
        "must try lowercase maix first (uppercase Maix does not exist here)"
    assert "import Maix\n" not in src, "bare 'import Maix' is the bug we fixed"
    # main() must NEVER return early: a bare REPL looks exactly like a dead board
    assert not re.search(r"if auto_tune_gc_heap\(\):\s*\n\s*return", src), \
        "main() must keep running the app even when the split is deferred"
    i_tune = src.index("if auto_tune_gc_heap():")
    i_app = src.index("_APP = App()", i_tune)
    assert i_tune < i_app, "tuning must happen before the app starts"
    # once the stub is proven, the tuner must short-circuit for good
    # the tuner must only ever RAISE the GC heap: the single write site is the
    # "below HEAP_TUNE_GC_MIN" branch, and its target is the restore value
    i_fn = src.index("def auto_tune_gc_heap():")
    i_end = src.index("def main():", i_fn)
    fn = src[i_fn:i_end]
    assert fn.count("_apply_gc_heap(") == 1, "only one write site is allowed"
    assert "_apply_gc_heap(HEAP_TUNE_GC_RESTORE)" in fn, "the write must be a restore"
    assert "HEAP_TUNE_FLOOR" not in src and "HEAP_TUNE_MARGIN" not in src, \
        "the old measured-shrink floor must be gone"
    assert "HEAP_TUNE_GC_MIN" in src
    # kpu_load must still honour the skip flag if anything ever sets it
    i_load = src.index("def kpu_load():")
    i_body = src.index("_KPU_SKIP_REASON", i_load)
    assert i_body > i_load, "kpu_load must bail out on _KPU_SKIP_REASON"
    # ... and the retry loop must be bounded + must not retry hopeless failures
    assert "KPU_MAX_TRIES" in src, "unbounded retries leaked a KPU instance every 10s"
    assert "_KPU_GIVE_UP" in src and "def _kpu_fail(err, permanent=False)" in src
    assert 'permanent=True' in src, "a low-memory failure must not be retried"
    print("   tuner runs first, gives up permanently once proven, retries are bounded")

    print("== 10. the recovery script (k210_fw/gc_restore.py) stays usable ==")
    # It is the only way back when the saved split is so small that park_app.py cannot
    # even compile (bare "MemoryError:" at boot, no [MEM] line).  It must be tiny,
    # pure ASCII, and must never restore a split below park_app.py's own absolute floor.
    rp = os.path.join(REPO, "gc_restore.py")
    rsrc = open(rp, encoding="utf-8").read()
    assert all(ord(c) < 128 for c in rsrc), "board-side file: ASCII only"
    assert len(rsrc.encode()) < 4096, "must compile even on a broken heap"
    compile(rsrc, rp, "exec")
    assert re.search(r"^TARGET = ([0-9]+)", rsrc, re.M).group(1) == "524288", \
        "the recovery script must restore the firmware default, nothing smaller"
    assert "FALLBACK" not in rsrc, "no arithmetic: never pick a smaller value"
    assert "gc_heap_size(TARGET)" in rsrc and "POWER-CYCLE" in rsrc
    print("   %d B ASCII; restores gc_heap=512KB unconditionally"
          % (len(rsrc.encode()),))

    print("\nALL HEAP-TUNE CHECKS PASSED")


if __name__ == "__main__":
    main()
