# 接口协议规格（字段级，正式维护副本）

> 四端（C8T6 / M4 / core0 / core1 / K210）字段级协议的**正式维护副本**；草案母本 = `PhaseMd/10_协议规格总表_protocols草案.md`。
> 维护规则：改协议**先改母本** → 同步本文件 → 改两端代码 → 两份"变更记录"各留一行。
> 拷入进度：各通道随其实现步骤从母本拷入；尚未拷入的通道见 §4，正文以母本为准。

## 0. 通用帧格式（RPMSG 与 K210 UART 同构，type 编码按通道独立分配，0x7E 两边均作心跳）

```
| 帧头 0xAA 0x55 | type(1B) | seq(2B LE) | len(2B LE) | payload(len B) | CRC16(2B, 校验 type..payload) |
```

- 字节序：多字节字段小端（CAN 帧 lux 字段例外，按 CAN 章约定大端）。
- CRC16：**XMODEM**（poly 0x1021，init 0x0000，不反射、输出无异或）；帧尾 2B **低字节在前**；校验范围 type..payload（不含帧头）。
- seq：按（type 通道）独立计数；预览块 0x01 与帧尾 0x02 **共用预览计数器、单调递增**（接收端靠 seq 跳变丢帧）；其余 type 各自从 0 起。
- MTU：RPMSG ≤496B；UART 单帧 payload ≤1KB；超长分块同 seq 连发。

## 1. CAN 总线（C8T6 ↔ M4）—— 已实现（C8T6 侧代码落地，M4 联调中；2026-09-07 拷入）

- 物理层：经典 CAN 2.0，500 kbps，标准帧（11bit ID），数据帧；总线两端 120Ω，共地。
- ID 分配：0x100 主→从指令 ｜ **0x110 主→从事件确认** ｜ 0x200 从→主事件 ｜ 0x210 从→主心跳（预留 0x1xx/0x2xx 段扩展）。

### 0x100 主→从 指令帧（DLC=8，未用字节 0）
| 字节 | 含义 |
|---|---|
| d[0] | 0x01 开闸 ｜ 0x02 关闸 ｜ 0x10 查询（从端回 0x200 带当前状态） |

### 0x110 主→从 事件确认帧（DLC=8，2026-09-08 新增）
| 字节 | 含义 |
|---|---|
| d[0] | 回显被确认的 0x200 d[0] 事件码：0x01 遮光/车到位 ｜ 0x00 恢复 |
| d[1..7] | 0 |

- 主端(M4)每收到一帧 0x200（遮光事件或查询应答）自动回 0x110 作为回执；从端(C8T6)收到后 OLED 行4 显示确认标识 `CAN:*OK*`（约 OLED_ACK_FLASH_MS=1s）。

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
- 位时序：C8T6 = 36MHz/9/(1+5+2) 采样点 75%；M4 = FDCAN 内核时钟**随运行环境不同**——工程模式(CubeIDE)=PLL3Q 100MHz(.ioc)，Linux 引导/remoteproc=62.5MHz(实测)——固件启动时实测并自动换算 500k（`m4_fw` fdcan.c `FDCAN2_AutotuneBitTiming`；100MHz→Prescaler40/(1+3+1)/80%，62.5MHz→Prescaler25/(1+3+1)/80%）。

## 2. K210 UART（K210 ↔ Core1）—— 第5步开发中（2026-09-07 拷入，`k210_fw/` 骨架已生成）

- 物理层：3.3V TTL，921600 起步（实测定值，目标 1.5Mbps）；无流控；REPL 走板子 USB CDC（115200），与数据 UART 物理分离，不混用。
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
- 字符集：plate 为 UTF-8（省份简称汉字 + 字母 + 数字）；Core1 校验口径见 PhaseMd/06 P5-11（`RECOG_CONF_TH` 初值 0.60，第7步联动云兜底）。

## 3. RPMSG（M4 ↔ Core0）—— 已实现（Linux 侧代码落地 2026-09-07 拷入；M4 网关 `m4_fw` rpmsg_bridge 代码就绪 2026-09-10，待 CubeMX 勾 OPENAMP Regenerate + 板验，见 `m4_fw/rpmsg.md`）

> 注：本文件 §3 对应母本 `PhaseMd/10` 的 §2（两文档章节号不同，以内容名为准）。

- 通道：`/dev/ttyRPMSG0`，raw 模式；M4 侧 OpenAMP endpoint；ttyRPMSG0 归 Core0 独占。
- 帧 = §0 通用格式；**单帧 payload ≤480B**（总帧 ≤489B < MTU≈496B；本表无超长消息，跨帧分块规则预留，0x14 TLV 落地时再定块头）。
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

