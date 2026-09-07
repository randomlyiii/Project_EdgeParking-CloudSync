# 端侧 AI · 边云协同停车场（100ASK-MP157 + K210）

> 定位：基于 100ASK-MP157（STM32MP157：双核 Cortex-A7 + 单核 Cortex-M4）+ **外接 K210 AI 视觉模块**的停车场**端侧 AI + 边云协同**教学 Demo。
> 双系统：双 A7 运行同一 Linux（SMP），M4 运行 FreeRTOS；K210 运行独立固件（自带摄像头 + KPU）。
> 边云协同两种形态：识别置信度不足时调 **DeepSeek Vision API** 兜底（实时）；云平台数据上报为**阶段 4 可选**（详见 `Task.md` 第八节）。

## 分工一览

| 单元 | 系统 | 一句话职责 |
|---|---|---|
| M4 @209MHz | FreeRTOS | 纯硬件实时层：红外车辆到位检测、CAN 道闸、IO / 故障检测；仅经 RPMSG 与 A7 通信 |
| A7-0 | Linux | 实时业务底座：RPMSG、上位机（Modbus-TCP）、停车场业务逻辑、（可选）SQLite、（可选）云平台上报 |
| A7-1 | Linux | 多媒体 / 图形 / AI 协处理器管理：Qt5 LCD 界面、K210 模块（UART 预览流 + 车牌结果）、DeepSeek 兜底 |
| K210 | CanMV MicroPython 固件 | 自带摄像头：JPEG 预览流**常开**（与推理解耦）+ 指令触发的 KPU 车牌识别（低置信云兜底） |

> 注意：A7-0 / A7-1 是**同一个 Linux 内核下的两个 CPU**，"核隔离"靠内核隔离配置 + 进程 CPU 亲和性实现，不是物理分系统。落地手段见 `Task.md` 第二节。
> 注意：K210 是**多媒体外设**（自带固件与 AI），归 A7-1 直接管理；M4 只保留红外 / CAN / IO 等毫秒级硬实时器件，不做视频中转（理由见 `Task.md` 第一节）。

## 技术栈

- C（core0_service / m4_fw）、C++ Qt5（core1_ui 界面）、K210 固件（CanMV MicroPython，CanMV IDE 开发）
- KPU 车牌识别模型（K210 片内推理，替代原 A7 本地 TFLite）
- DeepSeek Vision API（兜底二次识别，HTTPS / libcurl）
- FreeRTOS + OpenAMP RPMSG（M4↔A7）、UART（A7↔K210，预留 SPI 升级）
- SQLite3（可选）、Modbus-TCP
- 共享内存 + 消息队列 / eventfd（双 A7 通信）

## 硬件清单

- 100ask-mp157 开发板（STM32MP157，双 A7 + M4）
- LCD 屏幕（RGB / LTDC 直连）
- **外接 K210 视觉识别模块**（自带摄像头 + KPU；UART 接 A7，接线与波特率见 `Task.md` 第五节）
- 红外线传感器 ×N（车辆到位检测，M4 GPIO 采集）
- CAN 道闸控制器（M4 CAN 外设控制）
- （可选）车位占用传感器 ×N（IO 电平，M4 采集；缺省用进出计数状态机演示）
- 以太网 + WiFi 模块（上位机 / 云端链路）
- 电源、线材等外围

## 目录结构

```
park_demo/                      # 本仓库根
├── README.md                   # 本文件（总览 / 目录 / 构建）
├── Task.md                     # 系统设计与任务规划（职责范围见文末表格）
├── k210_fw/                    # K210 固件（CanMV MicroPython，CanMV IDE 开发）
│   ├── main.py / boot.py       # 主循环编排（预览流+指令分发+心跳）与上电引导
│   ├── capture.py / preview.py # 摄像头 JPEG 采集、预览流拆帧发送
│   ├── infer.py                # KPU 车牌识别（指令触发；低置信云兜底）
│   ├── uart_proto.py           # 串口协议：分帧、CRC16-XMODEM、指令解析
│   ├── config.py               # 集中参数（波特率/阈值/引脚）
│   └── tools/selftest.py       # PC 端协议自检（无需板子）
├── m4_fw/                      # M4 FreeRTOS 固件（OpenAMP 从端）
│   ├── drivers/                # CAN / IO / 红外传感器驱动
│   ├── rpmsg/                  # 帧收发、解析
│   └── tasks/                  # FreeRTOS 任务（道闸 / 红外触发 / 故障检测）
├── core0_service/              # A7-Core0，C 语言，实时业务底座
│   ├── rpmsg/                  # /dev/ttyRPMSG0 帧收发、粘包处理、心跳（第3步已实现）
│   ├── protocol/               # Modbus-TCP 从站、上位机指令解析
│   ├── business/               # 停车场业务逻辑
│   ├── storage/                # SQLite3 落盘（可选，默认关闭）
│   ├── ipc_shm/                # 与 Core1 共享内存 + 事件通知
│   ├── cloud/                  # 云平台上报（阶段4可选）
│   ├── tools/                  # rpmsg_cli 打桩工具 + load_m4.sh（remoteproc 装载）
│   └── Makefile
├── core1_ui/                   # A7-Core1，C/C++ Qt5
│   ├── k210_link/              # K210 串口前端：预览流重组、抓拍指令、结果解析
│   ├── cloud_api/              # DeepSeek Vision 兜底（HTTPS / libcurl）
│   ├── qt_gui/                 # LCD 界面（预览视频 / 车牌 / 车位状态）
│   └── CMakeLists.txt
├── docs/
│   └── protocols.md            # 接口协议规格（字段级，正式维护副本）：
│                               # CAN / K210 UART / RPMSG 已拷入；Modbus /
│                               # 共享结构体随各步实现拷入（母本 PhaseMd/10）
└── deploy/
    └── systemd/                # 开机自启 unit（M4 装载 / core0 / core1 / k210_link）
```

## 构建与运行（概览）

1. **M4 固件**：编译出 `.elf` → 放入板卡 `/lib/firmware/` → 由 Linux remoteproc 启动 M4，且必须在 A7 业务之前就绪（启动顺序细节见 `Task.md` 第三节）。
2. **K210 固件**（`k210_fw/`）：CanMV IDE 通过 USB 连接板子，上传 .py 模块到 flash（boot→main 上电自启）；固件本体出厂已带 CanMV MicroPython（仅升级固件才需 kflash）。上电即常开输出 JPEG 预览流（接线与帧协议见 `Task.md` 第五节 / `docs/protocols.md`）。
3. **Linux 侧**：`core0_service`（Makefile）、`core1_ui`（CMake）用 ST SDK 交叉编译后部署。
4. **上电自启**：`deploy/systemd/` 下的 unit 负责 M4 装载与各进程启动；各进程的核绑定（taskset / systemd CPUAffinity）细节见 `Task.md` 第二节。
5. **接口规格**：帧格式、Modbus 寄存器映射、共享内存结构体、K210 串口帧统一维护在 `docs/protocols.md`，四端（M4 / core0 / core1 / K210）共用。

## 文档分工（内容隔离）

| 文件 | 职责范围 |
|---|---|
| `README.md`（本文件） | 项目定位、分工一览、硬件清单、目录结构、构建与运行入口 |
| `Task.md` | 系统架构、任务拆解与依赖、数据交互、约束避坑、迭代计划、验收 KPI |

两份文档内容不重复；交叉处只做链接引用，字段级协议规格统一归属 `docs/protocols.md`（2026-09-07 已建）。
