# 协议规格总表（`docs/protocols.md` 草案母本）

> 用途：本文是四端（C8T6 / M4 / core0 / core1 / K210）字段级协议的**单一母本**，随各步实现逐步拷入并维护 `docs/protocols.md`。
> 改协议规则：先改本文 → 同步 `docs/protocols.md` → 改两端代码 → 在本文"变更记录"留一行。
> 帧通用格式（RPMSG 与 K210 UART 同构，type 编码按通道独立分配，0x7E 两边均作心跳）：

```
| 帧头 0xAA 0x55 | type(1B) | seq(2B LE) | len(2B LE) | payload(len B) | CRC16(2B, 校验 type..payload) |
```
- 字节序：多字节字段小端（CAN 帧 lux 字段例外，按 CAN 章约定大端）。
- CRC16：**XMODEM**（poly 0x1021，init 0x0000，不反射、输出无异或）；帧尾 2B **低字节在前**；校验范围 type..payload（不含帧头）。
- seq：按（type 通道）独立计数；预览块 0x01 与帧尾 0x02 **共用预览计数器、单调递增**（接收端靠 seq 跳变丢帧）；其余 type 各自从 0 起。
- MTU：RPMSG ≤496B；UART 单帧 payload ≤1KB；超长分块同 seq 连发。

## 1. CAN 总线（C8T6 ↔ M4）—— 第2步实现

- 物理层：经典 CAN 2.0，500 kbps，标准帧（11bit ID），数据帧；总线两端 120Ω，共地。
- ID 分配：0x100 主→从指令 ｜ 0x110 主→从事件确认 ｜ 0x200 从→主事件 ｜ 0x210 从→主心跳（预留 0x1xx/0x2xx 段扩展）。

### 0x100 主→从 指令帧（DLC=8，未用字节 0）
| 字节 | 含义 |
|---|---|
| d[0] | 0x01 开闸 ｜ 0x02 关闸 ｜ 0x10 查询（从端回 0x200 带当前状态） |

### 0x110 主→从 事件确认帧（DLC=8，2026-09-08 新增）
| 字节 | 含义 |
|---|---|
| d[0] | 回显被确认的 0x200 d[0] 事件码：0x01 遮光/车到位 ｜ 0x00 恢复 |

- 主端(M4)每收到一帧 0x200（遮光事件或查询应答）自动回 0x110 作为回执；从端(C8T6)收到后 OLED 行4 显示确认标识 `CAN:*OK*`（约 1s）。

### 0x200 从→主 事件帧（DLC=8）
| 字节 | 含义 |
|---|---|
| d[0] | event：0x01=遮光/车到位 ｜ 0x00=恢复 |
| d[1..2] | lux（大端：高字节在前） |
| d[3] | drop%（相对基线掉点百分比） |
| d[4] | 状态位：bit0 闸开 bit1 遮光中 bit2 光感故障 bit3 CAN 错误 |

### 0x210 从→主 心跳帧（DLC=8，1Hz）
| 字节 | 含义 |
|---|---|
| d[0] | 同 0x200 d[4] 状态位 |
| d[1] | 上电秒数低 8 位（辅助判复位） |

- 超时：主端 3s 无 0x210 → 判从节点离线并上报 A7；从端不判主端。
- 位时序：C8T6 = 36MHz/9/(1+5+2) 采样点 75%；M4 = 按运行环境实测 FDCAN 时钟换算 500k（工程模式 PLL3Q=100MHz / Linux 引导=62.5MHz；m4_fw fdcan.c `FDCAN2_AutotuneBitTiming`，2026-09-08 更正）。

## 2. RPMSG（M4 ↔ Core0）—— 第3步实现

- 通道：`/dev/ttyRPMSG0`，raw 模式；M4 侧 OpenAMP endpoint。
- 帧 = 通用格式；**单帧 payload ≤480B**（总帧 ≤489B < ttyRPMSG0 MTU≈496B；本表无超长消息，跨帧分块规则预留，0x14 TLV 落地时再定块头）。
- 下行（Core0→M4）type：

| type | 含义 | payload |
|---|---|---|
| 0x11 | 开闸 | 空（M4 转 CAN 0x100 d[0]=0x01） |
| 0x12 | 关闸 | 空（M4 转 CAN 0x100 d[0]=0x02） |
| 0x13 | 查询全量状态 | 空 → M4 回 0x23 |
| 0x14 | 配置下发（预留） | TLV |

- 上行（M4→Core0）type：

| type | 含义 | payload 布局（偏移从 0 起，多字节小端） |
|---|---|---|
| 0x21 | CAN 事件转发 | 共 17B：`id[0..3]=CAN ID u32 LE` ｜ `dlc[4]=数据长度` ｜ `data[5..12]=8B 数据域（未用 0 填充）` ｜ `tick[13..16]=M4 本地毫秒 u32 LE` |
| 0x22 | 从节点离线/恢复 | 1B：0=离线 1=恢复在线 |
| 0x23 | M4 全量状态 | 共 4B：`b0=闸状态(0 关/1 开)` ｜ `b1=从节点在线(0 离线/1 在线)` ｜ `b2..b3=CAN 错误累计计数 u16 LE（饱和）` |
| 0x7E | 双向心跳（500ms） | 1B 序号（0..255 循环；发送方逐帧 +1，与帧头 seq 同向保鲜） |