- 0x21 的 `data` 按 §1 CAN 语义解释（0x200/0x210 的 d[0..4]：event/lux/drop/状态位），0x200 的 lux 在 CAN 内为大端、进 RPMSG 后仍按 CAN 字节原样搬运，不做字节序转换。
- 心跳：双向 500ms；**任一侧 1s 未收到任何有效帧 → LINK_DOWN**（0x7E 或 0x21 转发均可保鲜）；恢复后收方发 0x13 查询重同步。
- seq：发送方按 type 各自计数、单调递增；接收方按 type 记录 last_seq，跳变即计入丢帧统计（0x7E 双向各自计数不互扰）。

## 4. 双 A7 共享内存 `/park_shm`（P6-01；v3 2026-09-11 定版）

> 权威定义 = `core0_service/ipc_shm/park_shm.h`（Writer：Core0 业务守护；Reader：`core1_ui/qt_gui/src/park_shm.h`，**两份必须逐字节一致**，头文件内含编译期布局断言防止漂移）。写方 `shm_open("/park_shm", O_CREAT|O_RDWR, 0600)` + `ftruncate(sizeof(park_shm_t))`；读方 `O_RDWR` + `mmap(PROT_READ|PROT_WRITE, MAP_SHARED)`（读方也**必须可写**：心跳/结果/远程请求由 Core1 写入，见 §4.3）。

`park_shm_t`（`sizeof = 76`，自然对齐，无 `#pragma pack`）：

| 偏移 | 字段 | 类型 | 归属 | 说明 |
|---|---|---|---|---|
| 0 | `magic` | u32 | Core0 | `0x5041524B`（"PARK"） |
| 4 | `version` | u32 | Core0 | **当前 = 3**；版本不符读写双方均拒绝/告警 |
| 8 | `seq` | volatile u32 | Core0 | 写方更新前后各 +1；读方整块读后再读 seq，不等则重试（防撕裂读） |
| 12 | `free_slots` | i32 | Core0 | 空闲车位，限幅 [0, `total_slots`] |
| 16 | `used_slots` | i32 | Core0 | 已停车位；`free+used == total` |
| 20 | `gate_state` | u8 | Core0 | 0 关 / 1 开（以 0x23 实测状态校正） |
| 21 | `link_flags` | u8 | Core0 | bit0 rpmsg ｜ bit1 M4 在线 ｜ bit2 Core1 在线 |
| 22 | `recog_pending` | u8 | Core0 | 1 = 有车待识别（登记时置 1，出结果/超时清 0） |
| 23 | `cloud_pending` | u8 | **Core1** | 1 = 云兜底进行中 → Core0 识别超时 3s 延长至 6s 硬上限 |
| 24 | `result_valid` | u8 | **Core1** | 1 = 结果有效；Core0 读走并清 0 |
| 25 | `result_source` | u8 | **Core1** | 0 = 端侧(K210) / 1 = 云兜底 |
| 28 | `confidence` | float | **Core1** | 0~1（阈值判定在 Core1，Core0 不做比较） |
| 32 | `plate[16]` | char[16] | **Core1** | UTF-8 中文车牌，≤15B + NUL |
| 48 | `hb_core1` | volatile u32 | **Core1** | 每 1s +1；Core0 每 3s 检查无变化 → 判失联（故障字 bit2） |
| 52 | `req_gate_open` | volatile u8 | **Core1** | 脉冲：写 1 生效，Core0 执行后自清 |
| 53 | `req_gate_close` | volatile u8 | **Core1** | 脉冲：同上（走 0x12） |
| 54 | `fault_bits` | volatile u8 | Core0 | 故障字，见 §4.4 |
| 56 | `conf_threshold` | float | Core0 | 识别置信度阈值（Core0 为配置权威，下发给 Core1 判定用），默认 0.60 |
| 60 | `evt_seq_c0` | volatile u32 | Core0 | **v3**：置 `evt_bits_c0` 后 +1（事件发布点） |
| 64 | `evt_bits_c0` | volatile u8 | Core0 | **v3**：Core0→Core1 事件位掩码（`PARK_EVB(code)`） |
| 68 | `evt_seq_c1` | volatile u32 | **Core1** | **v3**：置 `evt_bits_c1` 后 +1 |
| 72 | `evt_bits_c1` | volatile u8 | **Core1** | **v3**：Core1→Core0 事件位掩码 |

### 4.1 事件码与跨进程事件通道（v3）

