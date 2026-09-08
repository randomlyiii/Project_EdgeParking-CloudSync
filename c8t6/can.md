# CAN 2.0 通信说明（c8t6 子工程）

> 工程：`c8t6/`（STM32F103C8T6 + CubeIDE + FreeRTOS CMSIS_V2）
> 角色：停车场 Demo **CAN 从节点**（bxCAN1 + TJA1050）
> 对端：MP157 **M4**（FDCAN1 PD0/PD1 → 板载 TJA1042），CAN↔RPMSG 网关，**主节点**
> 协议母本：`PhaseMd/10_协议规格总表_protocols草案.md`（CAN 章）；改协议先改母本 → 本文档 → 两端代码
> 最后更新：2026-09-06（CAN 2.0 阶段落地：收发 + 心跳 + 事件 + OLED 状态）

---

## 1. 总体架构与物理层

```
                    CAN 总线 (经典 CAN 2.0A / 500 kbps / 标准帧 11bit ID / 两端 120Ω / 三方共地)
   ┌────────────┐   ┌──────────┐  CANH ──╮      ╭── CANH   ┌──────────────┐
   │ M4 主节点  │   │ 板上      │         │ 120Ω │          │ 板上          │
   │ FDCAN1     │───│ TJA1042   │◄────────┘      └────────►│ TJA1050      │───┐
   │ PD0=RX PD1=│   │           │  CANL ──╮      ╭── CANL   │ (CAN1 收发器) │   │
   │ TX (AF9)   │   └──────────┘         │ 120Ω │          └──────────────┘   │
   └────────────┘                        └──────┘                ▲            │
                                                          PA12=CAN_TX        │
                                                     ┌──────────────┐        │
                                                     │ STM32F103C8T6│◄───────┘
                                                     │ bxCAN1       │
                                                     │ PA11=CAN_RX  │
                                                     └──────────────┘
```

| 项目 | 参数 |
|---|---|
| 标准 | **CAN 2.0A**（经典 CAN），标准帧（11-bit ID），数据帧（DLC=8），**无 CAN-FD**（F103 bxCAN 不支持） |
| 波特率 | **500 kbps**（两端一致；见 §3 位时序） |
| 拓扑 | 2 节点并联（M4 主 + C8T6 从），杜邦线短接（<1m 演示） |
| 终端 | **总线两端各 1 只 120Ω**（C8T6 侧 TJA1050 模块通常自带/外挂；M4 板上 TJA1042 侧自行确认），**严禁超过 2 只** |
| 共地 | MP157 / C8T6 / 收发器电源 **必须共地**（否则隐性电平均偏，收发异常） |
| ID 分配 | `0x1xx` 主→从指令段 ｜ `0x2xx` 从→主上报段（详见 §4） |
| 帧率预算 | 心跳 1Hz + 零星事件/指令。8 字节标准帧总线占用 ≤ ~122 bit ≈ 0.24ms/帧@500k ⇒ **常态负载 <1%**，无带宽压力 |

C8T6 接线：`PA11=CAN_RX`、`PA12=CAN_TX`（F103 默认映射，**无需 AFIO 重映射**；默认映射位置恰好不与已占用的 PB8/PB9 冲突）→ 接 TJA1050/SIT1050 模块的 RX/TX → 模块 CANH/CANL 上总线，模块 VCC 3.3~5V（按模块要求）、GND 共地。M4 侧 FDCAN1 = PD0/PD1（AF9）→ 板上 TJA1042 → 板上 CAN 端子。

---

## 2. CAN 帧结构速览（2.0A 标准数据帧）

| 字段 | SOF | 仲裁场 | 控制场 | 数据场 | CRC 场 | ACK | EOF | IFS |
|---|---|---|---|---|---|---|---|---|
| 位宽 | 1 | 12（11bit ID + RTR） | 6（IDE+r0+DLC[3:0]） | 0~8 字节 | 15 + 1 分隔 | 2 | 7 | 3 |

