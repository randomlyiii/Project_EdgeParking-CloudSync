# -*- coding: utf-8 -*-
"""CPython simulated test for k210_fw/main.py plate-decode logic (no hardware).

Run:  python k210_recog_test.py
"""
import importlib.util
import sys
import types

sys.dont_write_bytecode = True   # 别在 k210_fw/ 里生成 __pycache__

# ---- stub utime so the K210 module can be imported ----
ut = types.ModuleType("utime")
_t = [0]


def ticks_ms():
    _t[0] += 1
    return _t[0]


ut.ticks_ms = ticks_ms
ut.ticks_diff = lambda a, b: a - b
ut.ticks_add = lambda a, b: a + b
ut.sleep_ms = lambda ms: None
sys.modules["utime"] = ut

# park_app.py now imports machine/uos at module level (the INLINED SPI-FAT driver needs
# machine.SPI and the uos error codes), so stub them too or importing park_app.py fails
# with ModuleNotFoundError on a PC.
_machine = types.ModuleType("machine")


class _SPI:
    def __init__(self, *a, **kw):
        pass

    def write(self, b):
        pass

    def read(self, n):
        return b"\xff" * n


_machine.SPI = _SPI
sys.modules["machine"] = _machine

_uos = types.ModuleType("uos")
for _name, _val in (("ENOENT", 2), ("EPERM", 1), ("EROFS", 30), ("EISDIR", 21),
                    ("ENOTDIR", 20)):
    setattr(_uos, _name, _val)
_uos.listdir = lambda path: (_ for _ in ()).throw(OSError(19))
_uos.stat = lambda path: (_ for _ in ()).throw(OSError(2))
sys.modules["uos"] = _uos

MAIN = r"D:\Projects\Project_EdgeParking-CloudSync\k210_fw\park_app.py"
spec = importlib.util.spec_from_file_location("k210main", MAIN)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

# Sections 1-6 below exercise the DUAL-MODEL path on purpose, so pin it here (park_app.py's
# own default is ROI_MODE=1).  Section 7 turns the fixed-ROI mode on and restores this.
m.ROI_MODE = 0

fails = []


def check(name, got, want):
    ok = got == want
    print("%-46s %s  got=%r want=%r" % (name, "PASS" if ok else "FAIL", got, want))
    if not ok:
        fails.append(name)


# ---- 1. class tables ----
check("PROVINCES count", len(m.PROVINCES), 31)
check("PROVINCE_ZH covers all labels",
      sorted(m.PROVINCE_ZH.keys()) == sorted(m.PROVINCES), True)
check("PROVINCE_ZH[Yue]==Yue-char", m.PROVINCE_ZH["Yue"], "\u7ca4")
check("PROVINCE_ZH[Jing]==Jing-char", m.PROVINCE_ZH["Jing"], "\u4eac")
check("PROVINCE_ZH[Hu]==Hu-char", m.PROVINCE_ZH["Hu"], "\u6caa")
check("ADS count", len(m.ADS), 34)
check("ADS[0]/ADS[1]", (m.ADS[0], m.ADS[1]), ("A", "B"))
check("ADS[24..29] are 0..5", m.ADS[24:30], ["0", "1", "2", "3", "4", "5"])


# ---- 2. fake KPU + fake image ----
def make_rows(prov_idx, char_idxs, nrows):
    rows = []
    r0 = [0.01] * len(m.PROVINCES)
    r0[prov_idx] = 0.97
    rows.append(r0)
    for i in char_idxs:
        r = [0.01] * len(m.ADS)
        r[i] = 0.95
        rows.append(r)
    while len(rows) < nrows:
        rows.append([0.5] * len(m.ADS))
    return rows


class FakeRecog:
    def __init__(self, rows):
        self.rows = rows
        self.calls = 0

    def run_with_output(self, img):
        self.calls += 1

    def lp_recog(self):
        return self.rows


class SeqRecog:
    def __init__(self, seq):
        self.seq = seq
        self.i = 0

    def run_with_output(self, img):
        pass

    def lp_recog(self):
        r = self.seq[min(self.i, len(self.seq) - 1)]
        self.i += 1
        return r


class FakeDet:
    def __init__(self, boxes):
        self.boxes = boxes

    def run_with_output(self, img):
        pass

    def regionlayer_yolo2(self):
        return self.boxes


