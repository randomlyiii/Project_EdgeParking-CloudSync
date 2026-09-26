# fw_probe.py - ONE cold boot, everything needed to judge the firmware now on the
# board.  Pure ASCII by design.
#
# WHY: the plate demo needs three files resident in the K210 *system* heap
#     lp_detect.kmodel 460456 + lp_recog.kmodel 697512 + lp_weight.bin 1498500
#     = 2656468 B, plus a camera frame.
# K210 firmware carves a GC heap (MicroPython objects, the application's bytecode)
# and a system heap (images / AI / models) out of ONE pool: the port mallocs
# gc_heap_size at boot (maixpy_main.c).  So the number that decides everything is the
# SUM sys_free + gc_heap.  The vendor's plate demo says it needs the LITE CanMV build;
# the board runs the vendor's FULL build (2.07 MB) instead of the 1.55 MB Lite one,
# and K210 has no external DRAM.  Measure it, do not guess it.
#
# HOW TO RUN (this board cannot create files in /flash and its serial terminal
# cannot execute commands):
#   1. CanMV IDE -> open this file -> "Save file to device"  (lands as /flash/main.py)
#   2. POWER-CYCLE the board (unplug / replug).  NEVER use IDE "Run": that is a soft
#      reset, it does NOT give back the KPU memory, and every number below is wrong.
#   3. send the whole serial log back.
#   4. re-save the real k210_fw/main.py (launcher) + k210_fw/park_app.py afterwards
#      (gc_restore.py first if the saved GC heap is too small for park_app.py to
#      compile).
#
# Experiment: run this on the current firmware (baseline), flash the Lite build, run
# it again, compare the [BUD] pool line.  Nothing else changes.
import gc
import sys

# ---- the three model files, bytes (exact - from the vendor plate demo) --------
MODEL_DET = 460456
MODEL_RECOG = 697512
MODEL_WEIGHT = 1498500
MODELS_ALL = (MODEL_DET, MODEL_RECOG, MODEL_WEIGHT)
MODELS_CORE = (MODEL_RECOG, MODEL_WEIGHT)     # what park_app.py loads first

# ---- camera / LCD costs measured on this board (AGENTS.md section 8) ----------
CAM_QVGA = 2 * 320 * 240 * 2        # 307200 B: QVGA RGB565, double buffered
CAM_QQVGA = 2 * 160 * 120 * 2       # 76800 B: QQVGA RGB565, double buffered
LCD_PANEL = 155648                  # the on-board LCD panel buffer
SLACK = 512 * 1024                  # the same fixed margin park_app.py's _mem_budget uses
COLD_BOOT_MIN_FREE = 1800 * 1024    # below this it was a SOFT reset, not a cold boot

# Compiled OUT of the Lite build -> their presence is a board-side Full/Lite marker.
LITE_DROPPED = ("video", "ulab", "nes", "touchscreen", "yolo")

GCH = -1
SYSF = -1


def report_identity():
    print("=" * 66)
    print("[FW ] sys.version  :", sys.version)
    print("[FW ] sys.platform :", getattr(sys, "platform", "?"))
    print("[FW ] sys.implement:", getattr(sys, "implementation", "?"))
    try:
        print("[FW ] implementation.version :", sys.implementation.version)
    except Exception as e:
        print("[FW ] implementation.version : %r" % (e,))
    full = []
    for name in LITE_DROPPED:
        try:
            __import__(name)
            print("[FW ] module %-12s PRESENT (FULL build marker)" % name)
            full.append(name)
        except Exception:
            print("[FW ] module %-12s absent  (Lite build marker)" % name)
    print("[FW ] class        : %s"
          % ("FULL (vendor standard image)" if full else "LITE"))


def read_heap():
    """Fill GCH/SYSF from maix.utils.  Returns the source name ('' if absent)."""
    global GCH, SYSF
    u = None
    src = ""
    for name in ("maix", "Maix"):
        try:
            cand = getattr(__import__(name), "utils", None)
            if cand is not None and hasattr(cand, "heap_free"):
                u, src = cand, name + ".utils"
                break
        except Exception:
            pass
    if u is None:
        print("[MEM] no maix.utils with heap_free -> heap numbers unavailable")
        print("[MEM] (park_app.py tries lowercase 'maix' first; 'Maix' does not exist here)")
        return ""
    try:
        SYSF = u.heap_free()
    except Exception as e:
        print("[MEM] heap_free() failed: %r" % (e,))
    try:
        GCH = u.gc_heap_size()
    except Exception as e:
        print("[MEM] gc_heap_size() failed: %r" % (e,))
    try:
        gcf = gc.mem_free()
    except Exception:
        gcf = -1
    print("[MEM] source       : %s" % src)
    print("[MEM] sys_free     : %d" % SYSF)
    print("[MEM] gc_heap_size : %d" % GCH)
    print("[MEM] gc_free      : %d" % gcf)
    if GCH > 0 and gcf >= 0:
        print("[MEM] gc used      : %d B resident (the compile-time syntax tree is"
              " NOT visible here)" % (GCH - gcf))
    if SYSF >= 0 and GCH > 0:
        print("[MEM] pool (sys+gc): %d B" % (SYSF + GCH))
    if 0 <= SYSF < COLD_BOOT_MIN_FREE:
        print("[MEM] !! sys_free is far below a cold boot (%d) -> SOFT reset (IDE Run"
              " / Ctrl-D)." % COLD_BOOT_MIN_FREE)
        print("[MEM] !! The previous run still owns the KPU memory. POWER-CYCLE, then"
              " run this probe again; the numbers below are meaningless.")
    return src


