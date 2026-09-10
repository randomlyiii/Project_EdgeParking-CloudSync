# -*- coding: utf-8 -*-
"""K210 固件【单文件版】—— CanMV IDE 直接运行这一个文件即可（无需上传任何模块）。

内容 = config + uart_proto + capture + preview + infer + main 全部内联，
与 k210_fw/ 多文件版逻辑一致（帧协议见 docs/protocols.md §2）。
调参：见本文件顶部「常量区」；硬件差异点（sensor/UART API、JPEG 语义）在对应函数内标 TODO。
"""
import gc
import json
import sys
import utime

# CanMV IDE「运行」只投递当前文件；把常见模块根补进 sys.path（无害，不存在则忽略）。
for _p in ("/flash", "/sd"):
    if _p not in sys.path:
        sys.path.append(_p)


# ============================ 常量区（对应 config.py） ============================
UART_ID = 1                # TODO(实机)：以板子可用串口为准
UART_BAUD = 921600         # 起步值；实测误码可退回 460800
UART_RX_BUF_LEN = 4096
PAYLOAD_MAX = 1024         # 协议单帧 payload 上限（≤1KB）
PREVIEW_CHUNK = 900

# 实测(固件1.0.4)：set_pixformat(sensor.JPEG) 报 "Pixel format is not supported!" ——
# 本固件无硬件 JPEG，只能用软件 compress()（CPU 时间≈像素数，是 fps 瓶颈）。
# 对齐板卡验证过的 helloworld_1.py：QVGA(320x240)=板载 LCD 同尺寸，屏上画面干净满屏；
# 软件压缩在 QVGA 下 fps 约 3~5（要更快再临时切 QQVGA 160x120，代价是屏上画面错位/局部）。
CAM_W, CAM_H = 320, 240   # QVGA（=LCD 原生尺寸；帧率优先可临时改 160,120）
JPEG_HW = False           # 硬件 JPEG 本固件不支持（实测报错），保持 False 走软件压缩
JPEG_QUALITY = 55         # 软件压缩质量(2026-09-10: 70→55 压帧长提 console fps; 车牌清晰度够用)
LCD_PREVIEW = True        # 板载屏本地预览（helloworld 同款：QVGA 满屏显示）
# 方向修正 —— 两层，**实测(2026-09-11)传感器层无效，故默认走软件层**：
#   ① 传感器层(sensor.set_vflip/set_hmirror)：本 CanMV 固件(v1.0.4)实测**不起作用**
#      ——CAM_HMIRROR=True 后画面依旧镜像，排查模式四组合画面也无变化 → 故置 False 不用。
#   ② 软件层(img.replace(hmirror/vflip))：直接改像素，**必然生效**，且预览/上行 JPEG/
#      检测/识别走的是同一帧，方向一致（模型要的就是修正后的画面）。
# 实测症状：水平镜像（左右反），竖直正常 ⇒ 只开 hmirror。
# 若方向再变：跑 ORIENT_PROBE（现在也走软件层，一定看得出变化）挑"字正立且不镜像"的编号。
CAM_VFLIP = False          # 传感器层（本固件无效，保留开关备用）
CAM_HMIRROR = False
CAM_SW_VFLIP = False       # 软件层：垂直镜像（上下反）→ 关
CAM_SW_HMIRROR = True      # 软件层：水平镜像（左右反）→ 开（修正当前症状）
# 方向排查模式：置 1 后每 ORIENT_PROBE_MS 自动轮换 vflip/hmirror 四种组合并打印编号，
# 看 CanMV IDE 预览窗口挑"正立且不镜像"的那一组，再把编号填回上面两个常量。
ORIENT_PROBE = 0
ORIENT_PROBE_MS = 4000
# ⚠️ 实测(2026-09-07)：本固件 USB 只走 console(print) 文本；machine.UART 二进制帧到不了 USB。
# LINK="console"=走 console 打印 base64 文本行（零线、必通、较慢，QVGA 约 0.5~1s/帧）；
# LINK="uart"=板级串口 UART_ID（预留，需杜邦线）。
LINK = "console"
CONSOLE_CHUNK = 900       # console 模式：每行 base64 字符数(2026-09-10: 600→900 减行数提fps)
CONSOLE_PACE_MS = 25      # 每行间延时(2026-09-10: 40→25; 丢行就调回 40)
# 占机调试开关：置 0 = 停发 base64 预览(不压缩、省 CPU)，串口只剩 [KPU]/[RECOG]/[stat]
# 诊断行，IDE 预览窗口照常显示(帧缓冲与 print 是两条独立通道)。接 MP157 时置回 1。
CONSOLE_PREVIEW = 1
# 无 UART 引出 → 识别改"持续周期识别+console 结果行上行"（不再需要下行 0xC1 触发）：
RECOG_PERIOD_MS = 3000    # console 模式识别周期(预览流照常, 到点抓一帧识别)
CONSOLE_FAKE_RESULT = 0   # 置 1 = 无模型期间每周期发假车牌, 用于打通 Qt 弹卡/判定全链路
CONSOLE_FAKE_PLATE = "粤B12345"
CONSOLE_FAKE_CONF = 0.95

INFER_MODE = "A"           # A=本地 KPU 双模型(检测+识别)；B=仅占位（无模型，识别交云兜底）
RECOG_CONF_TH = 0.60       # 低置信判失败（与 Core1 P5-11 同值）
KPU_RETRY_MS = 10000       # kmodel 加载失败后的重试间隔（SD 后插/文件补齐场景）
DRAW_DETECT = 1            # 置 1 = 在识别帧上画检测框+车牌(ASCII)，IDE 帧缓冲可见=模型在工作
HEARTBEAT_MS = 1000        # 0x7E 心跳周期
TRIGGER_COOLDOWN_MS = 2000 # 两次识别最小间隔
GC_PERIOD_MS = 2000        # 低频 gc 周期（每轮 collect 开销大，拉低帧率）
STAT_PERIOD_MS = 5000      # 性能统计打印周期（IDE 串行终端可见）