| 码 | 位（`PARK_EVB`） | 方向 | 含义 |
|---|---|---|---|
| `0x01` | `0x02` | Core0 → Core1 | 触发识别（车辆登记完成） |
| `0x02` | `0x04` | Core1 → Core0 | 识别结果就绪 |
| `0x03` | `0x08` | Core0 → Core1 | 业务状态变更（UI 刷新） |
| `0x04` | `0x10` | Core0 → Core1 | 请求重同步（链路恢复后） |

- **为什么不用 eventfd**：`eventfd(2)` 的匿名 fd 无法被另一个独立进程打开，且 `EFD_CLOEXEC` 使其不能跨 `exec` 传递；本 Demo 中 Core0/Core1 是两个独立进程且无 fd 传递通道（systemd 亦不传 fd）。故 v3 用共享内存事件字实现同语义：**生产方先置位、再 +1 序号；消费方轮询序号变化后读取位并清除**。轮询周期 200ms（Core1 tick），满足 spec 4.1.3 的“状态刷新 ≤500ms”。
- 同进程/本地调试仍可用 `ipc_evt_*`（eventfd）快速路径，与 shm 事件通道互不影响（`--evt-c1` 传入 fd 时两条路都会走）。

### 4.2 一致性与版本

1. 写方启动即 `memset` + 盖 `magic/version` 并 `ftruncate` 到自身结构大小；读方校验 `magic` 与 `version`，**不符则拒绝挂载并打印 ERROR**（不再静默）。
2. `seq` 撕裂保护：读方整块拷贝 → 再读 `seq`，不等则重试（最多 3 次）。
3. 两端必须**同时重建**（版本 3）。旧二进制（v1/v2）与新二进制不兼容，读方会拒绝挂载。
4. 头文件含 `PARK_SHM_ASSERT` 编译期布局断言（`sizeof==76` 及关键字段偏移），两份副本一旦漂移即编译失败。

### 4.3 职责划分（谁写哪些字段）

- **Core0 写**：`magic/version/seq/free_slots/used_slots/gate_state/link_flags/recog_pending/fault_bits/conf_threshold/evt_*_c0`。
- **Core1 写**：`hb_core1/cloud_pending/result_valid/result_source/confidence/plate/req_gate_open/req_gate_close/evt_*_c1`。
- ⚠️ Core1 必须**以 `O_RDWR` 打开并 `PROT_WRITE` 映射**：只读打开会让心跳/结果/远程请求永远写不进去，Core0 会在 3s 后判 Core1 失联并跳过识别（已验证的集成陷阱）。

### 4.4 故障字 `fault_bits`

| 位 | 含义 |
|---|---|
| bit0 | rpmsg 断链（LINK_DOWN 置 1，恢复清 0） |
| bit1 | M4 侧从节点（C8T6）离线（0x22 上报 0 置 1） |
| bit2 | Core1 失联（心跳 3s 无变化置 1） |
| bit3 | CAN 错误（0x23 上抛的 CAN 错误计数非零） |
| bit4 | 云兜底失败（第7步起生效，本步预留） |

## 5. 云端 HTTP（Core1 `park_ui` → OpenAI 兼容 `/chat/completions`）—— 第7步实现（2026-09-11 拷入）

**通道性质**：单向请求/响应（HTTPS，无长连接、无轮询）。**只有 Core1 能发起**（PhaseMd/08：Core0 禁云请求；静态门禁 `core1_ui/qt_gui/tools/check_static.py` 扫 `core0_service/` 有无 curl/QNetwork）。传输层实现 = Qt Network（`QNetworkAccessManager`，板端 Qt 5.12.8 自带；**不用 libcurl**）。**`transport=` 三档**：`auto`（默认，Qt 失败自动切 python3 子进程并在本进程内保持）/ `qt`（只用 Qt）/ `python`（只用 python3）；起因是**本板 Qt 5.12.8 + OpenSSL 1.1.1 在 TLS 1.3 协商上卡死**（2026-09-11 实测：同一条请求 python 0.2s 握手、Qt 超时），python 通道用标准库 `urllib`+`ssl`，脚本内嵌在二进制里（无需部署额外文件），job 文件（含 key）写 `/tmp`、0600、进程退出即删。**代理默认强制直连**（`QNetworkProxy::NoProxy`），`proxy=` 可设 `env` 或 `http://host:port`。

### 5.1 请求

