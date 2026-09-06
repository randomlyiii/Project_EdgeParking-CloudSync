# 第 3 步 · Linux ↔ M4 RPMSG 通讯（双核通道打通）

> 步骤：3 ｜ 前置：G2（CAN 底盘闭环） ｜ 产出：M4 OpenAMP 端点 + `core0_service/rpmsg/` 稳定通道 ｜ 验收门：G3
> 约束（Task.md 第三节）：ttyRPMSG0 单帧 MTU ≈496~512B、**无流控** → 协议必须带 seq+心跳，断链 ≤1s 检出。
> 帧格式权威母本在文档 10；本文帧表为工作副本。

## 1. RPMSG 帧约定（与文档 10 同步）

```
| 帧头 0xAA 0x55 | type(1B) | seq(2B LE) | len(2B LE) | payload(len B) | CRC16(2B, 校验 type..payload) |
```
- 下行（Core0→M4）：`0x11` 开闸 ｜ `0x12` 关闸 ｜ `0x13` 查询全量状态 ｜ `0x14` 配置下发（预留）
- 上行（M4→Core0）：`0x21` CAN 事件转发（payload=CAN 帧 8B+本地 tick） ｜ `0x22` 从节点离线/恢复 ｜ `0x23` M4 状态/故障字 ｜ `0x7E` 双向心跳
- 超长数据分块同 seq 连发；seq 连续性用于丢帧统计。

## 2. M4 侧任务

### P3-01 OpenAMP/RPMSG 使能 ⬜
1. CubeMX：M4 工程 Middleware 勾 **OPENAMP**（自动带出 IPCC 外设 + IPCC 中断 + `rsc_table`/`openamp` 生成物）。
2. 确认生成：`rsc_table.c`（resource table，含 rpmsg VDEV 条目）、`openamp.c`（endpoint 创建）。
3. `main.c`：`MX_OPENAMP_Init()` 在 `osKernelStart()` 前；新建/复用一个任务做 RPMSG 收发轮询（API 名以 CubeMX 实际生成物为准，ST 模板常为 `OPENAMP_check_for_message` / `OPENAMP_send`）。
- **验收**：工程编译链接通过；rsc_table 在二进制里（`arm-none-eabi-objdump -h` 或看 map）。

### P3-02 事件上行：CAN → RPMSG ⬜
- 第2步的"待上报事件槽"置位后组帧发送：
  - `0x21` payload = CAN 帧（id/dlc/8B data）+ 32bit 本地 tick；
  - 从节点离线/恢复（第2步 P2-10 判定）→ `0x22`。
- 发送失败处理：rpmsg 缓冲满时重试 2 次后丢弃并计数（事件类可丢，状态类靠 `0x13` 全量重同步兜底）。
- **验收**：配合 P3-05，Linux 侧 hex 能看到 0x21/0x22 帧。

### P3-03 指令下行：RPMSG → CAN 0x100 ⬜
```c
/* 收 0x11/0x12/0x13 → 转成 CAN 帧 */
FDCAN_TxHeaderTypeDef th = {0};
uint8_t d[8] = {0};    /* d[0]: 0x01 开 / 0x02 关 / 0x10 查询 */
th.IdType = FDCAN_STANDARD_ID; th.Identifier = 0x100;
th.TxFrameType = FDCAN_DATA_FRAME; th.DataLength = FDCAN_DLC_BYTES_8;
HAL_FDCAN_AddTxMessage(&hfdcan1, &th, d, NULL);
```
- 收 `0x13` 查询：回 `0x23` 全量状态（闸状态、从节点在线、错误计数）。
- **验收**：Linux 发 0x11 → C8T6 SG90 动作（与 G2 场景打通成端到端）。

### P3-04 M4 故障检测（Task 3.4 对应）⬜
- 周期采样 `HAL_FDCAN_GetErrorCount` / ProtocolStatus；Bus_Off → 尝试重新 `HAL_FDCAN_Start` 并经 `0x23`/`0x22` 上报；
- 从节点离线（P2-10）持续上报状态变化（边沿触发，不刷屏）。
- **验收**：拔 CAN 线 → Linux 侧 ≤3s 收到离线；恢复 → 收到恢复。