def _log(msg):
    """cdc 模式下 print 与二进制帧共用同一条 CDC，会污染帧流（靠对端丢帧容错），
    故 cdc 模式静默；uart 模式正常打印。"""
    if LINK != "cdc":
        print(msg)


# ============================ 协议层（对应 uart_proto.py） ============================
HEADER = b"\xAA\x55"
TYPE_PREVIEW_CHUNK = 0x01   # ↑ JPEG 分块
TYPE_PREVIEW_TAIL = 0x02    # ↑ 帧尾：4B 本帧总长(LE) + 1B 用途
TYPE_CMD_RECOGNIZE = 0xC1   # ↓ 抓拍识别（payload 空）
TYPE_RECOG_RESULT = 0xC2    # ↑ 识别结果 JSON
TYPE_RECOG_FAIL = 0xC3      # ↑ 识别失败 JSON（≤1KB）
TYPE_HEARTBEAT = 0x7E       # ↔ 心跳/状态（1B：bit0 busy）
FRAME_KIND_PREVIEW = 0      # 0x02 用途：预览帧
FRAME_KIND_SNAP = 1         # 0x02 用途：识别抓拍帧（供云兜底）
MIN_FRAME = 9               # 2 头 + 1 type + 2 seq + 2 len + 2 crc
CRC_POLY = 0x1021
CRC_INIT = 0x0000


# ---- CRC16：优先 binascii.crc_hqx（C 实现，即 XMODEM 参数：poly 0x1021、
# 不反射、无异或，init 由入参给定）；导入失败退回 256 项查表法，结果一致。
# 纯 Python 逐位实现在 K210 上是预览流最大瓶颈（每帧 10~20KB × 8 次内层循环）。
try:
    from binascii import crc_hqx as _crc_hqx_fast
except ImportError:
    _crc_hqx_fast = None

if _crc_hqx_fast is not None:
    def crc16(data):
        """CRC-16/XMODEM。校验向量：crc16(b"123456789") == 0x31C3。"""
        return _crc_hqx_fast(data, CRC_INIT)
else:
    def _build_table():
        tb = []
        for i in range(256):
            c = i << 8
            for _ in range(8):
                c = ((c << 1) ^ CRC_POLY) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
            tb.append(c)
        return tuple(tb)

    _CRC_TABLE = _build_table()

    def crc16(data):
        """CRC-16/XMODEM（查表回退实现，与 crc_hqx 结果一致）。"""
        crc = CRC_INIT
        for b in data:
            crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[(crc >> 8) ^ b]
        return crc


def encode_frame(type_, seq, payload=b""):
    body = (bytes([type_])
            + int(seq & 0xFFFF).to_bytes(2, "little")
            + len(payload).to_bytes(2, "little")
            + bytes(payload))
    return HEADER + body + crc16(body).to_bytes(2, "little")


class FrameParser:
    """字节流 → 完整帧列表；坏帧/坏 CRC 丢 1 字节重同步。"""
    def __init__(self, on_frame=None, max_payload=1024):
        self._buf = bytearray()
        self._on_frame = on_frame
        self.max_payload = max_payload

    def feed(self, data):
        if data:
            self._buf += data
        out = []
        while True:
            n = len(self._buf)
            if n < MIN_FRAME:
                break
            if not (self._buf[0] == 0xAA and self._buf[1] == 0x55):
                del self._buf[0]
                continue
            plen = int.from_bytes(self._buf[5:7], "little")
            if plen > self.max_payload:
                del self._buf[0]
                continue
            need = 7 + plen + 2
            if n < need:
                break
            body = bytes(self._buf[2:7 + plen])
            crc = int.from_bytes(self._buf[7 + plen:9 + plen], "little")
            if crc16(body) != crc:
                del self._buf[0]
                continue
            type_ = self._buf[2]
            seq = int.from_bytes(self._buf[3:5], "little")
            payload = bytes(self._buf[7:7 + plen])
            del self._buf[:need]
            out.append((type_, seq, payload))
        if self._on_frame:
            for f in out:
                self._on_frame(*f)
        return out


# ============================ 摄像头（对应 capture.py） ============================
def cam_init():
    # 对齐板卡验证过的 helloworld_1.py 初始化顺序（lcd 先于 sensor + skip_frames 等感光稳定）。
    import sensor
    if LCD_PREVIEW:
        try:
            import lcd
            lcd.init()
            lcd.clear(lcd.RED)
        except Exception as e:
            print("[WARN] lcd init failed:", e)
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)   # 本固件无硬件 JPEG（sensor.JPEG 实测报不支持）
    if CAM_VFLIP:
        sensor.set_vflip(True)
    if CAM_HMIRROR:
        sensor.set_hmirror(True)
    table = {(160, 120): sensor.QQVGA, (320, 240): sensor.QVGA, (640, 480): sensor.VGA}
    sensor.set_framesize(table.get((CAM_W, CAM_H), sensor.QVGA))
    sensor.run(1)
    try:
        sensor.skip_frames(time=1500)     # 等自动曝光/白平衡稳定，避免头几帧花屏
    except Exception:
        pass
    try:
        print("CAM %dx%d" % (sensor.width(), sensor.height()))
    except Exception:
        pass


