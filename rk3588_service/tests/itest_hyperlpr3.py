# -*- coding: ascii -*-
"""HyperLPR3 regression through the production engine wrapper.

Board-side only (models in ~/.hyperlpr3; needs hyperlpr3 + onnxruntime):
    python3 tests/itest_hyperlpr3.py <img> [img ...]

Prints one JSON line per image with the best hit:
    {"img": "...", "plate": "jingAD06088", "conf": 0.99,
     "box": [x1, y1, x2, y2], "ptype": 3}
(unicode-escaped: ASCII-only console output)
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, "/opt/rk3588_service")

from lpr_server import HyperLpr3Recognizer  # noqa: E402


def main():
    paths = sys.argv[1:]
    assert paths, "usage: itest_hyperlpr3.py <img> [img ...]"
    rec = HyperLpr3Recognizer()
    for path in paths:
        with open(path, "rb") as fh:
            result = rec.plate(fh.read())
        if result is None:
            print(json.dumps({"img": path, "error": "bad_image"}))
            continue
        plate, conf, info = result
        print(json.dumps({
            "img": path, "plate": plate, "conf": round(conf, 4),
            "box": info.get("box"), "ptype": info.get("ptype"),
        }, ensure_ascii=True))


if __name__ == "__main__":
    main()