- 差分电平：显性 dominant = 逻辑 **0**（CANH≈3.5V/CANL≈1.5V），隐性 recessive = 逻辑 **1**（两线≈2.5V）。
- ID 越小优先级越高：`0x100` > `0x200` > `0x210`（总线仲裁自动处理，无需软件让行）。
- 总线空闲 ≥ 3 bit 后节点即可发帧；发送方监听 ACK slot，**无应答视为错误**。
- 本协议**只使用标准数据帧**：`IDE=0`、`RTR=0`、`DLC=8`、未用数据字节恒填 `0x00`。
- 载荷字节序：CAN 控制器按字节原样收发，多字节字段的字节序由协议自定——**lux 字段用大端（高字节在前）**，与 RPMSG 帧的小端约定区分（见母本文档）。

---

## 3. 位时序（500k 是怎么算出来的）

公式：`波特率 = CAN 时钟 ÷ Prescaler ÷ (1 + BS1 + BS2)`；采样点 = `(1+BS1) / (1+BS1+BS2)`。

| 端 | CAN 时钟 | Prescaler | SJW | BS1 | BS2 | 每 bit tq | 波特率 | 采样点 |
|---|---|---|---|---|---|---|---|---|
| **C8T6 bxCAN1** | APB1 = **36 MHz**（SYSCLK 72M/2） | **9** | 1 | 5 | 2 | 8 | 500 k | **75%** |
| M4 FDCAN1 | PLL3 = **100 MHz** | 40 | 1 | 3 | 1 | 5 | 500 k | 80% |

- C8T6 侧：tq = 36MHz/9 → 250ns；bit = 8×250ns = 2µs → 500kbps；采样点 (1+5)/8 = **75%**（≥75% 官方推荐）。
- 两端采样点 75%/80% 差距小，2 节点杜邦线演示无压力；**两端的"实际波特率"必须都在 500k**（CubeMX 参数页底部显示值核对，别只看 Prescaler）。
- 改波特率 = 改 `c8t6.ioc` → CAN.Prescaler/BS1/BS2，重新生成 `can.c`（核对 §8 再生成清单）；对端 M4 同步改 FDCAN 位时序。

---

## 4. 帧协议（字节级定义）

> 方向以 M4 视角：**↓ = M4→C8T6**，**↑ = C8T6→M4**。所有帧 DLC=8，未用字节恒 `0x00`。
> C8T6 侧代码落点：ID/周期宏在 `Core/Inc/config.h`，指令码/事件码/状态位宏与收发逻辑在 `Core/{Inc,Src}/can_node.{c,h}`。

### 4.1 ID 分配

| ID | 方向 | 用途 | 段 |
|---|---|---|---|
| 0x100 | ↓ | 指令（开闸/关闸/查询） | 0x1xx 主→从（预留扩展） |
| 0x110 | ↓ | 事件确认（主端收到 0x200 后的回执，d[0]=回显事件码） | 0x1xx 主→从 |
| 0x200 | ↑ | 事件上报 / 查询应答 | 0x2xx 从→主 |
| 0x210 | ↑ | 心跳（1Hz） | 0x2xx 从→主 |
| 0x1xx / 0x2xx 其余 | — | **预留**，不得占用 | — |

### 4.2 0x100 主→从 指令帧（C8T6 收）

| 字节 | 含义 |
|---|---|
| d[0] | 指令码：`0x01` 开闸 ｜ `0x02` 关闸 ｜ `0x10` 查询（从端回一帧 0x200 快照） |
| d[1..7] | 预留，恒 0 |

指令处理（`CAN_Node_Poll`，10ms 轮询）：开/关闸 → 更新闸逻辑状态（SG90 阶段接 PWM 后转 CCR 目标）；查询 → 立即发 0x200，载荷按当前状态组帧。

示例（开闸）：`ID=0x100, DLC=8, data = 01 00 00 00 00 00 00 00`
示例（查询）：`ID=0x100, DLC=8, data = 10 00 00 00 00 00 00 00`

### 4.2a 0x110 主→从 事件确认帧（C8T6 收，2026-09-08 新增）

| 字节 | 含义 |
|---|---|
| d[0] | 回显被确认的 0x200 d[0] 事件码（`0x01` 遮光/车到位 ｜ `0x00` 恢复） |
| d[1..7] | 预留，恒 0 |

