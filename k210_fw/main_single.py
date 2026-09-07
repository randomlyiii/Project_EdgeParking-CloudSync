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

CAM_W, CAM_H = 320, 240    # QVGA（协议约束 ≤320x240）
JPEG_HW = False            # TODO(实机)：sensor pixformat 是否可用硬件 JPEG（sensor.JPEG）
JPEG_QUALITY = 88          # 软件压缩质量；调到单帧 8~15KB

INFER_MODE = "B"           # A=检测+识别 kmodel；B=仅占位（默认，识别交云兜底）
KPU_MODEL = "/sd/plate_det.kmodel"   # TODO(路径A)：实际 kmodel 路径
RECOG_CONF_TH = 0.60       # 低置信判失败（与 Core1 P5-11 同值）
HEARTBEAT_MS = 1000        # 0x7E 心跳周期
TRIGGER_COOLDOWN_MS = 2000 # 两次识别最小间隔


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


def crc16(data):
    """CRC-16/XMODEM。校验向量：crc16(b"123456789") == 0x31C3。"""
    crc = CRC_INIT
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ CRC_POLY) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
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
    # TODO(实机)：核对本板固件 sensor API（MaixPy / CanMV-K210 大体同构）。
    import sensor
    sensor.reset()
    if JPEG_HW:
        sensor.set_pixformat(sensor.JPEG)
    else:
        sensor.set_pixformat(sensor.RGB565)
    table = {(320, 240): sensor.QVGA, (640, 480): sensor.VGA}
    sensor.set_framesize(table.get((CAM_W, CAM_H), sensor.QVGA))
    sensor.run(1)


def capture_jpeg():
    """拍一帧返回 JPEG 字节；失败返回 None（预览与抓拍共用）。"""
    try:
        import sensor
        img = sensor.snapshot()
        if img is None:
            return None
        if JPEG_HW:
            return bytes(img)
        comp = img.compress(quality=JPEG_QUALITY)
        return bytes(comp if comp is not None else img)
    except Exception:
        return None


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
    """数据 UART（与 USB REPL 物理分离）。TODO(实机)：核对固件 machine.UART API/串口号。"""
    try:
        from machine import UART
        try:
            return UART(UART_ID, UART_BAUD, 8, None, 1,
                        timeout=0, read_buf_len=UART_RX_BUF_LEN)
        except TypeError:
            return UART(UART_ID, UART_BAUD, 8, None, 1, timeout=0)
    except Exception as e:
        print("[WARN] UART open failed, preview only:", e)
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

    def run(self):
        try:
            cam_init()                      # TODO(实机)：失败可延后重试
        except Exception as e:
            print("[WARN] cam_init failed:", e)
        while True:
            now = utime.ticks_ms()
            if not self.busy:               # 预览流常开（与推理解耦）
                jpeg = capture_jpeg()
                if jpeg:
                    self.preview.send_jpeg(jpeg)
            n = self.uart.any()
            if n:
                data = self.uart.read(n)
                if data:
                    self.parser.feed(data)
            if utime.ticks_diff(now, self.hb_last) >= HEARTBEAT_MS:
                self.hb_last = now
                self.tx(TYPE_HEARTBEAT, bytes([1 if self.busy else 0]))
            gc.collect()
            utime.sleep_ms(2)


def main():
    app = App()
    app.run()


if __name__ == "__main__":
    main()
