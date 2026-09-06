# 协议规格总表（`docs/protocols.md` 草案母本）

> 用途：本文是四端（C8T6 / M4 / core0 / core1 / K210）字段级协议的**单一母本**，随各步实现逐步拷入并维护 `docs/protocols.md`。
> 改协议规则：先改本文 → 同步 `docs/protocols.md` → 改两端代码 → 在本文"变更记录"留一行。
> 帧通用格式（RPMSG 与 K210 UART 同构，type 编码按通道独立分配，0x7E 两边均作心跳）：

```
| 帧头 0xAA 0x55 | type(1B) | seq(2B LE) | len(2B LE) | payload(len B) | CRC16(2B, 校验 type..payload) |
```
- 字节序：多字节字段小端（CAN 帧 lux 字段例外，按 CAN 章约定大端）。
- MTU：RPMSG ≤496B；UART 单帧 payload ≤1KB；超长分块同 seq 连发。

## 1. CAN 总线（C8T6 ↔ M4）—— 第2步实现

- 物理层：经典 CAN 2.0，500 kbps，标准帧（11bit ID），数据帧；总线两端 120Ω，共地。
- ID 分配：0x100 主→从指令 ｜ 0x200 从→主事件 ｜ 0x210 从→主心跳（预留 0x1xx/0x2xx 段扩展）。

### 0x100 主→从 指令帧（DLC=8，未用字节 0）
| 字节 | 含义 |
|---|---|
| d[0] | 0x01 开闸 ｜ 0x02 关闸 ｜ 0x10 查询（从端回 0x200 带当前状态） |

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
- 位时序：C8T6 = 36MHz/9/(1+5+2) 采样点 75%；M4 = PLL3 100MHz/40/(1+3+1) 采样点 80%。

## 2. RPMSG（M4 ↔ Core0）—— 第3步实现

- 通道：`/dev/ttyRPMSG0`，raw 模式；M4 侧 OpenAMP endpoint。
- 下行（Core0→M4）type：

| type | 含义 | payload |
|---|---|---|
| 0x11 | 开闸 | 空（d[0]=0x01 由 M4 转 CAN） |
| 0x12 | 关闸 | 空 |
| 0x13 | 查询全量状态 | 空 → M4 回 0x23 |
| 0x14 | 配置下发（预留） | TLV |

- 上行（M4→Core0）type：

| type | 含义 | payload |
|---|---|---|
| 0x21 | CAN 事件转发 | CAN id(4B)+dlc(1B)+8B data+tick(4B) |
| 0x22 | 从节点离线/恢复 | 1B（0=离线 1=恢复） |
| 0x23 | M4 全量状态 | 闸状态/从节点在线/错误计数 |
| 0x7E | 双向心跳（500ms） | 1B 序号 |

- 心跳：双向 500ms；任一侧 1s 未收到 → LINK_DOWN；恢复后收方发 0x13/查询重同步。

## 3. K210 UART（K210 ↔ Core1）—— 第5步实现

- 物理层：3.3V TTL，921600 起步（实测定值，目标 1.5Mbps）；无流控。
- type 定义：

| type | 方向 | 含义 | payload |
|---|---|---|---|
| 0x01 | ↑ | 预览 JPEG 块 | JPEG 分块数据 |
| 0x02 | ↑ | 预览帧尾标记 | 4B 本帧总长 |
| 0xC1 | ↓ | CMD_RECOGNIZE 抓拍识别 | 空 |
| 0xC2 | ↑ | 识别结果 | JSON：`{"plate":"...","confidence":0.93,"ts":...}` |
| 0xC3 | ↑ | 识别失败 | 抓拍 JPEG + `{"error":"no_plate"}` |
| 0x7E | 双向 | 心跳/状态 | bit0 busy（识别中） |

- 丢帧策略：seq 跳变容忍（预览流可直接丢）；识别线一次触发一次结果，busy 期间新触发丢弃。

## 4. Modbus-TCP 寄存器表（Core0 ↔ 上位机）—— 第4步实现

| 地址 | R/W | 说明 |
|---|---|---|
| 0x0000 | R | 空闲车位数 |
| 0x0001 | R | 已停车辆数 |
| 0x0002 | R | 闸状态（0 关/1 开） |
| 0x0003 | R | 最近车牌长度 |
| 0x0004~0x000D | R | 最近车牌 ASCII（≤10 字符） |
| 0x0010~0x002F | R/W | 白名单槽 0~7（每槽 8 字符 = 4 寄存器） |
| 0x0030 | W | 远程开闸脉冲（写 1 生效，自清零） |
| 0x0031 | W | 远程关闸脉冲 |
| 0x0040 | R | 故障字：bit0 rpmsg断 bit1 M4/CAN离线 bit2 core1失联 bit3 CAN错误 bit4 云兜底失败 |
| 0x0041 | R | 心跳计数（每秒 +1） |

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