## 3. Linux（Core0）侧任务

### P3-05 remoteproc 加载 M4 固件 ⬜
```sh
cp m4_fw.elf /lib/firmware/
echo stop  > /sys/class/remoteproc/remoteprocX/state   # 若已运行
echo start > /sys/class/remoteproc/remoteprocX/state   # X 按实际（dmesg 确认）
dmesg | tail          # 应见 remoteproc/virtio rpmsg 探测日志
ls /dev/ttyRPMSG0     # 通道就绪标志
```
- 失败排查：rsc_table 缺失、固件路径/权限、内核 CONFIG_RPMSG_CHAR。
- **验收**：重启 5 次均稳定出 `/dev/ttyRPMSG0`。

### P3-06 通道初始化（`core0_service/rpmsg/`）⬜
- `open("/dev/ttyRPMSG0", O_RDWR | O_NOCTTY | O_NONBLOCK)`，termios raw；
- 接收环形缓冲 ≥8KB；读写用 epoll 单线程（与第4步业务的事件循环汇合）或独立收发线程。
- **验收**：M4 心跳帧能收到。

### P3-07 帧协议实现 ⬜
- RX 状态机：帧头→type→seq→len→payload→CRC16；粘包处理；seq 连续性统计（丢帧计数）。
- TX：组帧 + CRC；MTU 检查（≤496B，超长分块）。
- **验收**：对发 10 分钟无解析错；人为注错帧（坏 CRC）被丢弃且计数。

### P3-08 双向心跳 ⬜
- Core0 每 500ms 发 `0x7E`；监测 M4 心跳（RPMSG `0x7E` 或 CAN 0x210 转发）。
- 超时 1s → `LINK_DOWN`（业务层可读状态）；恢复 → 清除。
- **验收（KPI）**：remoteproc stop M4 → ≤1s 业务层感知。

### P3-09 断链恢复与状态重同步 ⬜
- 恢复动作：重开 tty/清缓冲 → 发 `0x13` 全量查询 → 按返回刷新状态表；
- seq 补偿：跨断链窗口的旧 seq 全部丢弃，从新 seq 对齐。
- **验收（KPI）**：断链期间丢的遮光事件不阻塞业务——恢复后靠全量状态帧补齐闸/到位状态。

### P3-10 业务数据流对接 ⬜
- 上行：`0x21` → 投递业务队列（第4步 P4-01 消费）；
- 下行：业务调 `rpmsg_send_gate(cmd)` 封装开/关闸；
- 线程模型：业务只碰队列不碰 fd。
- **验收**：事件从 CAN 到业务队列全链路日志可查（与第4步联测）。

### P3-11 打桩工具 `tools/rpmsg_cli` ⬜
- hex 收发/注入/丢帧统计小 CLI。
- **验收**：用它独立验证 M4 行为（不依赖业务代码）。

## 4. 验收门 G3

- [ ] P3-01/05 remoteproc 加载稳定、ttyRPMSG0 出现
- [ ] P3-07 帧协议 10 分钟无解析错
- [ ] P3-08 断链 ≤1s 检出
- [ ] P3-09 恢复重同步、无旧 seq 污染
- [ ] P3-02/03 事件上行 + 指令下行端到端（Linux 发指令 → SG90 动作）
- [ ] P3-04 CAN 离线/恢复经 RPMSG 可见
- [ ] P3-11 CLI 工具入库

## 避坑

- **启动顺序**：remoteproc 先于 core0 业务（第8步 systemd 排序落实）；业务启动时 tty 不存在要重试而非崩溃。
- ttyRPMSG0 归 Core0 独占；Core1/Qt **禁止**打开（图像不外溢原则）。
- M4 复位（stop/start）后 rpmsg 通道重建，Core0 必须走"断链恢复"路径，不能死等。
- 任何帧 >496B 必须分块，别赌一次 write。