def capture_frame():
    """snapshot 一帧 + 软件方向校正，返回 image（未压缩）；失败返回 None。
    ⚠️ 校正放在拍摄后、一切消费之前：板载屏/上行 JPEG/检测/识别因此方向一致
    （本固件传感器层 set_hmirror 无效，只能软件层修，见常量区注释）。"""
    try:
        import sensor
        img = sensor.snapshot()
    except Exception:
        return None
    if img is None:
        return None
    if CAM_SW_HMIRROR:
        try:
            img.replace(hmirror=True)
        except Exception as e:
            _dbg_once("sw_hmirror", "replace failed: %r" % (e,))
    if CAM_SW_VFLIP:
        try:
            img.replace(vflip=True)
        except Exception as e:
            _dbg_once("sw_vflip", "replace failed: %r" % (e,))
    return img


def lcd_show(img):
    """板载屏本地预览。⚠️ 必须在 compress 之前调——compress() 原地改写 img。"""
    if not LCD_PREVIEW or img is None:
        return
    try:
        import lcd
        lcd.display(img)
    except Exception:
        pass


def jpeg_from(img):
    """把已拍摄的 image 压成 JPEG 字节；失败返回 None（预览与抓拍共用）。"""
    try:
        if img is None:
            return None
        if JPEG_HW:
            return bytes(img)
        comp = img.compress(quality=JPEG_QUALITY)
        return bytes(comp if comp is not None else img)
    except Exception:
        return None


def capture_jpeg():
    """拍一帧返回 JPEG 字节；失败返回 None（抓拍用，不经本地屏）。"""
    return jpeg_from(capture_frame())


# ============================ 预览流（对应 preview.py） ============================
class PreviewStream:
    """send 为注入的帧写出回调（main 里绑 uart.write），便于协议层单测。"""
    def __init__(self, send, chunk_payload):
        self.send = send
        self.chunk = chunk_payload
        self._seq = 0

    def send_jpeg(self, jpeg, kind=FRAME_KIND_PREVIEW):
        if not jpeg:
            return
        for off in range(0, len(jpeg), self.chunk):
            self._frame(TYPE_PREVIEW_CHUNK, jpeg[off:off + self.chunk])
        tail = len(jpeg).to_bytes(4, "little") + bytes([kind])
        self._frame(TYPE_PREVIEW_TAIL, tail)

    def _frame(self, type_, payload):
        self.send(encode_frame(type_, self._seq, payload))
        self._seq = (self._seq + 1) & 0xFFFF


# ============================ 识别（对应 infer.py） ============================
# 车牌识别 = 本地 KPU 双模型（对齐正点原子 DNK210 车牌实验，2026-09 接入）：
#   1) lp_detect.kmodel  YOLOv2 检测车牌框（输入 QVGA 320x240，特征层 20x15）
#   2) lp_recog.kmodel   逐位识别（位0=省份，位1..7=字符 A-Z去IO + 0-9）
#   3) lp_weight.bin     识别模型配套权重（与 lp_recog.kmodel 一起加载）
# 三文件放 SD 卡（K210 挂载点 /sd）；依赖固件 KPU 扩展 API（init_yolo2/
# regionlayer_yolo2/lp_recog_load_weight_data/lp_recog），正点原子 CanMV(Lite)
# 固件自带；若板载 makerobo 固件缺这些 API，加载会失败并打印原因、按
# KPU_RETRY_MS 重试，预览流不受影响（识别静默返回 no_plate）。
# ⚠️ 目录不敏感：按 KPU_DIRS 顺序自动找齐三件套，**模型直接放卡根目录也能跑**
# （2026-09-11 实测教训：文件放在 F:\ 根目录 → /sd/KPU/ 找不到 → model_unavailable）。
KPU_PATH_TAIL = ("lp_detect.kmodel", "lp_recog.kmodel", "lp_weight.bin")
KPU_DIR = "/sd/KPU"                # 首选（参考例程路径）
KPU_DIRS = (KPU_DIR, "/sd", "/flash/KPU", "/flash", "/")   # 依次回退
KPU_DET_MODEL = KPU_DIR + "/" + KPU_PATH_TAIL[0]      # 首选路径（诊断/文档引用）
KPU_RECOG_MODEL = KPU_DIR + "/" + KPU_PATH_TAIL[1]
KPU_RECOG_WEIGHT = KPU_DIR + "/" + KPU_PATH_TAIL[2]
KPU_DET_THRESHOLD = 0.5    # 检测置信度（参考例程 0.7；现场漏检就调低）
KPU_DET_NMS = 0.3
KPU_DET_LAYER_W, KPU_DET_LAYER_H = 20, 15
KPU_DET_EXTEND = 0.08      # 车牌框外扩比例（切图前，参考例程）
RECOG_W, RECOG_H = 208, 64 # 识别模型输入尺寸
RECOG_HMIRROR = True       # 切图先水平镜像再送识别（参考例程；若字符反了改 False）

# YOLOv2 先验框（参考例程 anchor，勿改）
YOLO_ANCHOR = (8.30891522166988, 2.75630994889035, 5.18609903718768,
               1.7863757404970702, 6.91480529053198, 3.825771881004435,
               10.218567655549439, 3.69476690620971, 6.4088204258368195,
               2.38813526350986)

# 识别类别表（参考例程）：位0=省份 pinyin，位1..7=字符
PROVINCES = ['Wan', 'Hu', 'Jin', 'Yu^', 'Ji', 'Sx', 'Meng', 'Liao', 'Jl',
             'Hei', 'Su', 'Zhe', 'Jing', 'Min', 'Gan', 'Lu', 'Yu', 'E^',
             'Xiang', 'Yue', 'Gui^', 'Qiong', 'Cuan', 'Gui', 'Yun', 'Zang',
             'Shan', 'Gan^', 'Qing', 'Ning', 'Xin']