```
POST <apiBase>                       # 默认 https://api.deepseek.com/chat/completions
Content-Type: application/json
Authorization: Bearer <apiKey>       # key 只在内存 + /etc/park/cloud.conf，永不进日志

{"model":"<model>",                  # 默认 deepseek-chat；设置页可改或轮换预设
 "messages":[{"role":"user","content":[
   {"type":"text","text":"<prompt>"},
   {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,<...>"}}]}],
 "max_tokens":64,"temperature":0}
```

| 项 | 取值/约束 |
|---|---|
| 图像 | **最近一帧 K210 预览**（`K210Link::latestFrame()` 非消费取帧），JPEG q70 → base64，帧 ≤ ~10KB、body ≈ 13KB |
| `<prompt>` | 可配置（默认见 `cloud_settings_defaults()`），要求**只输出 JSON**：`{"plate":"省份简称+字母+5位","confidence":0.95}`，识别不到则空串+0 |
| 超时 | `timeoutMs` 默认 **5000ms** 硬上限（`QTimer` → `reply->abort()`；Qt 5.12 **无** `setTransferTimeout`） |
| 重试 | **≤1 次且仅传输类错误**；4xx/5xx 不重试 |
| 并发 | **单次触发单次调用**，同一时刻只允许一个在飞请求（`busy` 时新请求被忽略并记日志） |

### 5.2 响应与容错解析

`choices[0].message.content` → 文本；`error` 字段存在则直接判失败（key/余额/限流的中文提示原样进滑窗）。

