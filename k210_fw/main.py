# -*- coding: utf-8 -*-
"""K210 固件入口（P5-04/05/07 编排）。上电后 main.py 自启（CanMV MicroPython）。

单协程主循环：
  1) 预览流常开：非 busy 时 snapshot→拆帧发送（推理期间短暂停顿属预期，见 P5-04）；
  2) UART 收帧：FrameParser 拆帧分发 —— 0xC1 抓拍识别（busy/冷却中丢弃）；
  3) 识别一次触发一次结果：成功 0xC2 JSON；
     失败/低置信 → 先发抓拍 JPEG（0x01/0x02 用途=1）再 0xC3 失败 JSON（云兜底）；
  4) 1s 心跳 0x7E（bit0=busy）。

帧级定义见 docs/protocols.md §2。板级差异点（UART 串口号/API、sensor 语义）集中标注 TODO。
"""
import gc
import json
import sys
import utime

# CanMV IDE「运行」只投递当前文件；若多文件模块在 flash/SD 而 sys.path 不含它们，
# import 会失败。把常见模块根路径补进 sys.path（不存在则忽略，无害）。
for _p in ("/flash", "/sd"):
    if _p not in sys.path:
        sys.path.append(_p)

from config import (UART_ID, UART_BAUD, UART_RX_BUF_LEN, PREVIEW_CHUNK,
                    HEARTBEAT_MS, TRIGGER_COOLDOWN_MS)
from uart_proto import (FrameParser, encode_frame, FRAME_KIND_SNAP,
                        TYPE_HEARTBEAT, TYPE_CMD_RECOGNIZE,
                        TYPE_RECOG_RESULT, TYPE_RECOG_FAIL)
from preview import PreviewStream
import capture
import infer


def _open_uart():
    """数据 UART（与 USB REPL 物理分离）。TODO(实机)：核对固件 UART 打开方式/串口号。"""
    from machine import UART
    try:
        return UART(UART_ID, UART_BAUD, 8, None, 1,
                    timeout=0, read_buf_len=UART_RX_BUF_LEN)
    except TypeError:
        return UART(UART_ID, UART_BAUD, 8, None, 1, timeout=0)


class App:
    def __init__(self):
        self.uart = _open_uart()
        self.parser = FrameParser(on_frame=self.dispatch)
        self.preview = PreviewStream(self.uart.write, PREVIEW_CHUNK)
        self.busy = False
        self.hb_last = utime.ticks_ms()
        self.trigger_last = 0
        self._seqs = {}          # 各 type 独立 seq（0x01/0x02 预览计数在 PreviewStream 内）

    # ---- 发送 ----
    def tx(self, type_, payload=b""):
        seq = self._seqs.get(type_, 0)
        self._seqs[type_] = (seq + 1) & 0xFFFF
        self.uart.write(encode_frame(type_, seq, payload))

    # ---- 收帧分发（P5-05）----
    def dispatch(self, type_, seq, payload):
        if type_ != TYPE_CMD_RECOGNIZE:
            return                          # 未知下行 type：忽略
        now = utime.ticks_ms()
        if self.busy:
            return                          # 识别中：丢弃新触发（P5-05 busy 策略）
        if utime.ticks_diff(now, self.trigger_last) < TRIGGER_COOLDOWN_MS:
            return                          # 冷却中：丢弃
        self.do_recognize()

    # ---- 识别（P5-06/07）----
    def do_recognize(self):
        self.busy = True
        self.trigger_last = utime.ticks_ms()
        try:
            jpeg = capture.capture_jpeg()   # 抓拍（预览短暂停顿）
            result = infer.recognize(jpeg) if jpeg else {
                "plate": None, "confidence": 0.0,
                "ts": utime.ticks_ms(), "error": "no_image"}
            ok, obj = infer.decide(result)
            if ok:
                self.tx(TYPE_RECOG_RESULT, json.dumps(obj))
            else:
                if jpeg:
                    self.preview.send_jpeg(jpeg, kind=FRAME_KIND_SNAP)  # 云兜底用抓拍图
                self.tx(TYPE_RECOG_FAIL, json.dumps(obj))
        finally:
            self.busy = False

    # ---- 主循环 ----
    def run(self):
        capture.cam_init()                  # TODO(实机)：失败时可延后重试
        while True:
            now = utime.ticks_ms()

            if not self.busy:               # 预览流常开（与推理解耦）
                jpeg = capture.capture_jpeg()
                if jpeg:
                    self.preview.send_jpeg(jpeg)

            n = self.uart.any()
            if n:
                data = self.uart.read(n)
                if data:
                    self.parser.feed(data)  # dispatch 由回调触发

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