# pinyin 标签 -> 中文省份简称（协议 plate 须为 UTF-8 中文车牌，如 粤B12345）
PROVINCE_ZH = {'Wan': '\u7696', 'Hu': '\u6caa', 'Jin': '\u6d25',
               'Yu^': '\u8c6b', 'Ji': '\u5180', 'Sx': '\u664b',
               'Meng': '\u8499', 'Liao': '\u8fbd', 'Jl': '\u5409',
               'Hei': '\u9ed1', 'Su': '\u82cf', 'Zhe': '\u6d59',
               'Jing': '\u4eac', 'Min': '\u95fd', 'Gan': '\u8d63',
               'Lu': '\u9c81', 'Yu': '\u6e1d', 'E^': '\u9102',
               'Xiang': '\u6e58', 'Yue': '\u7ca4', 'Gui^': '\u6842',
               'Qiong': '\u743c', 'Cuan': '\u5ddd', 'Gui': '\u8d35',
               'Yun': '\u4e91', 'Zang': '\u85cf', 'Shan': '\u9655',
               'Gan^': '\u7518', 'Qing': '\u9752', 'Ning': '\u5b81',
               'Xin': '\u65b0'}
ADS = ['A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M', 'N', 'P',
       'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
       '0', '1', '2', '3', '4', '5', '6', '7', '8', '9']

_KPU_DET = None            # 检测 KPU 实例（惰性加载，见 kpu_load）
_KPU_RECOG = None          # 识别 KPU 实例
_KPU_LOADED = False
_KPU_LOAD_FAILED = False
_KPU_RETRY_AT = 0
_KPU_ERR = ""              # 失败原因(短, ASCII)——既打串口，也显示到屏上
_DBG_SHOWN = {}            # 一次性调试打印（避免每 3s 刷屏）


def _dbg_once(tag, msg):
    if tag not in _DBG_SHOWN:
        _DBG_SHOWN[tag] = True
        print("[DBG] %s: %s" % (tag, msg))


def _stat_size(path):
    """返回文件字节数；不存在/读不到返回 -1（模型放错目录/名字的第一诊断信号）。"""
    try:
        import uos
        return uos.stat(path)[6]
    except Exception:
        try:
            import os
            return os.stat(path)[6]
        except Exception:
            return -1


def _ls(path, n=12):
    """列目录（前 n 项）——SD 落位不对时直接看 K210 眼中的真实文件名。"""
    for mod in ("uos", "os"):
        try:
            m = __import__(mod)
            return str(m.listdir(path)[:n])
        except Exception:
            continue
    return "<ls failed>"


def _short(exc, n=26):
    s = ("%s" % (exc,)).replace("\n", " ").replace("  ", " ")
    if "emory" in s:
        return "low memory"
    return s[:n]


def _kpu_fail(err):
    """记失败原因（短串上屏用）+ 安排重试，返回 False。"""
    global _KPU_LOAD_FAILED, _KPU_RETRY_AT, _KPU_ERR
    _KPU_LOAD_FAILED = True
    _KPU_RETRY_AT = utime.ticks_add(utime.ticks_ms(), KPU_RETRY_MS)
    _KPU_ERR = err
    print("[KPU] load fail(retry in %dms): %s" % (KPU_RETRY_MS, err))
    return False


SD_MOUNT = "/sd"


def sd_ensure_mounted():
    """主动挂载 SD 卡 —— 本 CanMV 固件(v1.0.4)实测**不会自动挂**：
    REPL 里 `uos.listdir("/")` 只有 ['flash']、`uos.listdir("/sd")` 报 ENODEV，
    但固件里 `machine.SDCard` 与 `uos.mount` 都在（2026-09-11 探测确认）。
    返回 True = /sd 已可用（本来就挂好，或本次挂成功）。"""
    try:
        import uos
    except Exception as e:
        print("[SD] no uos: %r" % (e,))
        return False
    try:
        uos.listdir(SD_MOUNT)
        return True                     # 已经挂好了
    except Exception:
        pass
    try:
        import machine
    except Exception as e:
        print("[SD] no machine module: %r" % (e,))
        return False
    if not hasattr(machine, "SDCard"):
        print("[SD] firmware has no machine.SDCard")
        return False
    try:
        uos.mkdir(SD_MOUNT)
    except Exception:
        pass
    last = "no variant tried"
    for desc, args, kwargs in (("noargs", (), {}),
                               ("slot0", (0,), {}),
                               ("slot0,width1", (), {"slot": 0, "width": 1})):
        try:
            sd = machine.SDCard(*args, **kwargs)
        except Exception as e:
            last = "%s construct: %r" % (desc, e)
            continue
        try:
            uos.mount(sd, SD_MOUNT)
        except Exception as e:
            last = "%s mount: %r" % (desc, e)
            continue
        try:
            print("[SD] mounted %s -> %s"
                  % (SD_MOUNT, uos.listdir(SD_MOUNT)[:8]))
        except Exception:
            print("[SD] mounted %s" % SD_MOUNT)
        return True
    print("[SD] mount failed (%s)" % (last,))
    return False


def _api_missing(obj, names):
    """返回 obj 上缺失的方法名列表（判断固件有没有 DNK210 的 KPU 扩展 API）。"""
    miss = []
    for n in names:
        try:
            if not hasattr(obj, n):
                miss.append(n)
        except Exception:
            miss.append(n)
    return miss


def _exists(path):
    """文件是否可读：先 stat，stat 不可靠/返回 -1 时真去开一次兜底。"""
    if _stat_size(path) >= 0:
        return True
    f = None
    try:
        f = open(path, "rb")
        f.read(1)
        return True
    except Exception:
        return False
    finally:
        try:
            if f:
                f.close()
        except Exception:
            pass


