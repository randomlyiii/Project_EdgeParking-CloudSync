# 第 7 步 · Linux ↔ 云端通讯（DeepSeek 兜底 + 可选云平台）

> 步骤：7 ｜ 前置：G6（本地全闭环） ｜ 产出：边云协同实时兜底 +（可选）MQTT 云平台/轻量文件记录 ｜ 验收门：G7
> 边云协同两种形态（README）：①识别置信度不足时调 **DeepSeek Vision API**（实时，本步必做）；②云平台数据上报（**可选**，P7-09~11）。
> 约束：云请求**单次触发调用**不循环；5s 超时不阻塞本地业务与开闸降级；云故障时设备本地照常运行。

## 1. DeepSeek Vision 兜底（Core1，实现在 `core1_ui/qt_gui/src/`）

> **实现说明（2026-09-11）**：本步云模块**落在 Qt 应用 `park_ui` 内部**（`cloud_settings.*` / `cloud_client.*` / `wifi_manager.*` / `settingspage.*` / `softkeyboard.*`），而不是文档最初写的独立目录 `core1_ui/cloud_api/`——因为 **Core1 = park_ui 进程**，跨进程再拆一个 HTTP 客户端只会多一条 IPC 和一份配置。**Core0 (`core0_service`) 仍然一行云代码都没有**（静态门禁 `tools/check_static.py` 会扫 core0_service 有无 curl/QNetwork）。
> 传输层用 **Qt Network**（`QNetworkAccessManager`）而非 libcurl：板端 Qt 5.12.8 自带 QtNetwork，异步不阻塞 UI 线程，交叉编译**零新增依赖**。若板端实测缺 `libQt5Network.so`/CA 证书，退化顺序 = ① 补 SDK 库；② `python3 urllib+ssl` 外部进程；③ 引 libcurl。
> **LCD 上新增的操作入口**（用户追加需求）：设置页可改 **触发/接受置信度阈值、API 地址、模型名、API key（掩码显示）**，可**主动要求云端复检**，可**自动检测 WiFi 并改 SSID/密码**（带 20s 自动回滚），另有 OpenAPI 兼容预设模型轮换、测试连接、假结果/断网演练开关、诊断页（systemd/设备节点/shm）。

### P7-01 云请求传输层（Qt Network，替代 libcurl）🟡
- 原计划 Core1 链 libcurl；实测定稿为 **QtNetwork 异步 POST**（见上）。HTTPS 证书走系统 CA（`/etc/ssl/certs`），设置页提供 **insecure_tls** 逃生开关（演示现场自签/缺证书时用，日志会打印告警）。
- **板端探针实测（2026-09-11，`root@100ask`）**：`/usr/lib/libQt5Network.so.5.12.8` ✅、`libssl.so.1.1`+`libcrypto.so.1.1` ✅、`iw`/`wpa_cli`/`udhcpc`/`wpa_supplicant` ✅（**全在 `/sbin`**，代码已改为绝对路径调用）、`python3` ✅；**`/etc/ssl/certs/ca-certificates.crt` ❌ 不存在、`curl` ❌ 无** ⇒ ①"退化到 curl/libcurl"作废；② **HTTPS 必须自带 CA**：拷 bundle 到 `/etc/park/ca.pem`（代码自动优先使用）或配 `ca_file=`，否则只好临时 `insecure_tls=1`；③ 启动日志打印 `supportsSsl`/SSL 库版本/实际 CA 路径，设置页「云端」底部同步显示 `tls=...`。
- **验收**：板上「测试连接」返回 HTTP 200；日志出现 `cloud: Qt TLS supportsSsl=yes (...), CA=/etc/park/ca.pem`。

### P7-02 请求构造 ✅（图像源按现状改）
- 图像源：**取最近的 K210 预览帧**（`K210Link::latestFrame()` 非消费式取帧 → JPEG q70 → base64）。原计划"优先 0xC3 失败抓拍"未启用——K210 当前是 **console 文本链路**，没有下行抓拍通道；`latestFrame()` 已是同一块画面的最新帧，语义等价。
- 请求体见下方 JSON（`Authorization: Bearer <key>`，key 只在内存与 `/etc/park/cloud.conf` 出现，**从不进日志**）。
- **验收**：`cloud_client.cpp` 单帧 base64 体积 ≈ 13KB（P7-02 内存口径 OK，不缓存多帧）。

```json
POST <apiBase>            // 默认 https://api.deepseek.com/chat/completions
Authorization: Bearer $DEEPSEEK_API_KEY
{"model":"<model>",
 "messages":[{"role":"user","content":[
   {"type":"text","text":"<P7-03 Prompt>"},
   {"type":"image_url","image_url":{"url":"data:image/jpeg;base64,<...>"}}]}],
 "max_tokens":64,"temperature":0}
```

