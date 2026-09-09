# 14 · GitHub 现成项目调研 —— 剩余功能（第4~8步）

> 2026-09-09 调研。结论：**无整体可直接使用的项目**（M4/RPMSG/CAN 板级组合太定制），唯一推荐直接引入的是 libmodbus；其余当参考。板子身份：**K210 = 创乐博 makerobo（CMVBlock），非正点原子 DNK210**。

## 一、按块映射

| 剩余功能 | 结论 | 项目 |
|---|---|---|
| P4-05 Modbus-TCP 从站 | ✅ **直接引入**（LGPL，交叉编译成熟，TCP server 覆盖 KPI） | [libmodbus](https://github.com/stephane/libmodbus)；轻量备选 [liyanboy74/modbus](https://github.com/liyanboy74/modbus)、[c-modbus-slave](https://github.com/SiemensEnergy/c-modbus-slave) |
| 第5步 K210 车牌识别（加分项） | ⚠️ **仅参考**：kmodel 模型可用，代码需嫁接到本板 `k210_fw/main.py`（makerobo 板 sensor/LCD 初始化不同；仓库无 LICENSE） | [HodelY/DNK210-K210-](https://github.com/HodelY/DNK210-K210-)（正点原子 CanMV 车牌例程打包：固件+SD卡文件+主程序）；框架 [kendryte/canmv](https://github.com/kendryte/canmv) |
| 第4/6步 Qt 界面 | ⚠️ **只参考布局/业务结构**（均 PC 级，移植嵌入式 Qt 不现实） | [ParkingManagementSystem](https://github.com/1454147447/ParkingManagementSystem)（Qt+ARM+百度AI，最接近，无LICENSE）、[Parkinglot_Manage_System](https://github.com/RETAPxRSC/Parkinglot_Manage_System)（Qt5+OpenCV+EasyPR）、[Tairy/parking](https://github.com/Tairy/parking)（Qt+MySQL） |
| 第7步 云端兜底 | 🔴 **硬事实：DeepSeek 官方 API（api.deepseek.com）不支持图片输入**，只有文本。兜底须写**OpenAI 兼容 vision 接口**（`image_url`+base64），指向托管 VL 模型的服务商（如 SiliconFlow 的 DeepSeek-VL2/Qwen2.5-VL），做成可配 base_url/model 的通用客户端 | [DeepSeek-R1#83](https://github.com/deepseek-ai/DeepSeek-R1/issues/83)（官方无图确认）、[awesome-deepseek-integrations](https://github.com/deepseek-ai/awesome-deepseek-integrations) |
| 整套识别/停车云平台 | ℹ️ PC/服务器级，备料 | [kindow 停车云平台](https://github.com/kindow/License-plate-recognition-parking-cloud-platform-system)（C/C++ Linux Qt OpenCV EasyPR）、[jm12138/License_plate_recognition](https://github.com/jm12138/License_plate_recognition)（PaddleOCR）、[HyperLPR](https://github.com/szad670401/HyperLPR) |
| MQTT / SQLite / systemd | ✅ 标准件，无需项目：mosquitto 或 paho.mqtt.c；SQLite 官方源；systemd 单元+看门狗自写 | — |

## 二、待办影响

1. P4-05：libmodbus 进交叉编译链；状态机/白名单/车位统计/RPMSG 胶水仍自写（自家协议）。
2. K210 识别开工：拉 DNK210 仓库 **kmodel + 例程思路**，嫁接进 makerobo 板现成 `main.py`。
3. 第7步动手前：PhaseMd/08 + 协议母本中"DeepSeek Vision"表述 → "OpenAI 兼容视觉接口（默认 DeepSeek-VL 系托管端点）"。

## 三、许可证注意

- libmodbus = **LGPL-2.1**，动态链接无碍。
- DNK210 仓库、ParkingManagementSystem = **无 LICENSE**，只参考思路/模型，勿直接拷码。