主端(M4)每收到一帧 0x200（遮光事件或查询应答）自动回 0x110 回执。C8T6 收到后记录 ack 时刻
（`CAN_Node_LastAckTick()`），OLED 行4 显示特殊标识 **`CAN:*OK*`**（约 OLED_ACK_FLASH_MS=1s）
表示"板子已确认收到光敏信号"，随后回 `CAN:OK`。行4 状态集：`CAN:OK` ｜ `CAN:EVT`(已发未确认)
｜ `CAN:*OK*`(已确认) ｜ `CAN:ERR`。

### 4.3 0x200 从→主 事件帧（C8T6 发）

| 字节 | 含义 |
|---|---|
| d[0] | event：`0x01`=遮光/车到位 ｜ `0x00`=恢复 ｜ 查询应答也走本帧（d[0]=当前是否遮光） |
| d[1..2] | **lux（大端：d[1]=高字节）** |
| d[3] | drop%：lux 相对环境基线掉点百分比 0~100 |
| d[4] | 状态位（见 §4.5，与 0x210 d[0] **同源**） |
| d[5..7] | 预留，恒 0 |

触发规则（在 `BH1750_Task` 遮光状态机边沿，只发一次）：
- CLEAR→SHADED 边沿：`d[0]=0x01`（"车到位"，主端据此触发业务）；
- SHADED→CLEAR 边沿：`d[0]=0x00`（"恢复"）；
- 收到 0x100/0x10 查询：主端主动要快照，应答帧不区分 event 时序（主端以 d[4] 状态位为准）。

示例（遮光，lux=8000=0x1F40，drop=85%，状态 bit1 遮光中→d[4]=0x02）：
`ID=0x200, data = 01 1F 40 55 02 00 00 00`

### 4.4 0x210 从→主 心跳帧（1Hz）

| 字节 | 含义 |
|---|---|
| d[0] | 状态位（与 0x200 d[4] 同源，见 §4.5） |
| d[1] | 上电秒数低 8 位（辅助主端判 C8T6 是否复位） |
| d[2..7] | 预留，恒 0 |

### 4.5 状态位定义（0x200 d[4] / 0x210 d[0]）

| bit | 宏 | 含义 |
|---|---|---|
| bit0 | `CAN_STAT_GATE_OPEN` | 1=闸处于开位（收过 0x100/0x01） |
| bit1 | `CAN_STAT_SHADED` | 1=遮光中（车到位） |
| bit2 | `CAN_STAT_LUX_FAULT` | 1=光感 BH1750 读取故障（掉线/未上电） |
| bit3 | `CAN_STAT_CAN_ERR` | 1=本端 CAN 发送异常（Start 失败 / 无空闲邮箱 / `HAL_CAN_AddTxMessage` 失败）；成功提交一帧即清零。**注意：NART 单发下"无 ACK"的帧会被硬件丢弃并释放邮箱，本端（轮询+邮箱检查）检测不到，故 bit3 ≠ "远端离线"**——远端离线由主端 3s 收不到 0x210 判定（§4.6） |

### 4.6 超时/在线约定

- **主端判从端**：3s 无 0x210 → 判 C8T6 离线（M4 侧实现，第3步经 RPMSG 报 A7）；收到心跳即恢复在线。
- **从端不判主端**：C8T6 不因长时间收不到主端指令做任何动作（照常心跳），简化逻辑。

---

## 5. C8T6 侧软件架构（FreeRTOS 集成）

### 5.1 模块与任务归属

| 模块/函数 | 位置 | 职责 |
|---|---|---|
| `MX_CAN_Init()` | `can.c`（CubeMX 生成） | bxCAN1 500k、Normal、NART=ENABLE、ABOM=ENABLE；PA11/PA12 引脚 + NVIC（见 §8 注） |
| `CAN_Node_Init()` | `can_node.c` | 调度器启动前（main USER CODE 2）配过滤器 + `HAL_CAN_Start` |
| `CAN_Node_Poll()` | `can_node.c` | **CAN_Rx_Task 每 10ms** 调：轮询收 FIFO0（指令/查询）+ 1Hz 心跳 0x210 |
| `CAN_Node_SendEvent()` | `can_node.c` | BH1750_Task 遮光边沿调用，发 0x200（事件） |
| `Shade_FSM_Update()` | `shade.c` | 百分比掉点遮光状态机（每 200ms 采样喂一次） |
| 显示 | `freertos.c` OLED_Task | 行1 标题 / 行2 Lux+drop% / 行3 Gate / 行4 CAN 状态 |