def _mount_diag():
    """SD 挂载诊断：K210 到底认不认这张卡（没挂载时 listdir 会失败）。
    ⚠️ 卡被格式化成 exFAT/NTFS 而固件 FatFs 只支持 FAT12/16/32 时，
    表现就是"卡能读但 /sd 不存在"——此时需把卡重新格式化为 FAT32。"""
    try:
        import uos
        try:
            st = uos.statvfs("/sd")
            print("[KPU] statvfs /sd -> blk=%d bsize=%d free=%d"
                  % (st[0], st[1], st[3]))
        except Exception as e:
            print("[KPU] statvfs /sd failed: %r (SD 未挂载?卡格式?插好?)" % (e,))
    except Exception:
        pass
    for d in ("/", "/sd", "/flash"):
        print("[KPU] ls %s -> %s" % (d, _ls(d)))


def _resolve_models():
    """在 KPU_DIRS 里找齐三件套，返回 (det, recog, weight, dir)；
    都没找齐则返回首选路径 + None（调用方据此报错并列出搜索过的目录）。"""
    for d in KPU_DIRS:
        ps = [d + "/" + n for n in KPU_PATH_TAIL]
        if min(_exists(p) for p in ps):
            return ps[0], ps[1], ps[2], d
    ps = [KPU_DIR + "/" + n for n in KPU_PATH_TAIL]
    return ps[0], ps[1], ps[2], None


def kpu_load():
    """加载检测+识别双模型与权重；失败不阻塞预览，按 KPU_RETRY_MS 重试。
    分步打印（文件→API→检测模型→识别模型），失败原因同时进 _KPU_ERR 上屏。"""
    global _KPU_DET, _KPU_RECOG, _KPU_LOADED, _KPU_ERR
    if _KPU_LOADED:
        return True
    if _KPU_LOAD_FAILED and utime.ticks_diff(utime.ticks_ms(), _KPU_RETRY_AT) < 0:
        return False        # 上次失败且仍在重试窗口内
    # ① 找模型文件：按 KPU_DIRS 顺序解析（卡根目录也认），找不到就报 -1
    det_path, recog_path, weight_path, found = _resolve_models()
    if not found:
        # 本固件不自动挂 SD → 主动挂一次再找（也支持上电后才插卡的情况）
        if sd_ensure_mounted():
            det_path, recog_path, weight_path, found = _resolve_models()
    sizes = (_stat_size(det_path), _stat_size(recog_path),
             _stat_size(weight_path))
    if found:
        print("[KPU] model dir=%s det=%d recog=%d weight=%d (bytes)"
              % ((found,) + sizes))
    else:
        print("[KPU] files det=%d recog=%d weight=%d (bytes, -1=not found)"
              % sizes)
        print("[KPU] searched dirs: %s" % (KPU_DIRS,))
        _mount_diag()          # /sd 到底挂没挂 + 各根目录真实内容
        return _kpu_fail("sd models missing")
    # ② 固件是否提供需要的 KPU 扩展 API（DNK210 CanMV 才有）
    try:
        from maix import KPU
    except Exception as e:
        print("[KPU] import maix.KPU failed: %r" % (e,))
        return _kpu_fail("no maix.KPU")
    try:
        det = KPU()
    except Exception as e:
        print("[KPU] KPU() construct failed: %r" % (e,))
        return _kpu_fail("KPU() failed")
    miss = _api_missing(det, ("load_kmodel", "init_yolo2", "run_with_output",
                              "regionlayer_yolo2"))
    print("[KPU] detect api missing=%s" % miss)
    if miss:
        return _kpu_fail("no API:" + miss[0])
    rec = None
    try:
        rec = KPU()
    except Exception as e:
        print("[KPU] KPU() #2 construct failed: %r" % (e,))
        return _kpu_fail("KPU() failed")
    miss2 = _api_missing(rec, ("load_kmodel", "lp_recog_load_weight_data",
                               "lp_recog", "run_with_output"))
    print("[KPU] recog api missing=%s" % miss2)
    if miss2:
        return _kpu_fail("no API:" + miss2[0])
    # ③ 检测模型
    try:
        det.load_kmodel(det_path)
        det.init_yolo2(YOLO_ANCHOR, anchor_num=len(YOLO_ANCHOR) // 2,
                       img_w=CAM_W, img_h=CAM_H,
                       net_w=CAM_W, net_h=CAM_H,
                       layer_w=KPU_DET_LAYER_W, layer_h=KPU_DET_LAYER_H,
                       threshold=KPU_DET_THRESHOLD, nms_value=KPU_DET_NMS,
                       classes=0)
        print("[KPU] detect model ok")
    except Exception as e:
        print("[KPU] detect load/init failed: %r" % (e,))
        return _kpu_fail("det:" + _short(e))
    # ④ 识别模型 + 权重
    try:
        rec.load_kmodel(recog_path)
        rec.lp_recog_load_weight_data(weight_path)
        print("[KPU] recog model ok")
    except Exception as e:
        print("[KPU] recog load failed: %r" % (e,))
        try:
            del det                 # 失败就别白占 KPU 内存
        except Exception:
            pass
        return _kpu_fail("recog:" + _short(e))
    _KPU_DET, _KPU_RECOG = det, rec
    _KPU_LOADED = True
    _KPU_ERR = ""
    print("[KPU] models ready mem_free=%d" % gc.mem_free())
    return True


