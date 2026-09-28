# 基于 RK3588 + STM32MP157 的端云协同车牌识别系统

>个人玩玩瑞芯微开发板项目，禁止商用，出事概不负责哈。
> 定位：基于 100ASK-MP157（STM32MP157：双核 Cortex-A7 + 单核 Cortex-M4）+ **RK3588（定昌 DC-A588，NPU 边缘识别节点）** + **USB 摄像头（UVC，直插 RK3588；K210 为备选采集源，已归档 `other/k210_fw/`）**的停车场**端侧 AI + 边云协同**学习 Demo。
> 双系统：双 A7 运行同一 Linux（SMP），M4 运行 FreeRTOS；RK3588 运行 Ubuntu（V4L2 采集 + HyperLPR3-RKNN NPU 识别）；K210（CanMV 固件）为备选采集源。
> 边云协同两种形态：识别置信度不足时调 **DeepSeek Vision API** 兜底（实时）；云平台数据上报为**阶段 4 可选**。
> 状态：**全链路初步验收已通过（2026-09-28，实拍存档 `验收IMG/`）**。

## 分工一览

| 单元 | 系统 | 一句话职责 |
|---|---|---|
| M4 @209MHz | FreeRTOS | 纯硬件实时层：CAN 道闸、C8T6 节点管理；仅经 RPMSG 与 A7 通信 |
| C8T6 | FreeRTOS | 下位机节点：BH1750 遮光检测（车到位）、SG90 道闸、OLED；CAN 2.0 500k 挂 M4 |
| A7-0 | Linux | 实时业务底座：RPMSG、停车场业务逻辑、白名单、（可选）轻量记录 |
| A7-1 | Linux | 图形 / 云端：Qt5 LCD 界面（park_ui，状态栏 FPS 芯片）、相机上行流消费、DeepSeek 兜底（`enabled=` 总开关） |
| RK3588 | Ubuntu 24.04 | **车牌识别主责**：USB 摄像头 V4L2 采集（640x480@30 MJPG）+ HyperLPR3→RKNN（320 档检测+识别 NPU 合计 ~18ms/帧）+ TCP :8089 中继回 MP157（LCD 实测最高 27fps）；HTTP 测试台 :8088；`--engine lprnet` 回退 |
| K210（备选） | CanMV MicroPython 固件 | 纯图像采集上行（相机→JPEG→`K2:IMG/END` 行流，支持 `CAM_CROP` 固件取景框）；`edge_hub --source /dev/ttyACM0` 一键切换 |

> 数据流：USB 摄像头 →(V4L2)→ **RK3588 edge_hub**（K210 备选源同一出口） →(以太网 TCP 行流：30fps 预览帧 + `K2:OK/NG` 结果 + 日志)→ MP157 park_ui →(shm)→ core0 →(RPMSG/CAN)→ 道闸。
> 注意：A7-0 / A7-1 是**同一个 Linux 内核下的两个 CPU**，"核隔离"靠内核隔离配置 + 进程 CPU 亲和性实现，不是物理分系统。
> 网络：MP157 云端链路 = 板载 WiFi（唯一）；MP157↔RK3588 = 以太网直连（eth0 192.168.10.1/24 ↔ 192.168.10.2/24，主机名 `rk3588`），两条链路互不相干。

## 技术栈

- C（core0_service / m4_fw / c8t6）、C++ Qt5（core1_ui 界面，qmake + book SDK 交叉编译）
- K210 固件（CanMV MicroPython，CanMV IDE 开发）：2026-09-21 起为**纯发图固件**（`BUILD=2026-09-21-imgonly`），旧 KPU 识别固件归档在 `other/k210_fw/tmp_kpu_old/`
- HyperLPR3 车牌识别（→ RKNN，RK3588 NPU：320 档检测+识别合计 ~18ms/帧；`--engine lprnet` 回退；替代 K210 本地 KPU）
- DeepSeek Vision API（兜底二次识别，HTTPS；`transport=python` 通道已真机验证）
- FreeRTOS + OpenAMP RPMSG（M4↔A7）、经典 CAN 2.0 500k（C8T6↔M4）
- 共享内存 `/park_shm` v3 + 事件字（双 A7 通信）

## 硬件清单

