# -*- coding: utf-8 -*-
"""集中参数（对应 PhaseMd/06 P5 各步调优点；帧级定义不在此，见 docs/protocols.md §2）。

首次上板核对区：UART 引脚/串口号/API、sensor 像素格式、JPEG 语义 —— 都是硬件相关 TODO。
"""

# ---- 串口链路（P5-02 / P5-14）----
UART_ID = 1                    # TODO(实机)：以板子可用串口为准（正点原子 DNK210 等常见 1/2/3）
UART_BAUD = 921600             # 起步值；实测误码可退回 460800，定值记入 P5-14
UART_RX_BUF_LEN = 4096         # 固件支持 read_buf_len 时生效
PAYLOAD_MAX = 1024             # 协议单帧 payload 上限（≤1KB）
PREVIEW_CHUNK = 900            # 预览块 payload（<PAYLOAD_MAX）

# ---- 预览流（P5-03 / P5-04）----
CAM_W, CAM_H = 320, 240        # QVGA（协议约束 ≤320x240）
JPEG_HW = False                # TODO(实机)：sensor pixformat 是否可用硬件 JPEG（sensor.JPEG）
JPEG_QUALITY = 88              # 软件压缩质量；调到单帧 8~15KB
PREVIEW_TARGET_FPS = 5         # KPI 下限（带宽核算见 P5-03）

# ---- 识别（P5-06 / P5-07）----
INFER_MODE = "B"               # A=检测+识别 kmodel；B=仅占位（默认，识别交云兜底）
KPU_MODEL = "/sd/plate_det.kmodel"   # TODO(路径A)：实际 kmodel 路径（flash 根或 /sd）
RECOG_CONF_TH = 0.60           # 低置信判失败（与 Core1 P5-11 同值；第7步联动云兜底）

# ---- 心跳 / 触发冷却（P5-04 / P5-10）----
HEARTBEAT_MS = 1000            # 0x7E 心跳周期
TRIGGER_COOLDOWN_MS = 2000     # 两次识别最小间隔（与 M4/C8T6 冷却双保险）