def _extend_box(x, y, w, h, scale):
    """车牌框按比例外扩并夹到画面内（参考例程 extend_box）。"""
    x1 = int(x - scale * w)
    x2 = int(x + w - 1 + scale * w)
    y1 = int(y - scale * h)
    y2 = int(y + h - 1 + scale * h)
    x1 = x1 if x1 > 0 else 0
    x2 = x2 if x2 < (CAM_W - 1) else (CAM_W - 1)
    y1 = y1 if y1 > 0 else 0
    y2 = y2 if y2 < (CAM_H - 1) else (CAM_H - 1)
    return x1, y1, x2 - x1 + 1, y2 - y1 + 1


def _recog_crop(img, x, y, w, h):
    """切车牌 -> resize(208x64) -> (hmirror) -> pix_to_ai；
    返回可直接送识别模型的 image；失败返回 None（不打断检测循环）。"""
    try:
        lp_img = img.cut(x, y, w, h)
        if lp_img is None or lp_img.width() <= 2 or lp_img.height() <= 2:
            return None
        rimg = lp_img.resize(RECOG_W, RECOG_H)
        if RECOG_HMIRROR:
            rimg.replace(hmirror=True)
        rimg.pix_to_ai()
        return rimg
    except Exception as e:
        _dbg_once("recog_crop", "exception=%r" % (e,))
        return None


def recognize_frame(img):
    """对一帧 RGB565 画面做 检测 -> 裁剪 -> 逐位识别。
    返回 {"plate": 中文车牌(如 粤B12345), "plate_ascii": YueB12345,
          "confidence", "ts", "error", "det": 检测框数, "ms": 本周期耗时,
          "boxes": [(x,y,w,h), ...] 外扩后的检测框}。
    ⚠️ 本函数不改画面：框/文字由 App.overlay 叠加到"预览帧"上（板载屏与
    IDE 显示的是预览帧，画在识别帧上会看不到）。"""
    t0 = utime.ticks_ms()
    ts = t0
    if INFER_MODE != "A":
        return {"plate": None, "confidence": 0.0, "ts": ts, "error": "no_plate",
                "det": -1, "ms": 0, "boxes": []}
    if not kpu_load():
        return {"plate": None, "confidence": 0.0, "ts": ts,
                "error": "model_unavailable", "det": -1, "ms": 0, "boxes": [],
                "kpu_err": _KPU_ERR or "model unavailable"}
    try:
        _KPU_DET.run_with_output(img)
        lps = _KPU_DET.regionlayer_yolo2() or []
    except Exception as e:
        _dbg_once("detect", "exception=%r" % (e,))
        return {"plate": None, "confidence": 0.0, "ts": ts,
                "error": "detect_error", "det": -1,
                "ms": utime.ticks_diff(utime.ticks_ms(), t0), "boxes": []}
    best = None             # (conf, plate, plate_ascii)，多个框取最高分
    boxes = []              # 外扩后的框，供 overlay 画图
    for lp in lps:
        try:
            x, y, w, h = _extend_box(lp[0], lp[1], lp[2], lp[3], KPU_DET_EXTEND)
            boxes.append((x, y, w, h))
            rimg = _recog_crop(img, x, y, w, h)
            if rimg is None:
                continue
            _KPU_RECOG.run_with_output(rimg)
            out = _KPU_RECOG.lp_recog()
            if not out or len(out) < 2:
                continue
            idx = []
            for row in out:
                idx.append(row.index(max(row)))
            if not (0 <= idx[0] < len(PROVINCES)):
                continue
            chars = []
            nch = min(7, len(idx) - 1)   # 通常 7 位；参考例程只用前 6 位
            for k in range(1, nch + 1):
                i = idx[k]
                chars.append(ADS[i] if 0 <= i < len(ADS) else '?')
            # plate = 中文省份简称 + 字母数字（协议 UTF-8 中文车牌，如 粤B12345）
            ascii_plate = PROVINCES[idx[0]] + ''.join(chars[:6])
            zh = PROVINCE_ZH.get(PROVINCES[idx[0]], PROVINCES[idx[0]])
            plate = zh + ''.join(chars[:6])
            conf = sum(max(row) for row in out) / len(out)
            if best is None or conf > best[0]:
                best = (conf, plate, ascii_plate)
            if conf >= RECOG_CONF_TH:
                break      # 命中即出（框按置信排序）
        except Exception as e:
            _dbg_once("recog_box", "exception=%r" % (e,))
            continue
    ms = utime.ticks_diff(utime.ticks_ms(), t0)
    try:
        gc.collect()       # 切图/缩放临时对象较多，识别完回收一次
    except Exception:
        pass
    if best and best[0] >= RECOG_CONF_TH:
        return {"plate": best[1], "plate_ascii": best[2],
                "confidence": round(best[0], 3),
                "ts": ts, "error": None, "det": len(lps), "ms": ms,
                "boxes": boxes}
    return {"plate": None, "confidence": 0.0, "ts": ts, "error": "no_plate",
            "det": len(lps), "ms": ms, "boxes": boxes}


def recognize(jpeg):
    """一次抓拍识别（0xC1 二进制链路专用）。
    双模型识别需要原始 image（见 recognize_frame）；此处只服务非 A 模式。"""
    ts = utime.ticks_ms()
    if INFER_MODE == "A":
        return {"plate": None, "confidence": 0.0, "ts": ts, "error": "live_only"}
    return {"plate": None, "confidence": 0.0, "ts": ts, "error": "no_plate"}


def decide(result):
    plate = result.get("plate")
    conf = result.get("confidence", 0.0)
    ts = result.get("ts")
    if plate and conf >= RECOG_CONF_TH:
        return True, {"plate": plate, "confidence": round(conf, 3), "ts": ts}
    return False, {"error": result.get("error", "no_plate")}


