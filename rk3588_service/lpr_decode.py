# -*- coding: ascii -*-
"""LPRNet CTC greedy decode - pure python, zero third-party deps.

Shared by lpr_server.py (RK3588) and the host unit tests. The character
table MUST match the dictionary the deployed lprnet model was trained with;
the model used in this project is the common CCPD LPRNet export
(input 1x3x24x94, output 1x68x18, 67 symbols + CTC blank at index 67).
"""

# 31 provinces + 10 digits + 24 letters (no I, no O) + 2 special = 67.
# CALIBRATED 2026-09-26 against lprnet.onnx on the RK3588 with a real
# plate crop (chuan A 88888): class 22=chuan, 41=A, 39='8' fixes the
# digits-before-letters order; blank is index 67. Indices 65/66 are the
# rare-plate tail; if such a plate ever decodes wrong, swap "shi ling"
# for "gang ao" (see README "decode table").
CHARS = (
    "\u4eac\u6d25\u6caa\u6e1d\u5180\u664b\u8499\u8fbd\u5409\u9ed1"
    "\u82cf\u6d59\u7696\u95fd\u8d63\u9c81\u8c6b\u9102\u6e58\u7ca4"
    "\u6842\u743c\u5ddd\u8d35\u4e91\u85cf\u9655\u7518\u9752\u5b81"
    "\u65b0"
    "0123456789"
    "ABCDEFGHJKLMNPQRSTUVWXYZ"
    "\u4f7f\u9886"
)
BLANK_INDEX = len(CHARS)          # 67
NUM_CLASSES = len(CHARS) + 1      # 68

import math


def _softmax(row):
    m = max(row)
    ex = [math.exp(x - m) for x in row]
    s = sum(ex)
    return [e / s for e in ex]


def _as_time_major(logits):
    """Accept either (T, C) or (C, T) nested lists; return list of T rows."""
    if not logits:
        return []
    if len(logits) == NUM_CLASSES and len(logits[0]) != NUM_CLASSES:
        # (C, T) -> transpose
        return [[logits[c][t] for c in range(len(logits))]
                for t in range(len(logits[0]))]
    return [list(r) for r in logits]


def ctc_greedy_decode(logits, blank=BLANK_INDEX, chars=CHARS):
    """Greedy CTC decode.

    logits: nested lists, (T, C) or (C, T); raw scores or probs per class.
    Returns (plate, conf, seq):
      plate - decoded string (consecutive duplicates merged, blank dropped)
      conf  - mean per-timestep probability of the selected class (0..1)
      seq   - list of per-timestep argmax class indices (diagnostics)
    """
    rows = _as_time_major(logits)
    if not rows:
        return "", 0.0, []
    out = []
    seq = []
    probs = []
    prev = -1
    for row in rows:
        if len(row) < NUM_CLASSES:
            raise ValueError("row too short: %d < %d" % (len(row), NUM_CLASSES))
        p = _softmax(row)
        bi = max(range(len(row)), key=lambda i: row[i])
        seq.append(bi)
        probs.append(p[bi] if bi < len(p) else 0.0)
        if bi != blank and bi != prev and bi < len(chars):
            out.append(chars[bi])
        prev = bi
    conf = sum(probs) / len(probs) if probs else 0.0
    return "".join(out), conf, seq