class FakeImg:
    def __init__(self):
        self.drawn = []

    def cut(self, x, y, w, h):
        return self

    def width(self):
        return 100

    def height(self):
        return 30

    def resize(self, w, h):
        return self

    def replace(self, **kw):
        return self

    def pix_to_ai(self):
        pass

    def draw_rectangle(self, x, y, w, h, color=None):
        self.drawn.append(("rect", x, y, w, h))

    def draw_string(self, x, y, s, color=None, scale=1):
        self.drawn.append(("str", s))


def run_case(name, rows, boxes=((10, 10, 100, 30),)):
    m.INFER_MODE = "A"
    m._KPU_LOADED = True          # skip real kpu_load
    m._KPU_DET = FakeDet(list(boxes))
    m._KPU_RECOG = FakeRecog(rows)
    res = m.recognize_frame(FakeImg())
    print("  -> %s" % {k: res[k] for k in ("plate", "plate_ascii",
                                           "confidence", "det", "error")
                       if k in res})
    return res


# YueB12345: province idx19=Yue; chars B=1, then digits '1'=25..'5'=29
CHAR_YUE_B12345 = [1, 25, 26, 27, 28, 29]
res = run_case("yue-b-12345-8rows", make_rows(19, CHAR_YUE_B12345, 8))
check("plate is Chinese plate", res["plate"], "\u7ca4B12345")
check("plate_ascii is pinyin", res["plate_ascii"], "YueB12345")
check("error is None", res["error"], None)
check("det count == 1", res["det"], 1)
exp_box = m._extend_box(10, 10, 100, 30, m.KPU_DET_EXTEND)
check("boxes returned (extended coords)", res.get("boxes"), [exp_box])
check("ms field is int", isinstance(res.get("ms"), int), True)

# 7-row output (province + 6 chars) must decode the same
res = run_case("7rows", make_rows(19, CHAR_YUE_B12345, 7))
check("7-row output plate", res["plate"], "\u7ca4B12345")

# JingA88888: province idx12=Jing; A=0, '8'=32
res = run_case("jing-a-88888", make_rows(12, [0, 32, 32, 32, 32, 32], 8))
check("plate JingA88888", res["plate"], "\u4eacA88888")

# ---- 3. overlay: recognition result drawn on the PREVIEW frame ----
app = m.App()                      # UART unavailable -> _NullUart, no hardware
app.ovl = res
pimg = FakeImg()
app.overlay(pimg)
exp_box2 = m._extend_box(10, 10, 100, 30, m.KPU_DET_EXTEND)
check("overlay draws rect",
      ("rect",) + exp_box2 in [tuple(d) for d in pimg.drawn], True)
check("overlay draws plate text",
      any(d[0] == "str" and "JingA88888" in d[1] for d in pimg.drawn), True)

app.ovl = {"boxes": [], "det": 0, "error": "no_plate", "plate_ascii": None}
pimg2 = FakeImg()
app.overlay(pimg2)
check("overlay shows det status when no plate",
      any(d[0] == "str" and "det=0" in d[1] for d in pimg2.drawn), True)

m.DRAW_DETECT = 0
pimg3 = FakeImg()
app.overlay(pimg3)
check("overlay disabled by DRAW_DETECT=0", pimg3.drawn, [])
m.DRAW_DETECT = 1

# ---- 4. multiple boxes / failure paths ----
low = make_rows(12, [0, 32, 32, 32, 32, 32], 8)
for r in low:
    for j in range(len(r)):
        r[j] *= 0.5
m.INFER_MODE = "A"
m._KPU_LOADED = True
m._KPU_DET = FakeDet([(10, 10, 100, 30), (50, 50, 120, 40)])
m._KPU_RECOG = SeqRecog([low, make_rows(19, CHAR_YUE_B12345, 8)])
res = m.recognize_frame(FakeImg())
check("higher-conf box wins", res["plate"], "\u7ca4B12345")
check("det count == 2", res["det"], 2)

m._KPU_DET = FakeDet([])
m._KPU_RECOG = FakeRecog(make_rows(19, CHAR_YUE_B12345, 8))
res = m.recognize_frame(FakeImg())
check("no box -> error no_plate", res["error"], "no_plate")
check("no box -> det 0", res["det"], 0)

lowrows = make_rows(19, CHAR_YUE_B12345, 8)
for r in lowrows:
    for j in range(len(r)):
        r[j] *= 0.3
m._KPU_DET = FakeDet([(10, 10, 100, 30)])
m._KPU_RECOG = FakeRecog(lowrows)
res = m.recognize_frame(FakeImg())
check("low conf -> no_plate", res["error"], "no_plate")

