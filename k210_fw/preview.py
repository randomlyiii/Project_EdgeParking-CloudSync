# -*- coding: utf-8 -*-
"""预览流发送（P5-04）：JPEG → 0x01 分块 + 0x02 帧尾（4B 总长 LE + 1B 用途）。

seq 用单调预览计数器（0x01/0x02 共用，接收端靠跳变丢帧），见 docs/protocols.md §2。
推理期间由 main 暂停调用本模块（KPU 资源竞争），恢复后自动续流。
"""

from uart_proto import (encode_frame, TYPE_PREVIEW_CHUNK, TYPE_PREVIEW_TAIL,
                        FRAME_KIND_PREVIEW, FRAME_KIND_SNAP)


class PreviewStream:
    """send 为注入的帧写出回调（main 里绑 uart.write），便于协议层 PC 单测。"""

    def __init__(self, send, chunk_payload):
        self.send = send
        self.chunk = chunk_payload
        self._seq = 0

    def seq(self):
        return self._seq

    def send_jpeg(self, jpeg, kind=FRAME_KIND_PREVIEW):
        """发送一帧 JPEG；kind=FRAME_KIND_SNAP 时该帧为"识别抓拍帧"（供云兜底）。"""
        if not jpeg:
            return
        for off in range(0, len(jpeg), self.chunk):
            self._frame(TYPE_PREVIEW_CHUNK, jpeg[off:off + self.chunk])
        tail = len(jpeg).to_bytes(4, "little") + bytes([kind])
        self._frame(TYPE_PREVIEW_TAIL, tail)

    def _frame(self, type_, payload):
        self.send(encode_frame(type_, self._seq, payload))
        self._seq = (self._seq + 1) & 0xFFFF