### 5.2 接收路径：零中断纯轮询（本工程选择）

```
M4 0x100 ──► bxCAN FIFO0(3 深) ──(CAN_Rx_Task 每 10ms 轮询)──► HAL_CAN_GetRxFifoFillLevel
        ──► HAL_CAN_GetRxMessage ──► switch(d[0]) ──► 闸状态 / 查询应答
```

- 为什么不用 CAN 中断：从站对端只有 M4 一个主节点、指令零星，10ms 轮询绰绰有余；且 F1 的 `USB_LP_CAN1_RX0_IRQn` 在 FreeRTOS 下有优先级/临界区坑（参考 `MD文档/can_standard.md` §3.8/§4：从站"零中断纯轮询"是既定工程决策，ISR 内禁 take 类 FreeRTOS 函数）。
- CubeMX 已在 `can.c` 的 `HAL_CAN_MspInit` 打开 RX0/SCE 的 NVIC，但我们**不调 `HAL_CAN_ActivateNotification`** → CAN 外设 IER 无置位 → ISR 实际不触发，无副作用；不要自行加 `HAL_CAN_IRQHandler` 依赖的激活调用即可。
- 若未来指令突发量增大（如批量配置下发），再改 FIFO0 中断方案：`HAL_CAN_ActivateNotification(CAN_IT_RX_FIFO0_MSG_PENDING)` + 回调里只 `GetRxMessage → 置标志/入队(FromISR)`，动作仍在任务做（模板见 `PhaseMd/03` P2-01）。

### 5.3 发送路径：邮箱预检 + 任务间互斥

```
CAN_TxFrame(id, data):            # can_node.c, 仅任务上下文调用
  CanTxMutex 上锁                  # BH1750_Task(0x200) 与 CAN_Rx_Task(0x210/应答) 并发串行化
  HAL_CAN_GetTxMailboxesFreeLevel > 0 ? HAL_CAN_AddTxMessage : 失败
  CanTxMutex 解锁
  失败 → 状态位 bit3=1(CAN:ERR)；成功 → 清 bit3
```

- bxCAN 有 3 个发送邮箱，HAL 自动挑空闲邮箱；NART=ENABLE 下发送失败（无 ACK）硬件**放弃并释放邮箱**，不会占死邮箱（权衡见 §6）。
- 事件/心跳都是"最新状态"语义，丢一帧不补偿——心跳下 1s 补发，遮光状态持续存在由 0x210 状态位 bit1 兜底。

### 5.4 OLED 行约定（与帧内容一一对应）

```
Line1: PARK NODE        Line2: Lux:12345 D:78%   (D=当前掉点%, 调遮光参数看这里)
Line3: Gate:OPEN/CLOSE  Line4: CAN:OK / CAN:EVT(事件后闪1s) / CAN:ERR
```

- `Lux:ERR` = BH1750 故障（状态位 bit2 同源）；拔传感器 → `Lux:ERR`，重插约 5s 自愈。
- 所有 OLED 访问持 `OledMutex`；CAN 相关状态读取为 8/32 位原子读，无需额外锁。

---

## 6. NART / ABOM 取舍（改了默认值，重点核对）

| 参数 | 本端值 | 含义与理由 |
|---|---|---|
| **NART**（`AutoRetransmission`） | **ENABLE**（= 不自动重传） | 发送失败（无 ACK）**只试一次即放弃**，不占邮箱。避免主端(M4)断电/总线异常时本端反复重传 → TEC 冲到 255 → BusOff → 恢复 → 再重传的"风暴"。从端心跳 1s 一次，代价只是偶尔丢一帧，可接受。 |
| **ABOM**（Auto Bus-Off Mgmt） | **ENABLE** | 极端干扰下真进了 BusOff 由硬件自动等 128 个隐性位恢复，无需软件干预。 |

- ⚠️ **代码 vs .ioc**：`c8t6.ioc` 已设 `CAN.NART=ENABLE`（生成代码应为 `AutoRetransmission = DISABLE`）。若 CubeMX 重新生成后看到 `= ENABLE`，说明 .ioc 与生成不一致，请以 NART=ENABLE 为准（can.c 文件头 USER CODE 0 有注）。
- 与兄弟工程主站（ABOM=DISABLE + 软件 INRQ 恢复）不同：那是"主站收多从站发"拓扑；本端 2 节点 + 从站极低帧率，ABOM=ENABLE 足够且简单。