### P7-03 Prompt 约束输出 ✅
- Prompt 作为**可配置项**（设置页可改，默认见 `cloud_settings_defaults()`），要求只输出 `{"plate":"...","confidence":0.95}`，无法识别则空字符串+0。
- 解析侧不依赖模型的自觉：`extractContent()` 先剥离 ``` 围栏与前后杂文，`parsePlateJson()` 再**括号配对扫描**出第一个平衡 JSON 对象。
- **验收**：20 张测试图 + 3 种脏响应（围栏/纯文本/截断）在 P7-05 用例内安全落地（宿主机侧待补脚本）。

### P7-04 超时与重试策略 ✅
- `timeoutMs=5000` 硬超时：`QTimer` 单发 → `reply->abort()`（Qt 5.12 无 `setTransferTimeout`）；
- **单次触发单次调用**（识别事件驱动，无轮询）；**失败重试 ≤1**，且**仅对传输类错误**重试（4xx/5xx 不重试，直接按提示分类失败：401 key、402 余额、404 模型名、429 限流、5xx 服务端）；
- 与 Core0 的超时衔接：低置信/识别失败 → `IpcWriter` 置 `cloud_pending=1` 并 `emit cloudFallbackRequested` → Core1 发云请求；**无论成功/失败/无法识别/开关关闭，最终都会 `clearCloudPending()` 或写回结果**，Core0 的 3s→6s 延长必然收敛。
- **验收**：断网注入（设置页"断网演练"开关）→ 1.2s 内返回失败、`cloud_pending` 归零、本地业务不阻塞。

### P7-05 响应解析容错 ✅
- `extractContent()`：JSON body → `choices[0].message.content`，同时识别 `error` 字段（key 无效/余额不足等中文提示直接上滑窗）；
- `parsePlateJson()`：剥离围栏 → 括号配对 → `QJsonDocument` → 校验 `plate` 字符集（UTF-8 截断到 15 字节，与 shm `plate[16]` 对齐）与 `confidence` 夹取到 [0,1]；
- 解析失败 = HTTP 200 但无车牌 → `unreadable` → 走"云兜底失败"降级路径（**不置 result_valid**）。
- **验收**：见上，脏响应不崩不误开闸。

### P7-06 触发策略 ✅
- 触发条件：K210 置信度 < **`trigger_conf`（默认 0.60，LCD 可改）**、识别失败；另有**手动触发**（底栏「云端复检」/设置页按钮，受同一条开关约束）；
- 接受门限：模型给的 `confidence >= accept_conf`（默认 0.50，LCD 可改）才认，否则按 unreadable 降级；
- 结果回注：`IpcWriter::onCloudResult()` 写 `plate/confidence/result_source=1` + `result_valid=1` + `evt RESULT`（先字段后 valid 的释放序），Core0 逻辑不区分来源、**照样过白名单**；
- `writeback=0`（设置页可关）时结果只打日志 + 弹卡，不写 shm。
- **验收**：低置信样本自动走云 → `result_source=1` → 白名单开闸。

### P7-07 API Key 管理 ✅
- 落地为 **`/etc/park/cloud.conf`**（`$PARK_CLOUD_CONF` 可覆盖路径，`$DEEPSEEK_API_KEY` 可在 key 为空时注入），模式 600、目录 `/etc/park/`，**不在仓库内 → 天然不入 git**；文档里 P7-07 原写的 `cloud.env` 语义由本文件承担；
- 进程内 key 只在 `CloudSettings::apiKey`（构造 → `CloudClient::setSettings`），日志一律 `cloud_settings_mask_key()` 输出 `sk-abcd...wxyz`；设置页输入框也只回显掩码；
- 静态门禁新增**负向断言**：任何含 `qWarning/qDebug/printf` 的行里出现 `apiKey` 即 FAIL；另扫 GUI 源码里像 `sk-xxxx` 的字面量。
- **验收**：`grep -rn "sk-" core1_ui/` 只命中掩码函数与注释；仓库内无 key。

### P7-12 LCD 上云/WiFi 运维入口（用户追加，2026-09-11）🟡
- 齿轮 → 全屏设置页三页签：
  - **云端**：`trigger_conf`/`accept_conf`（±0.05 步进，点击即存）、API 地址、模型名（含常用预设轮换）、API key（掩码 + 软键盘输入）、5 个开关（自动兜底/结果回写/跳过证书校验/假结果/断网演练）、测试连接、云端复检、保存、统计行；**Core0 的 `conf_threshold` 只读展示**（端侧阈值只允许一个，spec 5.5.1）；
  - **网络**：wlan0 状态/IP/网关/SSID/信号、SSID+密码（软键盘）连接、断开、扫描（`iw scan`，无 iw 时退 `wpa_cli scan`）、恢复备份、20s 自动回滚说明；
  - **诊断**：`systemctl is-active board-power/m4-load/core0-bus/park-ui`、`/dev/ttyRPMSG0`、`/dev/shm/park_shm`、`/dev/ttyACM0` 在位性。
- 底栏常驻 **WIFI 芯片**（2s 轮询，有租约绿=`SSID`，**不回显 IP/网关**——公开 LCD 不暴露本机地址，2026-09-11 用户要求）与 **CLOUD 芯片**（空闲/成功/无 key/失败分类）。
- 手动触发的实现放在这一步内：底栏「云端复检」→ `CloudClient::recognize(latestFrame, writeback)`。
- **验收**：触摸板端逐项点通（含改错密码 20s 后自动回滚仍能 SSH）。

## 2. 云平台上报（可选，Task 1.6 / 阶段 4）

### P7-08 MQTT 接入 ⬜
- Broker 选型：局域网 mosquitto（演示）或公有云 EMQX；Core0 侧用 paho.mqtt.c / libmosquitto，**低优先级线程**，不与 1.1~1.3 抢核抢时。
- 主题设计（写入文档 10 云端章）：
  - `park/{sn}/status`：车位/闸/故障字/心跳（周期 + 变更）
  - `park/{sn}/events`：进出记录、开闸记录
  - `park/{sn}/cmd`（下行，可选）：远程参数配置
- **验收**：MQTTX 订阅端能看到上报。

### P7-09 离线缓存与补传 ⬜
- 断网时本地队列（内存 + 可选落盘）暂存，重连后按 seq 补传、去重。
- **验收**：拔网 2 分钟再恢复，事件不丢不重。

### P7-10 轻量文件记录启用（联动 P4-07）⬜
- 若云上报需要历史查询，打开 `ENABLE_STORAGE` 验证联动（`events.log`/`gate_log` 既是本地留痕也是补传数据源）。**2026-09-11 用户定调：只用轻量文件记录，不上 SQLite。**
- **验收**：断网期间记录落文件，恢复后补传完成。

### P7-11 云故障不影响本地（验收红线）🟡
- 注入：断外网、云 API 5xx、Broker 不可达——本地闭环（第6步 P6-06 场景）全部照常。
- **代码侧已备三种注入**：设置页「断网演练」（请求不出网卡、1.2s 本地失败）、「假结果」（离线演示），API 侧 401/402/404/429/5xx 分类提示；**板端三场景复测待做**。
- **验收**：三注入场景下 P6-06 全过。

## 3. 验收门 G7

- [ ] P7-01~03 请求构造 + Prompt 板上打通（🟡 代码就绪：Qt Network + 可配置 Prompt）
- [ ] P7-04 5s 超时、单次调用、失败不阻塞（🟡 代码就绪）
- [ ] P7-06 低置信自动云兜底、结果与端侧同格式回流（🟡 代码就绪）
- [ ] P7-07 Key 不入库（✅ 静态门禁已含负向断言；`/etc/park/cloud.conf` 在仓库外）
- [ ] （可选）P7-08~10 MQTT 上报 + 补传
- [ ] P7-11 云故障红线验证（🟡 断网演练/假结果开关已就绪）
- [ ] P7-12 LCD 云/WiFi 设置页 + 手动复检触摸验收

## 避坑

- DeepSeek 请求跑在 **Core1**（Core0 禁云请求）；图像 Base64 内存峰值注意释放（单帧 JPEG ~10KB，Base64 ~14KB，无压力，但别缓存多帧）。
- 5s 超时是"云请求自身"的；不要让 Core0 的 3s 识别超时被云请求拖死——两计时器独立。
- 云兜底结果同样要过白名单校验（来源不豁免）。
- 演示现场网络不稳：准备手机热点 + 断网演练脚本。
- **Qt 5.12 没有 `QNetworkRequest::setTransferTimeout`**（5.15+）——超时必须用 `QTimer` + `reply->abort()`，否则编不过。
- GUI 源码**纯 ASCII**（板规），中文一律 `"\x.."` 转义宏；`check_static.py` 会把非 ASCII 字节直接判 FAIL。
- **改 WiFi 是"拆自己脚下的梯子"**：设置页写 `/etc/wpa_supplicant.conf` 前先备份 `.bak`，并 20s 无 IP 自动回滚；测试时最好同时留着串口/网口。板端**没有 `pgrep/pkill/timeout`**，杀旧 `wpa_supplicant` 只能扫 `/proc`。