- 100ask-mp157 开发板（STM32MP157，双 A7 + M4）+ LCD 屏
- **RK3588（定昌 DC-A588）**：8G RAM / 64G eMMC / 双千兆网口 / NPU；网线直连 MP157 以太网
- **USB 摄像头**（UVC 免驱，杂牌即可；`v4l2-ctl --list-devices` 认节点）：直插 **RK3588**——2026-09-27 起为**主采集源**（V4L2）；K210 视觉模块为备选（原方案 2，已归档 `other/k210_fw/`）
- C8T6 下位机节点（BH1750 遮光 + SG90 道闸 + OLED，TJA1050 挂 CAN）
- WiFi 模块（MP157 板载；云端**唯一网络链路**）
- 电源、线材等外围

## 目录结构

```
Project_EdgeParking-CloudSync/  # 本仓库根
├── docs/                       # 项目文档归口（**新增 .md 一律放这里**，见 AGENTS.md §一）
│   ├── protocols.md            # 接口协议规格（字段级，正式维护副本）：
│   │                           # CAN §1 / K210 UART §2 / RPMSG §3 / park_shm §4 / 云端 §5 / MP157↔RK3588 §6
│   ├── deploy.md               # 板端部署层：systemd 开机链全链路起动顺序 + 结构说明
│   ├── rpmsg.md                # Linux(A7)↔M4 OpenAMP/RPMSG 通讯记录（M4 侧文件地图 / CubeMX 步骤 / 根因修正）
│   ├── 接线.md                  # 整机接线图（MP157 / RK3588 / C8T6 / 摄像头 / 网络）
│   └── AGENTS_history.md       # AGENTS.md 历史归档（全量快照 + 索引）
├── m4_fw/                      # M4 FreeRTOS 固件（OpenAMP 从端，FDCAN2 网关 + rpmsg_bridge）
├── c8t6/                       # C8T6 下位机（CubeIDE；sw_i2c OLED/BH1750 + CAN 从节点 + SG90；can.md = CAN 协议约定）
├── rk3588_service/             # RK3588 边缘识别服务（Python，纯 ASCII）
│   ├── edge_hub.py             # 主服务：V4L2 摄像头/K210 行流（--source 切换）+ 周期识别 + TCP :8089 中继
│   ├── hyperlpr3_rknn.py       # HyperLPR3 RKNN 引擎（anchor-free 检测解码 + RPNet v3 CTC 解码，--engine hyperlpr3rknn）
│   ├── motion_watch.py         # 帧差运动门控（画面静止时识别休眠、运动即唤醒）
│   ├── plate_vote.py           # 连续帧识别结果投票（--votes 2）
│   ├── lpr_server.py           # HTTP 测试台：POST /recognize（raw JPEG→JSON）、GET /health
│   ├── lpr_decode.py           # LPRNet CTC 解码 + 字符表（67+blank，2026-09-26 实车标定；lprnet 回退引擎用）
│   ├── yolo_decode.py          # 检测器解码（yolov8s 两段式回退用）
│   ├── edge-hub.service / lpr-server.service   # systemd unit 模板
│   ├── tests/                  # 宿主单测（python -m unittest discover -s tests，11 个文件 / 73 项）
│   └── README.md               # RK3588 侧部署/冒烟/排障
├── core0_service/              # A7-Core0，C 语言，实时业务底座（**⛔ 一行云代码都没有**）
│   ├── rpmsg/                  # /dev/ttyRPMSG0 帧收发、粘包处理、心跳
│   ├── ipc_shm/                # 与 Core1 共享内存 park_shm v3 + 事件通知
│   ├── business/               # 停车场业务逻辑（app_config / business / whitelist / log）
│   ├── storage/                # 轻量文件记录（可选，默认关闭）
│   ├── tools/                  # core0_selftest / core1_stub / rpmsg_cli / load_m4.sh / hostcheck
│   └── sample_core0.conf       # 配置模板（真文件 core0.conf 不入库）
├── core1_ui/                   # A7-Core1：相机前端 + Qt 界面 + 云端兜底
│   ├── cam_link/               # 相机上行流接收参考脚本 + README
│   ├── qt_gui/                 # LCD 界面 + **第7步云端兜底（唯一云代码所在）**（qmake 工程）
│   │   ├── src/cam_link.*      #   相机流解析：serial text/binary + file + **tcp 中继模式**
│   │   ├── src/cloud_client.*  #   Qt Network 异步 POST + python3 回退传输
│   │   ├── src/settingspage.*  #   齿轮设置页（云端/网络/诊断）
│   │   └── tools/              #   build_arm.sh（book 交叉编译）/ check_static.py（门禁）
│   ├── ca.pem                  # 云端 API CA 证书（对应板端 /etc/park/ca.pem）
│   └── rpmsg_link_test.py      # RPMSG 链路测试脚本
├── other/k210_fw/              # K210 固件（CanMV MicroPython，已归档为备选采集源；纯发图 + CAM_CROP 取景框）
│   ├── main.py                 # 单文件固件：相机+JPEG+方向修正+console/帧协议双发图+心跳/统计
│   ├── tmp_kpu_old/            # 旧 KPU 识别时代固件归档（park_app/sd_probe2/gc_restore/kpu_main）
│   └── tools/                  # 宿主回归（**套件已失效待重写**：imgonly 重写后 discover 8 errors，见 AGENTS §六-4）
├── MD文档/                     # 既有规范与调试记录（**存量**，新增 .md 进 docs/）
│   ├── rk3588/调试记录.md       # RK3588 调通记录（镜像/烧录/外设/NPU 全栈）
│   └── *_standard.md           # a7-m4 / can / oled / usb / linux 规范 + Git_Update_standard
├── PhaseMd/                    # 执行层任务分解（15 份：P<步>-<序号>，G0~G8 验收门；10 = 协议母本）
├── 参考资料/                    # 参考素材：车牌识别实验代码 / k210 / rk3588模型（含 hyperlpr3/ PC 备份 + RESULTS.md）/ bh1750 驱动 / 板卡图
├── 云端API调用测试参考/          # DeepSeek 云端 API 独立测试台（cloud_api_test.py + 测试车牌图）
├── demo/                       # 演示与开发过程图片
├── 验收IMG/                     # 初步验收实拍存档
├── stm32mp157-已编译固件/        # MP157 已编译固件存档
└── deploy/
    ├── sample_cloud.conf       # /etc/park/cloud.conf 模板
    ├── sample_wpa_supplicant.conf  # /etc/wpa_supplicant.conf 模板
    ├── sample_park-ui.env      # /etc/park-ui.env 模板（含 PARK_UI_K210_TCP）
    └── systemd/                # 开机链：board-power → m4-load → core0-bus → rk3588-eth → park-ui（含 wifi-up / park-clock 辅助单元）
```