---

## 7. 验收与联调

### 7.1 本端自测（无对端也能验证的部分）
- [ ] 上电 OLED 正常四行；行4 无本端异常时 `CAN:OK`。`CAN:ERR` 出现于 Start 失败/邮箱忙等本端错误（§4.5 bit3）；**主端(M4)不在线在本端不显示为 ERR**（无 ACK 由硬件丢弃，离线判定在 M4）。
- [ ] 手遮 BH1750（掉点 ≥60% 连续 3 次 ≈ 0.6s）→ 行2 drop% 增大、遮光翻转，行4 闪 `CAN:EVT`（本端 0x200 已发出）；移开恢复再闪一次。
- [ ] 有逻辑分析仪/USB-CAN 工具时：抓 0x200/0x210 帧，字段与 §4 表比对（lux 大端！）。
- [ ] （可选）`can.c` 临时把 Mode 改 `CAN_MODE_LOOPBACK` + 过滤器掩码临时改全收（0x0000），观察自发 0x210 能否收回——验证 TX/RX/过滤器全链路；**验完恢复 Normal 与掩码**。

### 7.2 与 M4 联调（分级推进，参考 `PhaseMd/03` P2-16）
> M4 主端实现见 `m4_fw/CM4`（`can_master.{c,h}`：过滤器+Start、CANRxTask 10ms 轮询、0x200/0x210 解析、3s 离线判定、`CAN_Master_SendCmd/RequestCmd` 指令口）。本步未用底板按键，触发方式见 §1 互动方案说明。
1. **心跳**：C8T6 发 0x210 1Hz，M4 `g_can_master_mon.hb_count` 持续涨且 `online=1` ⇒ 物理层 + 波特率 OK（第一步只验这个）。
2. **事件上行**：遮光 → 0x200 → M4 `g_can_master_mon.ev_count/last_ev/last_lux/last_drop` 变化。
3. **指令下行**：M4 触发 `CAN_Master_RequestCmd(0x01/0x02/0x10)`（或调试器写 `g_can_master_cmd_pending=1, g_can_master_cmd_value=0x01`）→ C8T6 Gate 行翻转 / 查询应答。
4. **稳定性**：连续遮光/恢复 20 次 + 拔插 CAN 线，两端错误计数不增长。

### 7.3 物理层检查
- 断电量 CANH–CANL ≈ **60Ω**（两只 120Ω 并联）——不是 120Ω 也不是 0Ω；
- 三方共地确认；上电隐性：CANH≈CANL≈2.5V、差分≈0V；H/L 未反接；
- 时通时断：查杜邦线接触、终端电阻数量、两端口径（只挂一个终端电阻必出问题）。

### 7.4 故障速查

| 现象 | 排查顺序 |
|---|---|
| 收不到 M4 指令 | 两端实际波特率（CubeMX 显示）→ H/L 反接 → 60Ω → 共地 → **本端过滤器掩码**（§5.2 后注：`0x700<<5` 只收 0x1xx）→ M4 是否 Start |
| M4 收不到心跳 | C8T6 行4 是否 `CAN:ERR`（= 无 ACK/未上总线）；TJA1050 供电/共地；PA11/PA12 是否被占用 |
| 收到但字段乱 | lux 大端组装顺序；DLC 未按 8；对端解析 `DataLength>>16`（FDCAN） |
| 时通时断 | 终端电阻只挂一只 → 补到两端各一只；杜邦线接触；两线缠绕在一起走 |
| OLED `CAN:ERR` 但明明接了 M4 | TX 邮箱满（事件+心跳撞车，正常不该发生）→ 看 bit3 之外是否还有别的原因；M4 是否在收发 | 

---

## 8. CubeMX 再生成注意事项（重要）