解析链（三级容错，任一级失败 = "云兜底失败"，**不置 `result_valid`**）：
1. 去 ``` 围栏、去前后杂文；
2. **括号配对扫描**出第一个平衡的 JSON 对象（容忍截断/多余文字）；
3. `QJsonDocument` 解析 → `plate` 非空且 UTF-8 字节 ≤15（与 shm `plate[16]` 对齐，超出截断）、`confidence` 夹取到 [0,1]。

| 结果分类 | 条件 | Core1 动作 |
|---|---|---|
| accepted | HTTP 200 且 `plate` 非空且 `confidence >= acceptConf`（默认 0.50） | `IpcWriter::onCloudResult()` → shm `plate/confidence/result_source=1` + `result_valid=1` + `evt RESULT`，清 `cloud_pending` |
| unreadable | HTTP 200 但无车牌/置信度低于 `acceptConf` | 清 `cloud_pending`，不写结果（走降级） |
| failed | 传输错误/超时/非 200/解析失败 | 清 `cloud_pending`，滑窗提示（401 key、402 余额、404 模型名、429 限流、5xx 服务端） |

### 5.3 与 Core0 的衔接（P7-04 定稿）

低置信或端侧失败时，`IpcWriter` 置 shm `cloud_pending=1` 并 `emit cloudFallbackRequested(reason)`；`main.cpp` 据此（受"自动兜底"开关约束）取最新帧发云请求。**任何出口**（accepted/unreadable/failed/开关关闭/无帧）都会落到 `onCloudResult()` 或 `clearCloudPending()` ⇒ `cloud_pending` 必然归零 ⇒ Core0 的 3s→6s 延长必然收敛。

### 5.4 配置与密钥（P7-07）

`/etc/park/cloud.conf`（`key=value` 纯 ASCII，模式 600；`$PARK_CLOUD_CONF` 改路径、`$DEEPSEEK_API_KEY` 在 key 为空时注入）。字段：`api_base/api_key/model/prompt/trigger_conf/accept_conf/timeout_ms/retry/ca_file/proxy/transport/auto_fallback/writeback/insecure_tls/fake_result` **+ 每个 provider 一把 key 的 `key_<provider>=`**（provider id 由 endpoint host 判定：`deepseek`/`dashscope`/`openai`/`custom`）。`api_key` 是**当前生效**那把，`key_<provider>` 是各家的备份 key——LCD 的 PRESET 切模型时会同时切 endpoint 并**自动取用该 provider 的 key**（否则会拿上一家的 key 去请求，服务端回 `incorrect api key`；这是板上实测踩过的坑）。保存走 `QSaveFile` 原子替换 + 旧内容留 `.bak`，落盘后强制 0600。**该文件在仓库外 ⇒ 天然不入 git**，代码与文档里只出现 `cloud_settings_mask_key()` 的 `sk-abcd...wxyz` 掩码。

**TLS/CA（板级事实，2026-09-11 实测）**：本板 rootfs **没有** `/etc/ssl/certs/ca-certificates.crt`（有 `libssl.so.1.1`/`libcrypto.so.1.1`，无 `curl`）。CA 查找顺序 = `ca_file=` → `/etc/park/ca.pem` → 系统常规位置；三者皆无时 HTTPS 必然失败，日志与设置页明确报 `CA=NONE` 并给出"装 CA 或临时 `insecure_tls=1`"的处置。启动还会打印 `QSslSocket::supportsSsl()` 与 SSL 库版本（判断 Qt 是否编进了 OpenSSL）。

### 5.5 阈值口径（不新增端侧阈值）

| 阈值 | 归属 | 默认 | 作用 |
|---|---|---|---|
| `conf_threshold` | **Core0**（shm 偏移 56） | 0.60 | 端侧判"低置信"（Core0 业务用） |
| `trigger_conf` | Core1（cloud.conf） | 0.60 | **是否发起云请求**（设置页可改） |
| `accept_conf` | Core1（cloud.conf） | 0.50 | **是否接受云答案**（设置页可改） |

设置页只读展示 Core0 的 `conf_threshold`（spec 5.5.1：端侧阈值只允许一个）。

### 5.6 WiFi 配置（板载唯一网络链路，非协议但同属第7步运维面）

`ip link set wlan0 up` → `wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf` → `udhcpc -i wlan0 -n -q -t 5 -T 3`；写配置前备份 `/etc/wpa_supplicant.conf.bak`，**20s 内未拿到 IPv4 自动回滚并重新应用**（改 WiFi 等于拆自己脚下的 SSH 梯子）。板端无 `pgrep/pkill/timeout`，杀旧 `wpa_supplicant` 用 `/proc` 扫描（并跳过 `$$`）。

## 6. 尚未拷入的通道（母本 `PhaseMd/10` 保留正文）

| 通道 | 内容概要 | 状态 |
|---|---|---|
| Modbus-TCP 寄存器表（Core0 ↔ 上位机） | 原 0x0000~0x0041 寄存器映射 | **已废弃**（2026-09-11：Demo 无上位机；监控归 Core1 UI 读共享内存、远程控制走 0x11/0x12、白名单改本地配置热加载） |
| MQTT 上报主题（可选，P7-08~10） | `park/{sn}/status|events|cmd` | 第7步可选项，未实现（接口预留） |

## 变更记录

| 日期 | 变更 | 影响端 |
|---|---|---|
| 2026-09-07 | 初建：从母本拷入 CAN（§1）与 K210 UART（§2）；含 CRC16=XMODEM / seq 计数 / 0x02 用途字节 / 0xC3 语义定版 | C8T6 / M4 / K210 / Core1 |
| 2026-09-07 | RPMSG 拷入为 §3：payload ≤480B；0x11/0x12/0x13 空；0x21=id(4B LE)+dlc(1B)+data(8B)+tick(4B LE)；0x22=1B；0x23=闸(1B)/在线(1B)/CAN错误计数 u16 LE；0x7E=1B 序号；1s 无有效帧 LINK_DOWN。对应 `core0_service/rpmsg/` 代码落地（待板端编译联调） | Core0 / M4 |
| 2026-09-08 | M4 位时序二次更正：运行环境时钟不同（工程模式=PLL3Q 100MHz，Linux 引导=62.5MHz），静态配法不可两全 → 改为启动实测 + 自动换算（FDCAN2_AutotuneBitTiming） | M4 |
| 2026-09-08 | 新增 0x110 主→从 事件确认帧：主端收 0x200 自动回执(d[0] 回显事件码)，从端 OLED 行4 显示 `CAN:*OK*` | M4 / C8T6 |
| 2026-09-11 | 共享内存正式拷入为 §4（v3）：`park_shm_t` 全字段/偏移、事件码与**跨进程事件通道**（匿名 eventfd 不可跨进程 → 改用 `evt_bits_c0/c1`+`evt_seq_c0/c1`，200ms 轮询）、Core0/Core1 字段归属、故障字位定义；同时明确 Core1 必须 `O_RDWR` 挂载（只读会让心跳/结果写不进 → Core0 判失联）。头文件加编译期布局断言（sizeof=76）。Modbus-TCP 通道标记废弃 | Core0 / Core1 |
| 2026-09-11 | 新增 §5 云端 HTTP 通道（第7步）：OpenAI 兼容 `/chat/completions` 请求体、base64 图像、三级容错解析、accepted/unreadable/failed 三分类、超时 5s/重试 ≤1/单次触发、`cloud_pending` 收敛保证、三阈值口径（Core0 `conf_threshold` vs Core1 `trigger_conf`/`accept_conf`）、`/etc/park/cloud.conf` 密钥策略、WiFi 三步配置 + 20s 回滚；§6 保留"尚未拷入"（MQTT 可选） | Core1（Core0 禁云请求） |
