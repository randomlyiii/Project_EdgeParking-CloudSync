# -*- coding: utf-8 -*-
"""PC 端协议自检（P5-02 验收钩子，无需板子）：
    python tools/selftest.py

校验内容：
  1. CRC16-XMODEM 标准校验向量（b"123456789" -> 0x31C3）；
  2. 组帧→逐字节喂入 FrameParser 完整还原；
  3. 坏 CRC / 垃圾头 丢帧重同步；
  4. 0x02 帧尾示例（4B 总长 LE + 1B 用途）与 payload 超限防护。
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import uart_proto as up


def test_crc_vector():
    assert up.crc16(b"123456789") == 0x31C3, "CRC-16/XMODEM 校验向量不符"
    print("[ok] CRC16-XMODEM 向量 0x31C3")


def test_roundtrip():
    p = up.FrameParser()
    frames = []
    for t, s, payload in [(up.TYPE_PREVIEW_CHUNK, 0, b"\x00" * 900),
                          (up.TYPE_PREVIEW_TAIL, 1, (1000).to_bytes(4, "little") + bytes([up.FRAME_KIND_SNAP])),
                          (up.TYPE_HEARTBEAT, 0, b"\x01"),
                          (up.TYPE_RECOG_RESULT, 0, b'{"plate":"X"}')]:
        frames.append((t, s, payload))
    stream = b"".join(up.encode_frame(t, s, pl) for t, s, pl in frames)

    # 模拟串口：每次喂 1~7 字节（覆盖半帧/跨帧/对齐边界）
    p2 = up.FrameParser()
    got = []
    i, step = 0, 1
    while i < len(stream):
        got += p2.feed(stream[i:i + step])   # 帧可能在本次喂入时就解析完成
        got += p2.feed(b"")                  # 喂空串只为冲出缓冲残余
        i += step
        step = (step % 7) + 1
    assert got == frames, "拆帧结果与组帧不一致"
    print("[ok] 组帧/拆帧往返一致（含半帧与跨帧边界）")


def test_corruption_resync():
    good = up.encode_frame(up.TYPE_HEARTBEAT, 3, b"\x00")
    bad = bytearray(good)
    bad[-1] ^= 0xFF                      # 破坏 CRC
    garbage = b"\xAA\x00\x11\x22"        # 垃圾头
    p = up.FrameParser()
    got = p.feed(bytes(bad)) + p.feed(garbage) + p.feed(good)
    assert got == [(up.TYPE_HEARTBEAT, 3, b"\x00")], "坏帧后未能重同步收好帧"
    print("[ok] 坏 CRC + 垃圾头被丢，后续好帧正常接收")


def test_tail_example():
    p = up.FrameParser()
    jpeg = bytes(range(256)) * 4         # 1024B 假 JPEG
    tail = len(jpeg).to_bytes(4, "little") + bytes([up.FRAME_KIND_SNAP])
    frames = p.feed(up.encode_frame(up.TYPE_PREVIEW_TAIL, 5, tail))
    assert len(frames) == 1 and frames[0][0] == up.TYPE_PREVIEW_TAIL
    t, s, pl = frames[0]
    assert s == 5 and int.from_bytes(pl[:4], "little") == 1024 and pl[4] == up.FRAME_KIND_SNAP
    print("[ok] 0x02 帧尾：4B 总长(LE) + 1B 用途")


def test_payload_limit():
    p = up.FrameParser(max_payload=1024)
    big = up.encode_frame(up.TYPE_PREVIEW_CHUNK, 0, b"\x00" * 2048)  # 超限帧（不该出现于线上）
    got = p.feed(big + up.encode_frame(up.TYPE_HEARTBEAT, 9, b"\x00"))
    assert got == [(up.TYPE_HEARTBEAT, 9, b"\x00")], "超限帧应被丢弃且不吞后续好帧"
    print("[ok] payload 超限防护")


if __name__ == "__main__":
    test_crc_vector()
    test_roundtrip()
    test_corruption_resync()
    test_tail_example()
    test_payload_limit()
    print("\nPASS: K210 UART 协议层 PC 自检全部通过")