1. `can.c` 手改过的两处会被**重新生成覆盖**：① `AutoRetransmission`（见 §6）；② 文件头 USER CODE 0 注释（不会覆盖）。核对后再编译。
2. CAN 参数已固化在 `.ioc`：Prescaler=9/BS1=5/BS2=2/SJW=1 → 500k 采样点 75%；NART=ENABLE、ABOM=ENABLE；NVIC 两个 CAN 中断保持使能（轮询方案下无 IER 置位，不触发）。
3. `can_node.c / shade.c / config.h` 属 `Core/Src|Inc`，CubeIDE 源文件夹自动编译、无需登记进 `.ioc`；regen 不删除。
4. `freertos.c` 的任务体/CAN 互斥都在 **USER CODE 段**，regen 保留；但 CubeMX 重新生成时会重写任务定义区——确认 `OLED_Task` 栈仍为 **384 words**（.ioc `FREERTOS.Tasks01` 已同步），`configTOTAL_HEAP_SIZE=9600` 未被回退。
5. 新增的第二把互斥 `CanTxMutexHandle` 是动态创建（USER CODE RTOS_MUTEX 段），CubeMX 不感知；若堆报 `HEAP STILL AVAILABLE=0`，优先减 OLED 任务栈而非删互斥。

---

## 9. 相关文档

- `PhaseMd/10_协议规格总表_protocols草案.md` —— 字段级协议**母本**（CAN 章为本文档上位）；改帧格式先改它。
- `PhaseMd/03_第2步_M4与下位机CAN通讯.md` —— M4 侧任务（FDCAN 过滤器/队列/离线判定）与联调矩阵。
- `c8t6/实现步骤.md` —— 本工程三阶段路线图（阶段3 = CAN+SG90 落地）。
- `MD文档/can_standard.md` —— 兄弟工程 CAN 规范：§3.8 FreeRTOS CAN ISR 铁律、§4 从站零中断纯轮询架构（本工程采纳其工程结论，协议字段不通用）。
- `docs/protocols.md`（仓库级协议总表，2026-09-07 已建；CAN 章为本文档上位，改帧格式先改它/母本，再同步本文档）—— 本文档保持同步。

---

## 10. 附录 A：对照 `MD文档/can_standard.md` 合规自检（2026-09-06）

> 说明：can_standard.md 是兄弟工程(多从站数据采集)的 CAN 规范，其**帧字段布局/功能码/XOR 校验和等协议内容不适用于本项目**（本项目帧定义以 PhaseMd/10 为准，见 §4）；但其中的**物理层要求、FreeRTOS+CAN 工程铁律、从站架构结论**对本工程同样成立，逐条自检如下。

