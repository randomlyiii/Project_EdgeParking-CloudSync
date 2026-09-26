# -*- coding: ascii -*-
"""Two-stage bring-up through the production recognizer class.

Board-side integration test (NOT part of host `discover`; needs RK3588 NPU):
    python3 tests/itest_two_stage.py <jpg> [more.jpg ...]
    python3 tests/itest_two_stage.py --det /path/to/det.rknn <jpg> [...]

Prints one JSON line per image:
    {"img": "...", "plate": "chuanA88888", "plate_conf": 0.99,
     "box": [x1, y1, x2, y2], "det_conf": 0.54}
(plate is unicode-escaped: board rule, ASCII-only console output)
"""

import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, "/opt/rk3588_service")

from lpr_server import TwoStageRecognizer  # noqa: E402

DET_MODEL = "/root/models/yolov8s.rknn"
REC_MODEL = "/root/models/lprnet.rknn"


def main():
    args = sys.argv[1:]
    det = DET_MODEL
    if args[:1] == ["--det"]:
        det = args[1]
        args = args[2:]
    assert args, "usage: itest_two_stage.py [--det det.rknn] <jpg> [jpg ...]"
    rec = TwoStageRecognizer(det, REC_MODEL)
    for path in args:
        with open(path, "rb") as fh:
            result = rec.plate(fh.read())
        if result is None:
            print(json.dumps({"img": path, "error": "bad_image"}))
            continue
        plate, conf, info = result
        print(json.dumps({
            "img": path, "plate": plate, "plate_conf": round(conf, 4),
            "box": info.get("box"), "det_conf": info.get("det_conf"),
        }, ensure_ascii=True))


if __name__ == "__main__":
    main()