# INFER_MODE=B -> no_plate, det=-1
m.INFER_MODE = "B"
res = m.recognize_frame(FakeImg())
check("INFER_MODE=B -> no_plate/-1", (res["error"], res["det"]),
      ("no_plate", -1))


class BoomDet:
    def run_with_output(self, img):
        raise RuntimeError("kpu fail")

    def regionlayer_yolo2(self):
        return []


m.INFER_MODE = "A"
m._KPU_LOADED = True
m._KPU_DET = BoomDet()
res = m.recognize_frame(FakeImg())
check("detect exception -> detect_error", res["error"], "detect_error")

# ---- 5. model path resolution: files at SD root must be found ----
real_stat = m._stat_size


def fake_stat_factory(present_dir):
    """只有 present_dir 下三件套存在，其余路径 stat=-1。"""
    def _f(path):
        d, _, name = path.rpartition("/")
        if d == present_dir and name in m.KPU_PATH_TAIL:
            return {"lp_detect.kmodel": 460456, "lp_recog.kmodel": 697512,
                    "lp_weight.bin": 1498500}[name]
        return -1
    return _f


try:
    m._stat_size = fake_stat_factory("/sd")           # 只放卡根目录
    det_p, rec_p, w_p, found = m._resolve_models()
    check("resolve finds files at SD root", found, "/sd")
    check("resolve det path", det_p, "/sd/lp_detect.kmodel")
    check("resolve weight path", w_p, "/sd/lp_weight.bin")

    m._stat_size = fake_stat_factory("/sd/KPU")       # 参考例程目录仍优先
    _, _, _, found = m._resolve_models()
    check("resolve prefers /sd/KPU", found, "/sd/KPU")

    m._stat_size = fake_stat_factory("/flash")        # 兜底 /flash
    _, _, _, found = m._resolve_models()
    check("resolve falls back to /flash", found, "/flash")

    m._stat_size = lambda path: -1                    # 全都找不到
    _, _, _, found = m._resolve_models()
    check("resolve none when missing", found, None)
finally:
    m._stat_size = real_stat

# ---- 6. model-load failure surfaces a short reason (shown on screen too) ----
m.INFER_MODE = "A"
m._KPU_LOADED = False
m._KPU_LOAD_FAILED = False
m._KPU_ERR = ""
keep = (m.KPU_DET_MODEL, m.KPU_RECOG_MODEL, m.KPU_RECOG_WEIGHT)
m.KPU_DET_MODEL = "/nope/lp_detect.kmodel"
m.KPU_RECOG_MODEL = "/nope/lp_recog.kmodel"
m.KPU_RECOG_WEIGHT = "/nope/lp_weight.bin"
res = m.recognize_frame(FakeImg())
m.KPU_DET_MODEL, m.KPU_RECOG_MODEL, m.KPU_RECOG_WEIGHT = keep
check("missing models -> model_unavailable", res["error"], "model_unavailable")
check("missing models -> det -1", res["det"], -1)
check("missing models -> kpu_err set", res.get("kpu_err"),
      "sd models missing")
app.ovl = res
pimg4 = FakeImg()
app.overlay(pimg4)
check("overlay shows kpu_err on screen",
      any(d[0] == "str" and "sd models missing" in d[1] for d in pimg4.drawn),
      True)

# ---- 6. decide() + geometry ----
m.INFER_MODE = "A"
m._KPU_LOADED = True
ok, obj = m.decide({"plate": "\u7ca4B12345", "confidence": 0.93, "ts": 123})
check("decide ok", (ok, obj), (True, {"plate": "\u7ca4B12345",
                                      "confidence": 0.93, "ts": 123}))
ok, _ = m.decide({"plate": "\u7ca4B12345", "confidence": 0.40, "ts": 123})
check("decide rejects low conf", ok, False)
ok, _ = m.decide({"plate": None, "confidence": 0.0, "error": "no_plate"})
check("decide rejects no plate", ok, False)

check("extend_box clamps to frame",
      m._extend_box(-20, -20, 400, 300, 0.08), (0, 0, m.CAM_W, m.CAM_H))
check("extend_box grows by 8%",
      m._extend_box(100, 100, 100, 40, 0.08), (92, 96, 116, 47))