| # | 规范条目 | 要求要点 | c8t6 现状 | 结论 / 处置 |
|---|---|---|---|---|
| 1 | §1 物理层 | 500 kbps | Prescaler=9/BS1=5/BS2=2 @APB1 36MHz ⇒ 500k | ✅ |
| 2 | §1 物理层 | 采样点 **75%**（教训：50%→2 节点 CRC 错） | (1+5)/8 = **75%** | ✅ 与规范同值。位时序分配不同（8tq vs 12tq）不影响；**SJW=1 vs 规范 2** 属晶振容差余量，2 节点短总线无影响，保留现配置（要加余量可 .ioc 改 SJW=2，非必须） |
| 3 | §1 物理层 | 两端各 120Ω / 共地 | 接线层，代码不可验 | 🟨 按 §7.3 checklist 现场执行（断电 60Ω、上电隐性≈2.5V、H/L 不反接） |
| 4 | §2.1/§2.3 帧格式 | 帧含优先级/节点/功能码/载荷 + **XOR 校验和** | 本协议无这些字段（PhaseMd/10 定义 0x100/0x200/0x210 布局） | ⚠️ 有意不适用；XOR 校验需 **M4 端同步**才有效 ⇒ 列为建议（协议演进项，不单边改） |
| 5 | §3.5 帧校验 | DLC!=8 丢弃、坏帧丢弃 | **已加 `rh.DLC != 8` 丢弃**（HAL 对 DLC<8 帧会带回邮箱残留数据，一并规避） | ✅ 本次整改；XOR 部分同 #4 |
| 6 | §3.8 FreeRTOS CAN ISR 铁律 | ISR 最小化/无 take/FromISR 需满足优先级 | **零 CAN 中断**（未 `ActivateNotification`，IER 全 0，ISR 不触发） | ✅ 最强形态合规。将来改中断方案需遵守 §3.8：本端 RX0/SCE 中断优先级=5=configMAX_SYSCALL 边界，可用 FromISR 系 API，但建议沿用"ISR 只搬数据/置标志" |
| 7 | §4.1 从站零中断纯轮询 | 从站不挂 CAN ISR，任务轮询 | CAN_Rx_Task 每 10ms 轮询 FIFO0 | ✅（fallback 中断方案见 §5.2） |
| 8 | §4.2 **NART=ENABLE**（网络隔离，单帧丢不重试） | 发送失败不占邮箱、不拖累全网 | `AutoRetransmission=DISABLE`（.ioc NART=ENABLE） | ✅ 推论已评估：遮光事件帧单发若被总线错误丢掉，**0x210 状态位 bit1 在 ≤1s 内兜底**（主端看 bit1 判"遮光中"即可，见 §4.6） |
| 9 | §4.2 ABOM=ENABLE（从站兜底） | BusOff 自动恢复 | `AutoBusOff=ENABLE` | ✅ |
| 10 | §4.3 从站初始化在**任务首行** + `CAN_DeInit` 清寄存器破坏 + 按 node_id 错峰 | 防御上电寄存器残留/多从站同时上电冲突 | 本项目在 **main USER CODE 2（调度器启动前）** 做 ConfigFilter+Start（PhaseMd/03 P2-01 既定）；无"vTaskStartScheduler 寄存器破坏"症状；单从站无需错峰 | ⚠️ 有意差异，保留并文档化；若将来出现上电偶发收不到帧，可参考其做法在任务首行 DeInit+重初始化 |
| 11 | §4.4 轮询接收 **drain ≤16 帧/周期**（防饿死低优先任务） | 收帧循环设上限 | **已加 `CAN_RX_DRAIN_MAX=16`**（can_node.c） | ✅ 本次整改 |
| 12 | §4.4 周期发送（心跳+传感器） | 心跳在线探针 | 心跳 0x210 @1Hz（CAN_Node_Poll 内） | ✅（主端 3s 判离线 = 3×周期，同规范 3×500ms=1500ms 的比例） |
| 13 | §4.5 发送超时（轮询 TxStatus）+ 失败入**本地缓存重试** | 发送失败要有兜底 | HAL 提交式：邮箱空闲预检 + `AddTxMessage`；失败置状态位 bit3、成功即清；数据兜底靠 0x210 状态位（见 #8） | ⚠️ 等价语义下更简，保留；逐帧缓存重试列为建议项（事件可靠性再提升时做） |
| 14 | §3.6 主站错误处理（ErrorMonitor/INRQ 恢复） | 主站专属 | 从站不需要；BusOff 已由 ABOM 自愈 | ✅ 不适用。已知限制：**上电瞬间若总线被强占导致 `HAL_CAN_Start` 超时，本端不会自恢复**（低频场景，必要时加 INRQ 重试） |
| 15 | 滤波器 | 从站可全通或选择滤波 | 0x1xx 掩码选择接收（比全通更严，硬件挡噪声/非指令帧） | ✅ |
| 16 | §3 负载估算（8B 标准帧 ≈122bit≈0.24ms@500k） | 评估带宽余量 | 心跳 1Hz + 稀疏事件 ⇒ 常态负载 **<1%** | ✅ 无带宽压力 |

**本次整改（对照 §3.5/§4.4）**：① 收帧增加 `DLC!=8` 丢弃；② 单轮收帧上限 16 防饿死——见 `can_node.c` `CAN_Node_Poll()`。
**建议项（不阻塞，均需评估后再动）**：P2 与 M4 定稿协议时评估加 XOR 校验和/源节点字节（改 PhaseMd/10 母本 → 两端同步）；P3 SJW=1→2；P3 事件帧失败重发一次；P3 Start 失败 INRQ 重试。

## 变更记录

| 日期 | 变更 | 影响 |
|---|---|---|
| 2026-09-06 | 初版：随 C8T6 CAN 2.0 功能落地编写 | C8T6 / M4 |
| 2026-09-06 | 依 `can_standard.md` 审计整改：收帧 DLC==8 校验 + 单轮 drain 上限 16；新增 §10 合规自检附录 | C8T6 |
