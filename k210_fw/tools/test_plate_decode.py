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

MAIN = r"D:\Projects\Project_EdgeParking-CloudSync\k210_fw\main.py"
spec = importlib.util.spec_from_file_location("k210main", MAIN)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)

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

print()
if fails:
    print("FAILED %d: %s" % (len(fails), fails))
    sys.exit(1)
print("ALL PASS")