# ---- 7. ROI_MODE=1: the OPERATOR'S fixed box replaces the detection model ----
# Strategy decision 2026-09-15: at a fixed capture point the plate always sits in the
# same region, so the YOLOv2 detector (460456 B + init_yolo2 ~68KB of system heap and
# one more inference per tick) can be skipped entirely.  What must hold:
#   * the detector is never loaded or run in this mode;
#   * the crop is the ROI, mapped from the REAL image size (relative coords);
#   * the model search must not still demand lp_detect.kmodel;
#   * ROI_EXTEND=0 (the operator's box is used as-is, so the plate fills 208x64).
print()
print("== 7. ROI_MODE: fixed box instead of the detection model ==")


class FakeImgWH(FakeImg):
    def __init__(self, w, h):
        FakeImg.__init__(self)
        self._w, self._h = w, h

    def width(self):
        return self._w

    def height(self):
        return self._h


_saved = (m.ROI_MODE, m.ROI_X, m.ROI_Y, m.ROI_W, m.ROI_H, m.ROI_EXTEND,
          m.KPU_DET_MODEL)
m.INFER_MODE = "A"
m._KPU_LOADED = True
m._KPU_RECOG = FakeRecog(make_rows(19, CHAR_YUE_B12345, 8))
m._KPU_DET = None
m.ROI_MODE = 1
m.ROI_X, m.ROI_Y, m.ROI_W, m.ROI_H = 0.05, 0.30, 0.90, 0.40
m.ROI_EXTEND = 0.0

res = m.recognize_frame(FakeImgWH(320, 240))
want = m._extend_box(int(0.05 * 320), int(0.30 * 240), int(0.90 * 320),
                     int(0.40 * 240), 0.0)
check("ROI -> det count is 1", res["det"], 1)
check("ROI box == the operator's box (no 8% growth)", res.get("boxes"), [want])
check("ROI -> plate decoded", res["plate"], "\u7ca4B12345")
# 2026-09-16 correction: the box the OPERATOR configured is what the model gets.  The
# 09-15 single-file build - the one that really recognised plates on the board - fed the
# WHOLE ROI (288x96 = 55,296 B of contiguous GC memory here).  Adding a "centred sub-box"
# cap silently chopped the plate's ends off and turned recognition into garbage (crop_ok
# followed by `NG ... no usable output`), so the cap defaults to OFF and the memory is
# bought with the GC heap instead (main.py GC_SET=768K).
check("the cap is OFF by default", m.CROP_MAX_PIXELS, 0)
check("the whole configured ROI reaches the model", want[2] * want[3] * 2, 288 * 96 * 2)

# relative coords: the same ROI constants on QQVGA must land at half the pixels
res = m.recognize_frame(FakeImgWH(160, 120))
want_q = m._extend_box(int(0.05 * 160), int(0.30 * 120), int(0.90 * 160),
                       int(0.40 * 120), 0.0)