def report_budget():
    """The decision line: can this firmware hold models + camera?"""
    print("-" * 66)
    if SYSF < 0 or GCH < 0:
        print("[BUD] no heap numbers -> cannot judge (see [MEM] above)")
        return
    total = SYSF + GCH
    print("[BUD] pool = sys_free %d + gc_heap %d = %d" % (SYSF, GCH, total))
    cases = (("core models (recog+weight)", MODELS_CORE, 0, 0),
             ("core models + QVGA camera", MODELS_CORE, CAM_QVGA, 0),
             ("all 3 models + QVGA camera", MODELS_ALL, CAM_QVGA, 0),
             ("all 3 + QVGA cam + LCD", MODELS_ALL, CAM_QVGA, LCD_PANEL),
             ("all 3 + QQVGA camera", MODELS_ALL, CAM_QQVGA, 0))
    for label, sizes, cam, lcd in cases:
        need = sum(sizes) + cam + lcd + SLACK
        print("[BUD] %-27s need %7d  %s" % (label, need,
              "OK  (spare %d)" % (total - need) if total >= need
              else "SHORT by %d" % (need - total)))
    print("[BUD] NOTE: file size is first-order - the board measured lp_detect.kmodel"
          " 460456 B costing ~501 KB resident (1.09x).")
    print("[BUD] NOTE: 'core + QVGA' is the case that matters: LCD_PREVIEW=0 means the"
          " panel buffer is never allocated.")


def report_kpu_api():
    print("-" * 66)
    try:
        from maix import KPU
        k = KPU()
        print("[KPU] from maix import KPU -> OK")
    except Exception as e:
        print("[KPU] KPU() failed: %r" % (e,))
        return
    old = ("load_kmodel", "init_yolo2", "run_with_output", "regionlayer_yolo2",
           "lp_recog_load_weight_data", "lp_recog", "pix_to_ai")
    new = ("get_outputs", "Yolo2", "Lpr", "deinit")
    miss = []
    for a in old + new:
        has = hasattr(k, a)
        print("[KPU]   %-26s %s" % (a, has))
        if a in old and not has:
            miss.append(a)
    print("[KPU] plate API  : %s"
          % ("COMPLETE -> the plate demo can run" if not miss else "MISSING %s" % miss))
    print("[KPU] old API %d/%d ; new-style API present: %s"
          % (len(old) - len(miss), len(old),
             [a for a in new if hasattr(k, a)] or "none"))
    try:
        del k
        gc.collect()
    except Exception:
        pass


def report_vfs():
    print("-" * 66)
    try:
        import uos
    except Exception as e:
        print("[VFS] import uos failed: %r" % (e,))
        return
    print("[VFS] " + " ".join("%s=%s" % (f, hasattr(uos, f)) for f in
          ("mount", "umount", "register_vfs", "statvfs", "ilistdir")))
    for path in ("/", "/sd", "/sd/KPU", "/flash"):
        try:
            print("[VFS] listdir(%-9s) -> %s" % (path, uos.listdir(path)[:8]))
        except Exception as e:
            print("[VFS] listdir(%-9s) -> %r" % (path, e))
    print("[VFS] model files (want det=%d recog=%d weight=%d):"
          % (MODEL_DET, MODEL_RECOG, MODEL_WEIGHT))
    for tail, want in (("lp_detect.kmodel", MODEL_DET),
                       ("lp_recog.kmodel", MODEL_RECOG),
                       ("lp_weight.bin", MODEL_WEIGHT)):
        got, where = None, ""
        for d in ("/sd/KPU", "/sd", "/flash/KPU", "/flash"):
            try:
                got, where = uos.stat(d + "/" + tail)[6], d
                break
            except Exception:
                continue
        if got is None:
            print("[VFS]   %-18s NOT FOUND" % tail)
        else:
            print("[VFS]   %-18s %d in %s %s" % (tail, got, where,
                  "== vendor copy" if got == want
                  else "!! differs from the vendor copy %d" % want))


def main():
    report_identity()
    read_heap()
    report_budget()
    report_kpu_api()
    report_vfs()
    print("=" * 66)
    print("[END] send ALL of the above back")
    print("[NEXT] 1) baseline on the current firmware; 2) flash the Lite build;"
          " 3) run this again and compare the [BUD] pool line.")


main()
