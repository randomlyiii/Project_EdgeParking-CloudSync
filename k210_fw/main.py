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
JPEG_QUALITY = 70         # 软件压缩质量；QVGA 下约 15~40KB/帧（QQVGA 下 3~6KB）
LCD_PREVIEW = False        # 板载屏本地预览（helloworld 同款：QVGA 满屏显示）
# ⚠️ 实测(2026-09-07)：本固件 USB 只走 console(print) 文本；machine.UART 二进制帧到不了 USB。
# LINK="console"=走 console 打印 base64 文本行（零线、必通、较慢，QVGA 约 0.5~1s/帧）；
# LINK="uart"=板级串口 UART_ID（预留，需杜邦线）。
LINK = "console"
CONSOLE_CHUNK = 600       # console 模式：每行 base64 字符数
CONSOLE_PACE_MS = 40      # 每行间延时：115200 下 600B 约需 55ms 发送，保守取 40ms+（丢行就调大）

INFER_MODE = "B"           # A=检测+识别 kmodel；B=仅占位（默认，识别交云兜底）
KPU_MODEL = "/sd/plate_det.kmodel"   # TODO(路径A)：实际 kmodel 路径
RECOG_CONF_TH = 0.60       # 低置信判失败（与 Core1 P5-11 同值）
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
    """snapshot 一帧，返回 image 对象（未压缩）；失败返回 None。"""
    try:
        import sensor
        return sensor.snapshot()
    except Exception:
        return None


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
def recognize(jpeg):
    """一次抓拍识别；返回 {"plate","confidence","ts","error"}。"""
    ts = utime.ticks_ms()
    if INFER_MODE == "A":
        try:
            # TODO(实机/模型)：加载 kmodel → 前处理 → KPU forward → 后处理。
            raise NotImplementedError("INFER_MODE=A 待接 kmodel")
        except Exception:
            return {"plate": None, "confidence": 0.0, "ts": ts, "error": "model_unavailable"}
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

    def run(self):
        try:
            cam_init()                      # TODO(实机)：失败可延后重试
        except Exception as e:
            print("[WARN] cam_init failed:", e)
        while True:
            now = utime.ticks_ms()
            if not self.busy:               # 预览流常开（与推理解耦）
                img = capture_frame()
                lcd_show(img)               # 先上屏（compress 会原地改写 img）
                jpeg = jpeg_from(img)
                if jpeg:
                    self.stat_frames += 1
                    self.stat_jlen = len(jpeg)
                    if LINK == "console":
                        self.console_send_jpeg(jpeg)
                    else:
                        self.preview.send_jpeg(jpeg)
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