# ============================ 入口（对应 main.py / App） ============================
class _NullUart:
    """UART 打开失败时的占位：不崩，跑起来看到底哪一步挂了。"""
    def write(self, *a, **k):
        pass
    def any(self):
        return 0
    def read(self, *a, **k):
        return b""


def _open_uart():
    """数据口（LINK="uart" 时用板级串口 UART_ID/921600）。
    LINK="console" 时不需要 UART（走 print 文本），返回 _NullUart 占位。
    """
    try:
        from machine import UART
    except Exception:
        return _NullUart()
    if LINK != "uart":                      # console/其它模式不需要板级 UART
        return _NullUart()
    uid = UART_ID
    try:
        return UART(uid, UART_BAUD, 8, None, 1,
                    timeout=0, read_buf_len=UART_RX_BUF_LEN)
    except TypeError:
        try:
            return UART(uid, UART_BAUD, 8, None, 1, timeout=0)
        except Exception:
            return _NullUart()
    except Exception:
        return _NullUart()


class App:
    def __init__(self):
        self.uart = _open_uart()
        self.parser = FrameParser(on_frame=self.dispatch)
        self.preview = PreviewStream(self.uart.write, PREVIEW_CHUNK)
        self.busy = False
        self.hb_last = utime.ticks_ms()
        self.trigger_last = 0
        self._seqs = {}
        self.stat_last = utime.ticks_ms()
        self.stat_frames = 0
        self.stat_jlen = 0
        self.gc_last = utime.ticks_ms()
        self.recog_last = utime.ticks_ms()
        self.orient_last = utime.ticks_ms()
        self.orient_idx = -1                # -1 → 首次进 ORIENT_PROBE 打 combo=0(原始姿态)
        self.ovl = None                     # 最近一次识别结果，供 overlay 叠加显示

    def tx(self, type_, payload=b""):
        seq = self._seqs.get(type_, 0)
        self._seqs[type_] = (seq + 1) & 0xFFFF
        self.uart.write(encode_frame(type_, seq, payload))

    def dispatch(self, type_, seq, payload):
        if type_ != TYPE_CMD_RECOGNIZE:
            return
        now = utime.ticks_ms()
        if self.busy:
            return
        if utime.ticks_diff(now, self.trigger_last) < TRIGGER_COOLDOWN_MS:
            return
        self.do_recognize()

    def do_recognize(self):
        self.busy = True
        self.trigger_last = utime.ticks_ms()
        try:
            jpeg = capture_jpeg()
            result = recognize(jpeg) if jpeg else {
                "plate": None, "confidence": 0.0,
                "ts": utime.ticks_ms(), "error": "no_image"}
            ok, obj = decide(result)
            if ok:
                self.tx(TYPE_RECOG_RESULT, json.dumps(obj))
            else:
                if jpeg:
                    self.preview.send_jpeg(jpeg, kind=FRAME_KIND_SNAP)
                self.tx(TYPE_RECOG_FAIL, json.dumps(obj))
        finally:
            self.busy = False

    def console_send_jpeg(self, jpeg):
        """零线 console 文本传输：JPEG -> base64 分行走 print（MP157 解析 K2:IMG/K2:END 行）。
        ⚠️ 本固件实测 machine.UART 二进制到不了 USB；console(print) 文本可以（保真/慢）。"""
        try:
            import binascii
            b64 = binascii.b2a_base64(jpeg).decode().rstrip("\n")
        except Exception:
            return
        for i in range(0, len(b64), CONSOLE_CHUNK):
            print("K2:IMG:%d:%s" % (i, b64[i:i + CONSOLE_CHUNK]))
            utime.sleep_ms(CONSOLE_PACE_MS)
        print("K2:END:%d" % len(b64))

    def console_recognize_tick(self):
        """console 模式周期识别：INFER_MODE=A 对实时帧跑本地 KPU 双模型；
        无模型时若 CONSOLE_FAKE_RESULT=1 发假车牌打通 Qt 弹卡/判定链路。结果行:
        K2:OK:<json{plate,confidence,ts}>  /  K2:NG:<json{error}>"""
        if LINK != "console":
            return
        if self.busy:
            return
        if CONSOLE_FAKE_RESULT:
            print("K2:OK:%s" % json.dumps(
                {"plate": CONSOLE_FAKE_PLATE, "confidence": CONSOLE_FAKE_CONF,
                 "ts": utime.ticks_ms()}))
            return
        if INFER_MODE != "A":
            return                          # B 模式无本地识别，不发结果
        img = capture_frame()
        if img is None:
            return
        self.busy = True                    # 识别期间暂停预览发送（与预览流解耦）
        try:
            result = recognize_frame(img)
            self.ovl = result              # 缓存给 overlay（下几帧预览都会叠上）
            ok, obj = decide(result)
            # 每周期一行诊断：det=检测框数(-1=模型不可用/异常) ms=耗时
            # 这行就是"模型到底跑没跑"的直接证据（IDE 串口可见）
            print("[RECOG] det=%s ms=%s %s"
                  % (result.get("det"), result.get("ms"),
                     ("OK plate=%s conf=%s" % (result.get("plate"),
                                               result.get("confidence")))
                     if ok else "NG err=%s%s"
                     % (result.get("error"),
                        (" [" + result["kpu_err"] + "]")
                        if result.get("kpu_err") else "")))
            if ok:
                print("K2:OK:%s" % json.dumps(obj))
            # NG 不发(预览流已在发图; 云兜底/调试需要时再开)
        finally:
            self.busy = False

    def overlay(self, img):
        """把最近一次识别结果叠画到"预览帧"上：绿框=检测到的车牌位置，
        左上角一行 ASCII 状态（板载屏/IDE/Qt 预览都能看到，证明模型在工作）。
        ⚠️ 必须在 lcd_show / compress 之前调用。"""
        if not DRAW_DETECT or img is None or not self.ovl:
            return
        r = self.ovl
        try:
            for b in (r.get("boxes") or []):
                img.draw_rectangle(b[0], b[1], b[2], b[3], color=(0, 255, 0))
            if r.get("plate_ascii"):
                txt = "%s %.2f" % (r["plate_ascii"], r.get("confidence", 0.0))
                col = (255, 0, 0)
            else:
                # 失败时优先显示模型失败原因（一眼定位，不必翻串口）
                txt = r.get("kpu_err") or ("det=%s %s" % (r.get("det"),
                                                          r.get("error") or ""))
                col = (255, 255, 0)
            img.draw_string(4, 4, txt, color=col, scale=1)
        except Exception as e:
            _dbg_once("overlay", "exception=%r" % (e,))

    def orient_probe_tick(self):
        """方向排查：每 ORIENT_PROBE_MS 轮换 (vflip,hmirror) 四种组合并打印编号。
        ⚠️ 走**软件层**（CAM_SW_*）——本固件传感器层 set_vflip/set_hmirror 实测无效，
        只有软件层才能保证画面真的会变。编号同时画在画面上(见 orient_label)。"""
        global CAM_SW_VFLIP, CAM_SW_HMIRROR
        now = utime.ticks_ms()
        if utime.ticks_diff(now, self.orient_last) < ORIENT_PROBE_MS:
            return
        self.orient_last = now
        self.orient_idx = (self.orient_idx + 1) & 3
        CAM_SW_VFLIP = bool(self.orient_idx & 2)
        CAM_SW_HMIRROR = bool(self.orient_idx & 1)
        v = 1 if CAM_SW_VFLIP else 0
        h = 1 if CAM_SW_HMIRROR else 0
        print("[ORIENT] combo=%d vflip=%d hmirror=%d "
              "(0=raw 1=hmirror 2=vflip 3=180)" % (self.orient_idx, v, h))

    def orient_label(self, img):
        """把当前方向组合编号画在画面上（板载屏/IDE 都能看到，免去对日志时间）。"""
        if img is None or self.orient_idx < 0:
            return
        v = 1 if (self.orient_idx & 2) else 0
        h = 1 if (self.orient_idx & 1) else 0
        try:
            img.draw_string(4, 4, "ORIENT %d v%d h%d" % (self.orient_idx, v, h),
                            color=(255, 0, 0), scale=2)
        except Exception:
            pass

    def run(self):
        print("[BOOT] park k210 fw INFER_MODE=%s LINK=%s preview=%d draw=%d probe=%d"
              " sw_vflip=%d sw_hmirror=%d"
              % (INFER_MODE, LINK, CONSOLE_PREVIEW, DRAW_DETECT, ORIENT_PROBE,
                 1 if CAM_SW_VFLIP else 0, 1 if CAM_SW_HMIRROR else 0))
        try:
            cam_init()                      # TODO(实机)：失败可延后重试
        except Exception as e:
            print("[WARN] cam_init failed:", e)
        if INFER_MODE == "A" and not ORIENT_PROBE:
            sd_ensure_mounted()             # 本固件不自动挂 SD → 主动挂一次
            kpu_load()                      # 启动即试加载：SD 文件/固件 API 一览无余
        while True:
            now = utime.ticks_ms()
            if not self.busy:               # 预览流常开（与推理解耦）
                img = capture_frame()
                if ORIENT_PROBE:
                    self.orient_label(img)  # 画组合编号(必须在 lcd_show 之前)
                else:
                    self.overlay(img)       # 叠最近一次识别结果(框+车牌)
                lcd_show(img)               # 先上屏（compress 会原地改写 img）
                # 方向排查/停发预览时不发 base64：省 CPU，且让串口只剩诊断行
                if LINK == "console" and (ORIENT_PROBE or not CONSOLE_PREVIEW):
                    self.stat_frames += 1
                else:
                    jpeg = jpeg_from(img)
                    if jpeg:
                        self.stat_frames += 1
                        self.stat_jlen = len(jpeg)
                        if LINK == "console":
                            self.console_send_jpeg(jpeg)
                        else:
                            self.preview.send_jpeg(jpeg)
            if ORIENT_PROBE:                # 方向排查模式：只出图+编号，不跑识别
                self.orient_probe_tick()
                utime.sleep_ms(20)
                continue
            if LINK == "console" and utime.ticks_diff(now, self.recog_last) >= RECOG_PERIOD_MS:
                self.recog_last = now
                self.console_recognize_tick()
            n = self.uart.any()
            if n:
                data = self.uart.read(n)
                if data:
                    self.parser.feed(data)
            if utime.ticks_diff(now, self.hb_last) >= HEARTBEAT_MS:
                self.hb_last = now
                self.tx(TYPE_HEARTBEAT, bytes([1 if self.busy else 0]))
            if utime.ticks_diff(now, self.stat_last) >= STAT_PERIOD_MS:
                fps = self.stat_frames * 1000 // max(1, utime.ticks_diff(now, self.stat_last))
                _log("[stat] fps=%d jpeg=%dB mem_free=%d"
                     % (fps, self.stat_jlen, gc.mem_free()))
                self.stat_frames = 0
                self.stat_last = now
            if utime.ticks_diff(now, self.gc_last) >= GC_PERIOD_MS:
                self.gc_last = now
                gc.collect()
            utime.sleep_ms(2)


def main():
    app = App()
    app.run()


if __name__ == "__main__":
    main()
