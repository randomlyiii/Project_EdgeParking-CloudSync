# -*- coding: utf-8 -*-
"""车牌识别（P5-06）：指令触发的单次阻塞推理。

路径 A（INFER_MODE="A"）：KPU kmodel 检测+识别 —— 模型未配/未装即视为识别失败，走云兜底。
路径 B（默认，INFER_MODE="B"）：骨架占位 —— 同样返回失败，抓拍 JPEG 交 Core1 → DeepSeek Vision。

返回 dict：{"plate": str|None, "confidence": float, "ts": int, "error": str|None}
调用方（main.py）用 decide() 决定发 0xC2（成功）还是 0xC3（失败）。
"""
import utime

from config import INFER_MODE, RECOG_CONF_TH


def recognize(jpeg):
    """一次抓拍识别。jpeg：JPEG 字节（预览同源图）。"""
    ts = utime.ticks_ms()
    if INFER_MODE == "A":
        try:
            # TODO(实机/模型)：加载 kmodel → 前处理 → KPU forward → 后处理（检测框→字符识别）。
            # 参考：正点原子 DNK210 CanMV 指南·车牌识别实验；nncase 转换见 P5-06。
            # 填实后在此返回成功 dict，例如：
            #   return {"plate": "苏A12345", "confidence": 0.93, "ts": ts}
            raise NotImplementedError("INFER_MODE=A 待接 kmodel")
        except Exception:
            return {"plate": None, "confidence": 0.0, "ts": ts, "error": "model_unavailable"}
    # 路径 B 占位：真机阶段至少给出"有无车牌/框"，仍按低置信失败交给云端兜底。
    return {"plate": None, "confidence": 0.0, "ts": ts, "error": "no_plate"}


def decide(result):
    """识别决策（P5-07 / P5-11 口径）：
    - 命中且 confidence >= RECOG_CONF_TH → (True, 0xC2 的 JSON dict)
    - 否则 → (False, 0xC3 的 JSON dict)，抓拍 JPEG 由调用方另行以用途=1 上送。
    """
    plate = result.get("plate")
    conf = result.get("confidence", 0.0)
    ts = result.get("ts")
    if plate and conf >= RECOG_CONF_TH:
        return True, {"plate": plate, "confidence": round(conf, 3), "ts": ts}
    return False, {"error": result.get("error", "no_plate")}
