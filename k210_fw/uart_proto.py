# -*- coding: utf-8 -*-
"""K210 ↔ A7 UART 帧协议层 —— 纯 Python，无板级依赖，PC 可直接自检。

帧格式权威定义：docs/protocols.md §2（草案母本 PhaseMd/10）。
| 帧头 0xAA 0x55 | type(1B) | seq(2B LE) | len(2B LE) | payload(len B) | CRC16(2B LE) |
CRC16 = XMODEM（poly 0x1021 / init 0x0000 / 不反射 / 无异或），校验范围 type..payload。
seq 规则：按 type 通道独立计数；预览 0x01/0x02 共用单调预览计数器，其余 type 各自从 0 起。
"""

HEADER = b"\xAA\x55"

# type 定义（K210 UART 通道，与 docs/protocols.md §2 一致）
TYPE_PREVIEW_CHUNK = 0x01   # ↑ JPEG 分块
TYPE_PREVIEW_TAIL = 0x02    # ↑ 帧尾：4B 本帧总长(LE) + 1B 用途
TYPE_CMD_RECOGNIZE = 0xC1   # ↓ 抓拍识别（payload 空）
TYPE_RECOG_RESULT = 0xC2    # ↑ 识别结果 JSON
TYPE_RECOG_FAIL = 0xC3      # ↑ 识别失败 JSON（≤1KB）
TYPE_HEARTBEAT = 0x7E       # ↔ 心跳/状态（payload 1B：bit0 busy）

# 0x02 用途字段
FRAME_KIND_PREVIEW = 0      # 预览帧
FRAME_KIND_SNAP = 1         # 识别抓拍帧（供云兜底）

MIN_FRAME = 9               # 最小帧长：2 头 + 1 type + 2 seq + 2 len + 2 crc
CRC_POLY = 0x1021
CRC_INIT = 0x0000


def crc16(data):
    """CRC-16/XMODEM。标准校验向量：crc16(b"123456789") == 0x31C3。"""
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
    """打包一帧。seq 由调用方按 type 通道维护（见协议 seq 规则）。"""
    body = (bytes([type_])
            + int(seq & 0xFFFF).to_bytes(2, "little")
            + len(payload).to_bytes(2, "little")
            + bytes(payload))
    return HEADER + body + crc16(body).to_bytes(2, "little")


class FrameParser:
    """字节流 → 完整帧列表；坏帧/坏 CRC 丢 1 字节重同步（容忍丢块）。

    用法：parser = FrameParser(on_frame=None)
          frames = parser.feed(uart.read(n))      # 返回 [(type, seq, payload), ...]
    或传入 on_frame(type, seq, payload) 回调，feed 时自动逐帧调用。
    """

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
                del self._buf[0]          # 未对齐帧头：丢 1B 重找
                continue
            plen = int.from_bytes(self._buf[5:7], "little")
            if plen > self.max_payload:   # 长度越界 = 误同步：丢 1B 重找
                del self._buf[0]
                continue
            need = 7 + plen + 2
            if n < need:
                break                     # 帧未收齐，等后续字节
            body = bytes(self._buf[2:7 + plen])
            crc = int.from_bytes(self._buf[7 + plen:9 + plen], "little")
            if crc16(body) != crc:
                del self._buf[0]          # CRC 错：丢 1B 重同步
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