check("ROI follows the real image size", res.get("boxes"), [want_q])
check("ROI box is half-size at QQVGA", (want_q[2], want_q[3]),
      (want[2] // 2, want[3] // 2))

# an over-large ROI must be clamped inside the frame, never negative/zero
m.ROI_X, m.ROI_Y, m.ROI_W, m.ROI_H = 0.80, 0.80, 0.90, 0.90
res = m.recognize_frame(FakeImgWH(320, 240))
b = res["boxes"][0]
check("over-large ROI is clamped in-frame",
      (b[0] >= 0 and b[1] >= 0 and b[0] + b[2] <= 320 and b[1] + b[3] <= 240
       and b[2] > 0 and b[3] > 0), True)
m.ROI_X, m.ROI_Y, m.ROI_W, m.ROI_H = 0.05, 0.30, 0.90, 0.40

# the detector must not be loaded at all (no stat of the kmodel, no [KPU] line)
stat_calls = []
_real_stat = m._stat_size
m._stat_size = lambda p: (stat_calls.append(p), _real_stat(p))[1]
m._KPU_DET_TRIED = False
m._KPU_DET = None
loaded = m.kpu_load_detect()
m._stat_size = _real_stat
check("ROI -> detector not loaded", loaded, False)
check("ROI -> det kmodel never stat'd",
      [p for p in stat_calls if "lp_detect" in p], [])
check("ROI -> det marked as tried (no re-check)", m._KPU_DET_TRIED, True)

# the model SEARCH must be satisfied by recog+weight alone in ROI mode
_tail = m.KPU_PATH_TAIL


def only_two(present_dir):
    def _f(path):
        d, _, name = path.rpartition("/")
        if d == present_dir and name in _tail[1:]:
            return {"lp_recog.kmodel": 697512, "lp_weight.bin": 1498500}[name]
        return -1
    return _f


try:
    m._stat_size = only_two("/sd/KPU")
    _det, _rec, _w, found = m._resolve_models()
    check("ROI resolve: 2 files are enough", found, "/sd/KPU")
    check("ROI resolve: recog/weight paths", (_rec, _w),
          ("/sd/KPU/lp_recog.kmodel", "/sd/KPU/lp_weight.bin"))
    m.ROI_MODE = 0
    _, _, _, found_dual = m._resolve_models()
    check("dual-model mode still demands all three", found_dual, None)
finally:
    m._stat_size = _real_stat

(m.ROI_MODE, m.ROI_X, m.ROI_Y, m.ROI_W, m.ROI_H, m.ROI_EXTEND,
 m.KPU_DET_MODEL) = _saved

# ---- 8. a failing C call is LOCALISED: which call, which stage, how much memory ----
# Board bug 2026-09-15: round 1 of the recognition ran (ms=67), every later round died
# with `TypeError("unsupported types for __mod__: '', 'tuple'")` reported under one
# catch-all tag (`recog_box`), so we could not tell run_with_output from lp_recog - and
# that message cannot come from our own code (every `%` here is a literal format
# string).  The board log must now name the call and print both heap figures.
print()
print("== 8. recog failures say WHICH C call died ==")
import contextlib
import io as _io

_src = open(MAIN, encoding="utf-8").read()
for _needle in ('_dbg_once("recog_run"', '_dbg_once("recog_out"',
                '_dbg_once("recog_box"',
                "stage=%s exception=%r", "_mem2()"):
    check("park_app.py localises: %s" % _needle, _needle in _src, True)

# The crop path reports differently ON PURPOSE (changed 2026-09-16): unlike the two C
# calls above, a failing crop really does recur frame after frame (GC fragmentation),
# so `_dbg_once` would hide the 2nd..Nth occurrence - exactly the wrong thing.  It now
# prints the first 3 and then every 20th, and resets on success.  It must still name
# itself and print both heap figures.
for _needle in ('print("[DBG] recog_crop:', "_recog_crop_fail_n"):
    check("park_app.py crop failure is rate-limited but named: %s" % _needle,
          _needle in _src, True)
_crop_body = _src[_src.index("def _recog_crop"):_src.index("def recognize_frame")]
check("_recog_crop collects before allocating (the fragmentation fix)",
      "gc.collect()" in _crop_body, True)
check("_recog_crop drops the cut image before resizing",
      "del lp_img" in _crop_body, True)
for _st in ("extend", "crop", "run", "out", "decode"):
    check("park_app.py marks stage %s" % _st, ('stage = "%s"' % _st) in _src, True)

# 2026-09-15 board run #2: every frame printed a bare `NG err=no_plate` while crop/run/out
# all succeeded - so the failure is in the DECISION layer, and `no_plate` has two very
# different causes (no usable output vs. answer below RECOG_CONF_TH).  The near miss must
# reach the log, otherwise every 3.5-minute cold boot buys the same useless line.
for _needle in ('"best_conf"', "best=%s conf=%s th=%s",
                "model gave no usable output"):
    check("park_app.py exposes the near miss: %s" % _needle, _needle in _src, True)


class BoomRun:
    """First crop runs (low confidence, so the loop keeps going), second crop raises
    exactly what the board raised."""

    def __init__(self, rows):
        self.rows = rows
        self.n = 0

    def run_with_output(self, img):
        self.n += 1
        if self.n > 1:
            raise TypeError("unsupported types for __mod__: '', 'tuple'")

    def lp_recog(self):
        return self.rows


def _low_rows():
    """below RECOG_CONF_TH -> the box loop must NOT break, it tries the next box."""
    rows = [[0.01] * len(m.PROVINCES)]
    for _ in range(7):
        rows.append([0.01] * len(m.ADS))
    return rows


_saved_dbg = dict(m._DBG_SHOWN)
_saved_state = (m.ROI_MODE, m.INFER_MODE, m._KPU_DET, m._KPU_RECOG, m._KPU_LOADED,
                m._KPU_DET_TRIED)
_saved_gc = m.gc
_saved_sysfree = m._sys_heap_free
try:
    m._DBG_SHOWN.clear()
    m.INFER_MODE = "A"
    m._KPU_LOADED = True
    m.ROI_MODE = 0                       # two boxes -> the second one dies mid-loop
    m._KPU_DET_TRIED = True
    m._KPU_DET = FakeDet([(10, 10, 100, 30), (50, 50, 120, 40)])
    m._KPU_RECOG = BoomRun(_low_rows())
    _buf = _io.StringIO()
    with contextlib.redirect_stdout(_buf):
        _res = m.recognize_frame(FakeImg())
    _log = _buf.getvalue()
    check("failed call named: recog_run", "recog_run" in _log, True)
    check("catch-all recog_box is NOT used", "recog_box" in _log, False)
    check("the C-layer message is kept",
          "unsupported types for __mod__" in _log, True)
    check("crop size logged", " sent as 100x30)" in _log, True)
    check("both heap figures logged", "free=" in _log, True)
    check("no plate invented on failure", (_res["plate"], _res["error"]),
          (None, "no_plate"))
    # The whole point of the 09-15 #2 change: a below-threshold answer must still be
    # visible, otherwise "model saw nothing" and "model saw it but scored 0.01" are
    # indistinguishable in the board log and the next cold boot teaches us nothing.
    check("near miss is exposed (answer present, below threshold)",
          (_res["best"] is not None, _res["best_conf"] is not None,
           _res["best_conf"] < m.RECOG_CONF_TH),
          (True, True, True))
    check("exposed near miss is not promoted to a plate", _res["plate"], None)

    # _mem2() must never raise, even when the heap helpers are missing/broken
    class _G:
        @staticmethod
        def mem_free():
            return 4096
    m.gc = _G
    m._sys_heap_free = lambda: (8192, None)
    check("_mem2 formats both heaps", m._mem2(), "free=4096 sysfree=8192")
    m._sys_heap_free = lambda: (_ for _ in ()).throw(RuntimeError("no maix"))
    check("_mem2 never raises", m._mem2(), "free=?")
finally:
    m._DBG_SHOWN.clear()
    m._DBG_SHOWN.update(_saved_dbg)
    (m.ROI_MODE, m.INFER_MODE, m._KPU_DET, m._KPU_RECOG, m._KPU_LOADED,
     m._KPU_DET_TRIED) = _saved_state
    m.gc = _saved_gc
    m._sys_heap_free = _saved_sysfree

# ---- 9. the SD read ETA line: silence must never look like a hang ----------------
# Board 2026-09-15: after `[DBG] > rec.lp_recog_load_weight_data(...)` there is NOTHING
# for ~35s at 400kHz, because that C call reads the whole 1.5MB file before returning and
# prints nothing itself.  One line of ETA (plus a 64KB heartbeat in the driver) is what
# turns "stuck?" into "reading, 30 more seconds".
print()
print("== 9. SD read ETA line ==")
_buf = _io.StringIO()
with contextlib.redirect_stdout(_buf):
    m._sd_eta("weight", 1498500)
_txt = _buf.getvalue()
check("eta names what and how many bytes",
      ("weight" in _txt and "1498500" in _txt), True)
# derive the expectation from the constants: BAUD_TRY/BAUD_FAST change as the SD
# speed-up is switched on and off, the ETA must follow whichever clock is in force.
_baud_now = m.BAUD_FAST if m.BAUD_TRY else m.BAUD
check("eta states the baud in use", ("%d baud" % _baud_now) in _txt, True)
check("eta states the heartbeat", "SDX progress line every 64 KB" in _txt, True)
_secs = int(_txt.split("-> ~")[1].split(" s")[0])
check("eta matches baud/8 * 0.8", _secs, 1498500 // (_baud_now // 8 * 4 // 5))
check("driver heartbeat is 64KB", m.READ_PROGRESS_BYTES, 64 * 1024)
_save_baud = (m.BAUD_TRY, m.BAUD_FAST)
try:
    m.BAUD_TRY = 0                 # mount clock only -> slower estimate, still prints
    m.BAUD_FAST = 0                # a silly value must not raise either
    _buf2 = _io.StringIO()
    with contextlib.redirect_stdout(_buf2):
        m._sd_eta("x", 10)
    check("eta survives silly constants", "100000 baud" in _buf2.getvalue(), True)
finally:
    m.BAUD_TRY, m.BAUD_FAST = _save_baud

print()
print("== 10. the console preview can never kill the app (2026-09-16 board report) ==")
# Board symptom: the stream simply stopped right after a 14280-char frame, with no
# Python error at all.  The preview is only "something to look at": if it raises
# (chunked print / base64 allocation / CDC write), the whole main loop dies and
# recognition dies with it.  So the send path must be guarded, and the drop rate
# must be reportable without flooding - or without throwing from the reporter.
check("preview drops are counted", "_preview_fail_n = 0" in _src, True)
check("the reporter exists", "def _preview_drop(self, why):" in _src, True)
check("the reporter names itself in the log",
      '"[DBG] console preview dropped' in _src, True)
_send = _src[_src.index("def console_send_jpeg(self, jpeg):"):
             _src.index("def console_recognize_tick(")]
check("the base64 step is guarded", _send.count("except Exception") >= 2, True)
check("the chunk loop is inside a try",
      _send.index("try:") < _send.index("for i in range(0, len(b64)"), True)
check("a dropped frame is only one frame",
      _send.count("self._preview_drop(") >= 2, True)
# the knobs that keep a preview frame small enough for the console link
check("preview is sent every 10th frame", m.CONSOLE_IMG_EVERY, 10)
check("jpeg quality trimmed for the console", m.JPEG_QUALITY, 40)
check("gc runs at least once a second", m.GC_PERIOD_MS <= 1000, True)

print()
print("== 11. the ROI crop OOMs on the board but not in the IDE (2026-09-16 report) ==")
# Board log: `[DBG] recog_crop: MemoryError('Out of normal MicroPython Heap Memory!')
# free=354784 sysfree=1265664` - BOTH heaps report plenty free, and the line does not
# say which allocation died or in which pool.  Without that, "raise gc_heap" and "cut
# the camera/LCD cost" are a coin flip.  So:
#   1. the failure names the STAGE and how many BYTES it wanted;
#   2. _gc_probe answers "can a GC block that size be had at all?" (the only direct
#      evidence that separates a fragmented GC heap from a fragmented system heap);
#   3. a cut that already IS 208x64 is not resized again (one 26KB allocation saved
#      per tick, and the 55KB cut is the fragile one);
#   4. the per-tick [RECOG] line carries cropfail=N, because with a dead crop the
#      model never sees an image and `no_plate` reads like "the model said no".
check("the probe helper exists", "def _gc_probe(nbytes):" in _src, True)
check("the crop names the stage it died in", 'stage = "cut"' in _crop_body, True)
check("the crop skips a useless resize",
      "if lp_img.width() == RECOG_W and lp_img.height() == RECOG_H:" in _crop_body, True)
for _needle in ("stage=%s", "want=%dB", "probe_cut=%d", "probe_recog=%d"):
    check("crop failure reports %s" % _needle, _needle in _crop_body, True)
check("the per-tick verdict carries the crop failure count",
      "cropfail=%s" in _src, True)
check("_gc_probe accepts a sane size", m._gc_probe(4096), True)
check("_gc_probe rejects an impossible size", m._gc_probe(1 << 45), False)


class _CutBoom:
    """The board's failing path: cut() is the allocation that dies."""

    def cut(self, x, y, w, h):
        raise MemoryError("Out of normal MicroPython Heap Memory!")

    def width(self):
        return 320

    def height(self):
        return 240


_saved_crop_n = m._recog_crop_fail_n
_saved_dbg2 = dict(m._DBG_SHOWN)
try:
    m._DBG_SHOWN.clear()
    m._recog_crop_fail_n = 0
    _b = _io.StringIO()
    with contextlib.redirect_stdout(_b):
        _r = m._recog_crop(_CutBoom(), 8, 72, 288, 96)
    _t = _b.getvalue()
    check("a failing cut returns None", _r, None)
    check("a failing cut is counted", m._recog_crop_fail_n, 1)
    check("a failing cut names stage=cut", "stage=cut" in _t, True)
    check("a failing cut reports the bytes it wanted", "want=55296B" in _t, True)
    check("a failing cut probes the GC heap", "probe_cut=1" in _t, True)
    check("a failing cut keeps the C message", "Out of normal MicroPython" in _t, True)
finally:
    m._recog_crop_fail_n = _saved_crop_n
    m._DBG_SHOWN.clear()
    m._DBG_SHOWN.update(_saved_dbg2)


class _Exact:
    """cut() already hands back 208x64 -> resize must not run at all."""

    def __init__(self):
        self.resized = 0

    def cut(self, x, y, w, h):
        return self

    def width(self):
        return m.RECOG_W

    def height(self):
        return m.RECOG_H

    def resize(self, w, h):
        self.resized += 1
        return self

    def replace(self, **kw):
        return self

    def pix_to_ai(self):
        pass


_e = _Exact()
_saved_dbg3 = dict(m._DBG_SHOWN)
_b2 = _io.StringIO()
try:
    m._DBG_SHOWN.clear()          # earlier sections already burned the once-only tag
    with contextlib.redirect_stdout(_b2):
        _ok = m._recog_crop(_e, 0, 0, m.RECOG_W, m.RECOG_H)
finally:
    m._DBG_SHOWN.clear()
    m._DBG_SHOWN.update(_saved_dbg3)
check("an exact-size cut comes straight back", _ok is _e, True)
check("an exact-size cut is never resized", _e.resized, 0)
check("a good crop resets the failure counter", m._recog_crop_fail_n, 0)
check("a good crop logs one baseline line", "ok free=" in _b2.getvalue(), True)

print()
print("== 12. the ROI box is capped to ONE allocatable block (2026-09-16 board run) ==")
# Board evidence that forced this: cropfail climbed 0->2->8->11->14->17->18 while the
# failure line carried BOTH probe results -- `want=54720B probe_cut=0 probe_recog=1` with
# `free=378048`.  Total free is plentiful; ONE 54,720 B *contiguous* GC block is not, and
# MicroPython's mark-sweep GC never compacts, so this never self-heals (`gc.collect()` is
# already called at the top of _recog_crop).  The memory answer is the GC HEAP (main.py
# GC_SET=768K), NOT shrinking the box: capping the box changes what the model sees, and
# doing that by default broke recognition outright (see section 7).  So the cap ships OFF
# and stays a manual lever; the probe + `cropfail` + the `[CFG]` fingerprint stay as the
# diagnostics, and the boot log still says which build/ROI/threshold the board runs.
check("the cap exists but ships OFF", "CROP_MAX_PIXELS = 0" in _src, True)
check("the cap is applied where the box is built (yellow box == model box)",
      "_cap_box(x, y, w, h, CROP_MAX_PIXELS" in _src, True)
check("a capped box says so once", '_dbg_once("roi_cap"' in _src, True)
check("the build fingerprint is printed at boot", "[CFG] build=%s" in _src, True)
check("the fingerprint carries the ROI + threshold",
      ("roi=%.3f,%.3f,%.3f,%.3f" in _src and "conf_th=%.2f" in _src), True)
check("BUILD is a non-empty literal", isinstance(m.BUILD, str) and len(m.BUILD) >= 6, True)
# 2026-09-16 board run #2: with the box capped the failure moved to the LAST allocation
# (`stage=pix_to_ai ... probe_cut=1 probe_recog=0 free=380032`) - cut, resize and hmirror
# had all succeeded.  So every big allocation on the crop path must be preceded by a
# collect (GC never compacts; the collect is what maximises the current run), and the
# failure hint must name the two real levers.
check("the collect helper never raises", "def _gc_collect():" in _src, True)
check("every big allocation in the crop is preceded by a collect",
      _crop_body.count("_gc_collect()") >= 3, True)
check("the failure hint names the real levers", "or raise GC_SET" in _src, True)

check("a box that already fits is untouched",
      m._cap_box(10, 20, 100, 40, 13312, 320, 240), (10, 20, 100, 40))
_bb = m._cap_box(0, 0, 288, 96, 13312, 320, 240)
check("a loose box is shrunk under the cap", _bb[2] * _bb[3] <= 13312, True)
check("shrinking keeps the centre",
      (abs((_bb[0] + _bb[2] / 2.0) - 144) <= 1, abs((_bb[1] + _bb[3] / 2.0) - 48) <= 1),
      (True, True))
check("shrinking keeps the aspect", abs((_bb[2] / float(_bb[3])) - 3.0) < 0.15, True)
check("shrinking stays inside the frame",
      (_bb[0] >= 0 and _bb[1] >= 0 and _bb[0] + _bb[2] <= 320 and _bb[1] + _bb[3] <= 240),
      True)
check("a huge box still lands inside the frame",
      m._cap_box(0, 0, 100000, 100000, 13312, 320, 240)[2:] >= (8, 8), True)
check("cap=0 disables the cap entirely",
      m._cap_box(0, 0, 288, 96, 0, 320, 240), (0, 0, 288, 96))

print()
if fails:
    print("FAILED %d: %s" % (len(fails), fails))
    sys.exit(1)
print("ALL PASS")
