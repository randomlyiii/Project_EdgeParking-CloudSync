# -*- coding: utf-8 -*-
"""摄像头采集 + JPEG 编码（P5-03）。CanMV MicroPython。

⚠️ 硬件 API 以实机固件为准：上板第一步先在 CanMV IDE 跑官方 sensor 例程，
把确认后的写法回填到下方 TODO 处（pixformat / JPEG_HW / compress 语义因固件而异）。
"""

from config import CAM_W, CAM_H, JPEG_HW, JPEG_QUALITY


def cam_init():
    # TODO(实机)：核对本板固件的 sensor 模块写法（MaixPy / CanMV-K210 大体同构）。
    import sensor
    sensor.reset()
    if JPEG_HW:
        sensor.set_pixformat(sensor.JPEG)   # 硬件 JPEG：snapshot 直接出 JPEG 流
    else:
        sensor.set_pixformat(sensor.RGB565)  # 软件 JPEG：snapshot 后 compress()
    sensor.set_framesize(_pick_framesize(sensor, CAM_W, CAM_H))
    sensor.run(1)


def _pick_framesize(sensor, w, h):
    table = {(320, 240): sensor.QVGA, (640, 480): sensor.VGA}
    return table.get((w, h), sensor.QVGA)


def capture_jpeg():
    """拍一帧并返回 JPEG 字节；失败返回 None（预览与抓拍共用本入口）。"""
    try:
        import sensor
        img = sensor.snapshot()
        if img is None:
            return None
        if JPEG_HW:
            # 硬件 JPEG 模式：帧缓冲本身就是 JPEG 流
            return bytes(img)
        comp = img.compress(quality=JPEG_QUALITY)
        # TODO(实机)：compress() 在多数固件返回新 image（缓冲=JPEG）；
        # 若返回 None/原地压缩则改 bytes(img)。语义以实测为准。
        return bytes(comp if comp is not None else img)
    except Exception:
        return None
