# -*- coding: ascii -*-
"""Convert HyperLPR3 onnx (det + rec) to FP16 RKNN for RK3588. Runs ON THE
BOARD (rknn-toolkit2 aarch64 is installed there; PC pythons are 3.13/3.14,
toolkit2 wheels stop at cp311). Usage:
    python3 convert_hyperlpr3_rknn.py y5fu_640x_sim.onnx rpv3_mdict_160_r3.onnx
Outputs: y5fu_640x.rknn / rpv3_mdict_160_r3.rknn next to this script.
"""
import os
import sys

from rknn.api import RKNN


def convert(src, dst, mean, std):
    r = RKNN(verbose=False)
    r.config(mean_values=[[mean] * 3], std_values=[[std] * 3],
             target_platform="rk3588")
    assert r.load_onnx(model=src) == 0, "load_onnx failed: %s" % src
    assert r.build(do_quantization=False) == 0, "build failed: %s" % src
    assert r.export_rknn(dst) == 0, "export failed: %s" % dst
    print("convert: %s -> %s (%d bytes)"
          % (src, dst, os.path.getsize(dst)), flush=True)


def main():
    det = sys.argv[1] if len(sys.argv) > 1 else "y5fu_640x_sim.onnx"
    rec = sys.argv[2] if len(sys.argv) > 2 else "rpv3_mdict_160_r3.onnx"
    det_out = os.path.splitext(os.path.basename(det))[0].replace("_sim", "") + ".rknn"
    rec_out = os.path.splitext(os.path.basename(rec))[0] + ".rknn"
    # det: image_to_input_tensor /255 -> mean 0, std 255
    convert(det, det_out, 0.0, 255.0)
    # rec: encode_images (x-127.5)/127.5 -> mean/std 127.5
    convert(rec, rec_out, 127.5, 127.5)
    print("ALL_CONVERTED", flush=True)


if __name__ == "__main__":
    main()