## 构建与运行（概览）

1. **M4 固件**：CubeIDE 编译 `.elf` → `/lib/firmware/m4_fw.elf` → remoteproc 启动（先于 A7 业务）。
2. **C8T6**：CubeIDE 编译烧录（`c8t6/`，regen 核对项见 `AGENTS.md`）。
3. **摄像头**：UVC 直插 RK3588，`edge_hub --source /dev/videoN`（节点用 `v4l2-ctl --list-devices` 认；RK 上 video52 是 HDMI-IN 骗点）。K210 备选源：CanMV IDE「保存到设备」上传 `other/k210_fw/main.py`。
4. **RK3588 侧**：`rk3588_service/` 按 `README.md` 部署（Tailscale scp 上传 + systemd）；模型放 `/root/models/`：hyperlpr3 三件（`y5fu_320x.rknn` 检测 + `rpv3_mdict_160_r3.rknn` 识别 = 验收配置）+ `lprnet.rknn`/`yolov8s.rknn` 回退，PC 备份 `参考资料/rk3588模型/`。
5. **Linux 侧**：`core0_service`（Makefile + book 交叉编译）；`core1_ui`（qmake + book SDK 交叉编译，`tools/build_arm.sh`）。
6. **上电自启**：`deploy/systemd/install_all.sh`（板端跑）装齐开机链与 eth0 静态链路。
7. **接口规格**：`docs/protocols.md`（改协议先改 `PhaseMd/10` 母本 → 同步 protocols → 改代码 → 变更记录）。

## 文档分工（内容隔离）

字段级协议规格统一归属 `docs/protocols.md`（2026-09-07 已建；**2026-09-26 起 §6 = MP157↔RK3588 以太网**）。
新增 .md 文档一律归 `docs/`（2026-09-27 起；存量 `MD文档/`、`PhaseMd/` 不搬迁）；`README.md` = 定位/分工/硬件/目录/构建。
