# -*- coding: ascii -*-
"""Convert yolov8s.onnx -> yolov8s.rknn on the RK3588 (rknn-toolkit2).

Runs ON THE BOARD (needs rknn.api, installed with rknn-toolkit2 2.3.2):
    python3 convert_yolov8s.py yolov8s.onnx yolov8s.rknn

Config follows the rknn_model_zoo yolov8 convention: NHWC uint8 input,
normalization (x/255) baked into the model via mean 0 / std 255, FP16
(no quantization -> no calibration dataset needed).
"""

import sys

from rknn.api import RKNN


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "yolov8s.onnx"
    dst = sys.argv[2] if len(sys.argv) > 2 else "yolov8s.rknn"
    rknn = RKNN(verbose=False)
    rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]],
                target_platform="rk3588")
    assert rknn.load_onnx(model=src) == 0, "load_onnx failed"
    assert rknn.build(do_quantization=False) == 0, "build failed"
    assert rknn.export_rknn(dst) == 0, "export_rknn failed"
    print("converted: %s -> %s" % (src, dst))


if __name__ == "__main__":
    main()