- 0x21 的 `data` 按 CAN 章语义解释（0x200/0x210 的 d[0..4]：event/lux/drop/状态位），0x200 的 lux 在 CAN 内为大端、进 RPMSG 后仍按 CAN 字节原样搬运，不做字节序转换。
- 心跳：双向 500ms；**任一侧 1s 未收到任何有效帧 → LINK_DOWN**（0x7E 或 0x21 转发均可保鲜）；恢复后收方发 0x13 查询重同步。
- seq：发送方按 type 各自计数、单调递增；接收方按 type 记录 last_seq，跳变即计入丢帧统计（0x7E 双向各自计数不互扰）。

## 3. K210 UART（K210 ↔ Core1）—— 第5步实现

- 物理层：3.3V TTL，921600 起步（实测定值，目标 1.5Mbps）；无流控。
- type 定义：

| type | 方向 | 含义 | payload |
|---|---|---|---|
| 0x01 | ↑ | 预览 JPEG 块 | JPEG 分块数据 |
| 0x02 | ↑ | 预览帧尾标记 | 4B 本帧总长(LE) + 1B 用途（0=预览帧 / 1=识别抓拍帧） |
| 0xC1 | ↓ | CMD_RECOGNIZE 抓拍识别 | 空 |
| 0xC2 | ↑ | 识别结果 | JSON：`{"plate":"...","confidence":0.93,"ts":...}` |
| 0xC3 | ↑ | 识别失败 | 失败 JSON：`{"error":"no_plate"}` 等（单帧 ≤1KB） |
| 0x7E | 双向 | 心跳/状态 | bit0 busy（识别中） |

- 丢帧策略：seq 跳变容忍（预览流可直接丢）；识别线一次触发一次结果，busy 期间新触发丢弃。
- 抓拍 JPEG 上送：识别失败/低置信时，JPEG 数据走 0x01/0x02（用途=1）连发，随后 0xC3 带失败 JSON —— Core1 依"用途=1 的完整 JPEG = 供云兜底的抓拍图"处理；0x02 重组规则与预览一致（读前 4B 总长即可）。
- ts 语义：K210 上电毫秒计数（utime.ticks_ms），非墙钟。

## 4. ~~Modbus-TCP 寄存器表~~ —— 已取消（2026-09-11）

> 原「Core0 ↔ PC 上位机」Modbus-TCP 从站（0x0000~0x0041 寄存器表）**整体取消**：Demo 无 PC 上位机，
> 状态监控归 Core1 LCD（读共享内存）、远程开/关闸走共享内存请求 → RPMSG 0x11/0x12、白名单走
> `core0.conf` 本地配置热加载。寄存器表内容已从本文件删除，决策与替代方案见 `docs/protocols.md` §6。

## 5. 双 A7 共享内存 `/park_shm`（Core0 ↔ Core1）—— 第6步实现

```c
typedef struct {
    uint32_t magic, version;
    volatile uint32_t seq;            /* 写方递增；读方前后双读比对防撕裂 */
    /* core0 → core1 */
    int32_t  free_slots, used_slots;
    uint8_t  gate_state;              /* 0 关 1 开 */
    uint8_t  link_flags;              /* bit0 rpmsg bit1 m4在线 bit2 core1在线 */
    uint8_t  recog_pending;
    /* core1 → core0 */
    uint8_t  cloud_pending;           /* 云兜底进行中：Core0 识别超时 3s→6s（P7-04） */
    uint8_t  result_valid;            /* 读后 core0 清 */
    uint8_t  result_source;           /* 0=edge 1=cloud */
    float    confidence;
    char     plate[16];
    /* 心跳 */
    volatile uint32_t hb_core1;       /* core1 每 1s +1 */
} park_shm_t;
```
- eventfd 事件码：0x01 触发识别 ｜ 0x02 识别结果到达 ｜ 0x03 状态变更(UI) ｜ 0x04 重同步。
- 原则：图像不跨核（JPEG 留 Core1）；共享结构体一律带 version/seq；大块数据走双缓冲环形区。

## 6. 云端（Core1 → DeepSeek；可选 Core0 → MQTT）—— 第7步实现

- DeepSeek：HTTPS POST chat/completions（模型/端点以官方现行 Vision 能力为准，核实后回填）；图像 base64 dataURL；超时 5s；输出 JSON `{"plate","confidence"}`。
- MQTT 主题（可选）：`park/{sn}/status`（周期+变更）、`park/{sn}/events`（进出/开闸记录）、`park/{sn}/cmd`（下行参数）；离线队列补传带 seq 去重。

## 变更记录

| 日期 | 变更 | 影响端 |
|---|---|---|
| （建表日） | 初版随 PhaseMd 建立 | — |
| 2026-09-07 | K210 UART 定版：0x02 尾扩 1B 用途（0 预览 / 1 抓拍）；0xC3 改为仅失败 JSON（抓拍 JPEG 走 0x01/0x02 用途=1）；定 CRC16=XMODEM、seq 按 type 通道计数（0x01/0x02 共用预览单调计数）、ts=上电毫秒；固件路线定 CanMV MicroPython（不影响帧格式） | K210 / Core1 |
| 2026-09-07 | RPMSG §2 定版（Linux 侧先行）：单帧 payload ≤480B；0x11/0x12/0x13 空 payload；0x21=id(4B LE)+dlc(1B)+data(8B)+tick(4B LE)；0x22=1B；0x23=闸(1B)/在线(1B)/CAN错误计数 u16 LE；0x7E=1B 序号；1s 无有效帧即 LINK_DOWN；0x23 布局供 M4 第3步实现照做 | Core0 / M4 |
