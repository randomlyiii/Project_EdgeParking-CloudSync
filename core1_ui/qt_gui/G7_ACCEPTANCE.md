# park_ui 第7步（G7）验收手册 —— 云端兜底 + LCD 运维面

> 范围：`core1_ui/qt_gui`（Core1 = `park_ui` 进程）。**本步不碰 Core0 / M4 / C8T6**，所以验收只需要**重编一次 park_ui**，其他二进制不动。
> 依据：`PhaseMd/08_第7步_Linux与云端通讯.md` P7-01~P7-12；协议口径见 `docs/protocols.md` §5。
> 板规：**给板子粘贴的命令/脚本必须纯 ASCII**（下方命令块已遵守）；`/etc/park/cloud.conf` 里的 prompt 也是纯 ASCII。

---

## 0. 一句话判定

| 门 | 判定条件 |
|---|---|
| L1 静态 | `python3 tools/check_static.py` → `RESULT: PASS` |
| L2 起来 | park-ui 起来后 journal 有 `cloud: config ... (loaded)`，屏上底栏 `WIFI:` 与 `CLOUD:` 芯片有值（不是 `CLOUD:--`） |
| L3 功能 | 下表 G7-A ~ G7-I 全过（G7-G 需要能改 WiFi；没有则记"未测"） |
| 红线 | 断网/云 5xx/无 key 三种情况下，本地"车到位→识别→开闸/降级"闭环完全不受影响 |

---

## L1 宿主静态门禁（PC，无需 Qt 工具链）

```sh
# 仓库根目录
python3 core1_ui/qt_gui/tools/check_static.py
# 期望末行: RESULT: PASS
```

它断言的东西（改代码后必须仍然 PASS）：

1. 20 个源文件**纯 ASCII**（含 `settingspage.cpp` 里的中文一律写成 `"\x..."` 转义宏）；
2. 信号/槽签名一致（`recogResult`、`busyChanged`、`monitorSnapshot` 系列、`cloud*`、`wifi*`）；
3. `ipc_writer.cpp` **绝不写 Core0 归属字段**（magic/version/seq/free/used/gate/link/recog/fault/conf_threshold/`evt_bits_c0` 写位），`evt_bits_c0` 只做 AND-NOT 消费清除；
4. **第7步负向断言**：任何 `qWarning/qDebug/printf` 行里出现 `apiKey` → FAIL；GUI 源码里出现 `sk-xxxx` 字面量 → FAIL；
5. `core0_service/**` 里出现 `curl_easy/libcurl/<curl/QNetworkAccessManager/api.deepseek.com` → FAIL（**Core0 禁云请求**）；
6. 设置页只**只读展示** Core0 的 `conf_threshold`，不写它；
7. `wifi_manager.cpp` 有 20s 回滚（`restoreBackup` + `m_rollbackArmed`）；
8. `qt_gui.pro` 含 `QT += widgets network` 且 5 个新模块的 `.cpp/.h` 都在列表里。

---

## L2 板端起来（不需要 M4/C8T6，可离线做）

### 2.1 能力探针（**2026-09-11 已在板上实测，结论如下**）

```sh
ls -l /usr/lib/libQt5Network.so* /usr/lib/libssl.so* /etc/ssl/certs/ca-certificates.crt
which iw wpa_cli udhcpc wpa_supplicant python3 curl
cat /etc/wpa_supplicant.conf; ip -4 addr show wlan0
```

| 项 | 实测 | 结论 |
|---|---|---|
| `libQt5Network.so.5.12.8` | ✅ 在 | **Qt Network 方案确定可用**，不需要 libcurl |
| `libssl.so.1.1` / `libcrypto.so.1.1` | ✅ 在 | TLS 后端有库 |
| `/etc/ssl/certs/ca-certificates.crt` | ❌ **不存在** | **HTTPS 默认会握手失败** → 先做 2.2，或临时 `insecure_tls=1` |
| `curl` | ❌ 无 | 退化链里的 curl/libcurl 分支作废 |
| `iw` / `wpa_cli` / `udhcpc` / `wpa_supplicant` | ✅ 全在（**都在 `/sbin`**） | WiFi 功能全支持；代码已用**绝对路径**调用（不依赖 systemd 的 PATH） |
| `python3` | ✅ `/bin/python3` | 可用 |
| `/etc/wpa_supplicant.conf` | ✅ 厂商原件（`ctrl_interface`/`update_config`/`ap_scan=1` + 一个 network） | 程序写配置时**保留这三个全局项**，写前备份 `.bak` |

> ⚠️ 这台 rootfs 的工具都在 `/sbin`，而 systemd 交给服务的 PATH 不保证含 `/sbin` ⇒ `wifi_manager.cpp` / `settingspage.cpp` 里 `ip`/`wpa_cli`/`iw`/`udhcpc`/`wpa_supplicant`/`systemctl` **一律经 `toolPath()` 解析成绝对路径**。改代码别退回裸名字，否则服务里会静默 "command not found"。

### 2.2 装 CA（**不做这步，HTTPS 一定失败**）

```sh
# 从 book/PC 取一份 Mozilla CA bundle（Ubuntu 类系统自带）
scp book@<book-ip>:/etc/ssl/certs/ca-certificates.crt /tmp/ca.pem
scp /tmp/ca.pem root@<board>:/etc/park/ca.pem      # park_ui 自动优先使用

# 也可以在 cloud.conf 里指路：ca_file=/path/to/bundle.crt
# 纯演示、不校验：sed -i 's/^insecure_tls=.*/insecure_tls=1/' /etc/park/cloud.conf
```

`park_ui` 启动会打一行权威诊断（设置页「云端」底部同样显示）：

```
cloud: Qt TLS supportsSsl=yes (OpenSSL 1.1.x ...), CA=/etc/park/ca.pem
cloud: Qt TLS supportsSsl=yes (...), CA=NONE - install /etc/park/ca.pem or set insecure_tls=1
```

`supportsSsl=NO` 说明 Qt 编的时候没带 OpenSSL —— 那 HTTPS 无解，只能改用 `http://` 网关或另想传输层（本板实测有 `libssl.so.1.1`，预计是 `yes`）。

### 2.3 部署

```sh
# book（PC）交叉编译见 core1_ui/qt_gui/README.md §3
scp bin/park_ui root@<board>:/root/park_ui

# board
mkdir -p /opt/park_ui
cp /root/park_ui /opt/park_ui/park_ui
chmod +x /opt/park_ui/park_ui
install -d -m 700 /etc/park
install -m 600 /root/deploy/cloud.conf.example /etc/park/cloud.conf
vi /etc/park/cloud.conf      # 填 api_key=sk-...(或 export DEEPSEEK_API_KEY)
systemctl restart park-ui
```

`deploy/systemd/install_all.sh` 的 **[7/8]** 步会自动做上面 `install -d/-m`（**已存在则不覆盖**，key 不会丢）。

### 2.4 期望日志（journalctl -u park-ui -n 30 --no-pager）

```
cloud: config /etc/park/cloud.conf (loaded) api=https://api.deepseek.com/chat/completions model=deepseek-chat key=sk-abcd...wxyz timeout=5000ms retry=1
cloud: Qt TLS supportsSsl=yes (OpenSSL 1.1.x ...), CA=/etc/park/ca.pem
ipc-writer: /park_shm attached (v3)
wifi: wlan0 state=... (每 2s 静默轮询，不刷屏)
```

- `(defaults)` 而不是 `(loaded)` = 文件没读到（路径/权限/大小写），此时用的是内置默认值。
- `cloud: no API key - set it on the LCD settings page` = key 为空且没有 `$DEEPSEEK_API_KEY`。
- `CA=NONE ...` = **没装 CA，HTTPS 必失败**，回 §2.2。
- 屏幕上：底栏右侧 `WIFI:<ssid>`（绿）、`WIFI:online`（绿，租约刚到手、SSID 还没探到）或 `WIFI:down`（红）；`CLOUD:idle`（灰绿）/`CLOUD:no key`（橙）。
- **地址红线（2026-09-11 用户要求）**：公开 LCD 上出现本机 IP/网关 = 给路人递刀子。`WifiStatus` 已**彻底删掉 `ip`/`gw` 字段**（`parseRoute()` 一并删除），全链路只保留布尔 `hasIp`；底栏芯片与设置页「网络」页签**只显示 iface/state/ssid/signal**，journal 里也没有地址。改这条之前先想清楚：屏是挂在门口给人看的。

---

## L3 功能用例

> 所有"看滑窗"的东西都在底栏事件行轮播（4s 一轮），同时在 `journalctl -u park-ui -f` 里。
> 无线环境不方便时，可用 `PARK_CLOUD_CONF=/tmp/cloud.conf` 起一个临时实例做实验（不改 `/etc`）。

### G7-A 阈值读写与持久化

1. 齿轮 → 「云端」页签 → `TRIGGER` 右侧按 `-`/`+`（0.05 步进）把触发阈值改成 0.99；
2. 期望：滑块文本立即变 `0.99`，状态行 `saved to /etc/park/cloud.conf`；
3. `cat /etc/park/cloud.conf | grep trigger_conf` → `trigger_conf=0.99`；目录里出现 `cloud.conf.bak`；
4. `systemctl restart park-ui` → 重开设置页，值仍是 0.99（**持久化过**）。
5. 恢复成 0.60。

> 为什么用 0.99 做实验：K210 正常识别置信度 0.8~0.95 都 `< 0.99`，于是**每一次端侧结果都会触发云兜底**，不用特意造低置信样本也能测整条云链路。

### G7-B 测试连接

1. 设置页「云端」→ `TEST` 按钮；
2. 期望：状态行 `testing cloud ...` → 数秒内 CLOUD 芯片变绿 `CLOUD:ok`，统计行 `ok=1 ... avg=<ms>`；
3. `journalctl` 里出现 `cloud: test OK (http 200, <ms> ms, model=...)`（**不含 key**）；
4. 故意把 key 改错一位 → 期望芯片橙 + `CLOUD:<错误短串>`（`lastError` 前 16 字符，如 `http 401 unauthorized`），统计行 `fail`+1；改回正确 key。

### G7-C 手动云端复检（LCD 主动要云检）

前置：K210 有画面（`/dev/ttyACM0` 在、预览区有图）。

1. 底栏点「云端复检」（或设置页 `云端复检`）；
2. 期望：
   - `cloud: manual request -> https://... (model=..., <n>B)`（板端会打 `cloud: manual-settings`/`manual` 作为 reason）；
   - 有结果时屏上弹卡（车牌 + `CLOUD` 来源）；
   - 「结果回写」勾选时：`ipc-writer: cloud result '<plate>' conf=0.xx -> result_source=1, cloud_pending cleared`，并且 **Core0 日志出现对 `result_valid`/RESULT 事件的响应（`[slots]`/`[biz]` 片段）与闸动作**；
   - 未勾选「结果回写」时：只有 `cloud: '<plate>' conf=.. http=200 in 1234ms (no write-back)` + 弹卡，`/park_shm` 的 `result_valid` 不动。
3. 没有 K210 画面时：`cloud: no K210 frame yet, recheck skipped`（**预期行为，不算失败**）。

### G7-D 自动兜底（低置信 → 云 → 回注）

用 G7-A 的 0.99 阈值，或用 K210 对着模糊车牌：

```
# 观察 /park_shm（板上）
python3 -c "import mmap,os,struct,time
b=mmap.mmap(os.open('/dev/shm/park_shm',os.O_RDWR),76,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE)
for i in range(20):
    print('pending=%d valid=%d src=%d conf=%.2f plate=%s'%(b[23],b[24],b[25],struct.unpack_from('<f',b,28)[0],b[32:48].split(b'\x00')[0].decode('utf-8','replace')))
    time.sleep(1)"
```

期望时序：

1. K210 结果 < `conf_threshold` → `ipc-writer: low confidence 0.xx < 0.60 -> cloud pending`，`pending=1`、`valid=0`；
2. 屏上 CLOUD 芯片 `CLOUD:兜底中`（橙）；
3. 云答复 → `ipc-writer: cloud result ... result_source=1`，`pending=0`、`valid=1`、`src=1`；
4. **Core0 照样走白名单**：车牌在白名单 → 日志 `[gate] OPEN ... (cloud)` 或等价，闸动作；不在 → DENY 弹卡。
5. 云失败/无 key → `cloud_pending` 归零（`pending=0`），屏上回到 `CLOUD:<err>`，**不允许卡在"兜底中"**（这是本步最关键的收敛性断言）。

### G7-E 模型名与 API 地址切换

1. 设置页 `MODEL` → `PRESET` 在 deepseek-chat / deepseek-reasoner / gpt-4o-mini / qwen-vl-max 间轮换，或点 `EDIT` 用软键盘手敲；
2. 故意填一个不存在的模型 → `TEST` → 期望 `CLOUD:http 404`（提示模型名），`fail`+1；
3. `API` 地址同理：填成 `https://example.com/v1/chat/completions` → 期望非 2xx/解析失败被容错，UI 不崩、`cloud_pending` 归零；
4. 改回默认并 `SAVE`。
5. **PRESET 会同时切「模型 + 端点 + 该 provider 的 key」**（2026-09-11 板验后加）：状态行显示 `preset -> qwen-vl-max @ dashscope.aliyuncs.com key=sk-abcd...wxyz`；若这家还没存过 key，显示 `key=NONE for this provider`，此时按 `KEY/EDIT` 输入一次即可（之后切回来会自动带出）。`/etc/park/cloud.conf` 里能看到 `api_key=`（当前生效）+ `key_deepseek=`/`key_dashscope=`/`key_openai=` 各家一把。

### G7-F 软键盘

1. 任意 `EDIT`（API 地址/模型/key/SSID/密码）弹出全屏键盘：QWERTY + 数字 + `. - _ : @ / +`；
2. `SHIFT` 大小写、`DEL` 退格、`CLEAR` 清空、`CANCEL` 不改、`OK` 提交；
3. 密码类输入（API key、WiFi 密码）默认掩码，`SHOW` 可切换明文；
4. 断网也无妨（纯本地控件）。

### G7-G WiFi 检测与改名（⚠️ 会动网络，风险最高）

**先准备好退路**：串口控制台或网口，或者保证 20s 回滚可用。

1. **连接状态**：设置页「网络」页签 → `REFRESH` → 期望显示 `wlan0: <state>  ssid=<ssid>  signal=<0..100>`（**故意不含 IP/网关**），连通性结论与 `ip -4 addr show wlan0`、`iw dev wlan0 link` 一致；底栏 WIFI 芯片同步（2s 内）；
2. **扫描**：`SCAN` → 列表出现周围 SSID（无 `iw` 时退 `wpa_cli scan` + 3s + `scan_results`）；
3. **故意改错密码**：
   - 输入正确 SSID + 错误密码 → `CONNECT`；
   - 期望：状态行 `wifi: apply (connect <ssid>)`，屏上 WIFI 芯片变红 `WIFI:down`；
   - **20s 内拿不到 IPv4 → 自动回滚**：日志 `wifi: no lease in 20s (<watchdog|apply>), restoring the previous config` + 状态行 `no ip lease - previous wifi config restored`，`/etc/wpa_supplicant.conf` 恢复成原内容（对比 `.bak`）并自动重新应用；原 WiFi 恢复、SSH 不断；
   - 若 20s 内 SSH 断了又回来，也算通过（回滚生效）。
4. **正确密码**：`CONNECT` → 期望状态行 `connected to <ssid>`（**不回显地址**），WIFI 芯片绿 `WIFI:<ssid>`，`abortRollback` 生效（日志不再出现 rollback）；
5. **恢复备份**：`RESTORE` 手动还原 `.bak`。
6. 无 WiFi 环境时记"未测"，但要确认 `WIFI:down` 芯片正确显示（这是需求 4 的可观测部分）。

### G7-H 云故障不影响本地（红线，P7-11）

| 注入 | 操作 | 期望 |
|---|---|---|
| 断外网 | 设置页勾「断网演练」（`outage=1`） | 每个云请求约 1.2s 本地失败、**不出网卡**；`CLOUD:outage` 橙；`cloud_pending` 归零；本地"车到位→识别→降级"闭环照常 |
| 假结果 | 勾「假结果」（`fake_result=1`） | 离线返回 `TEST001 / 0.98`，可被白名单接受 → 开闸（无网演示路径），日志标明是 fake |
| 无 key | `api_key=` 置空且不设 `$DEEPSEEK_API_KEY`，重启 UI | `cloud: no API key ...`，请求**不发**，`cloud_pending` 立即归零，本地闭环照常 |
| 5xx/429 | 填一个会返回错误的地址或用代理 | `CLOUD:http 5xx`，`fail`+1，不重试 4xx/5xx |

红线判据：上述四种情况下，**K210 高置信结果照常写回并开闸**（`result_source=0`），底栏与事件行不卡死，`hb_core1` 继续 +1（屏上 `CORE1:在线`）。

### G7-I Core0 不涉云（静态已保证，板端佐证）

```sh
strings /opt/core0/core0_business | grep -Ei 'curl|deepseek|http://|https://' || echo "core0: no cloud strings (expected)"
journalctl -u core0-bus -n 200 --no-pager | grep -i cloud || echo "core0: no cloud log lines (expected)"
```

`cloud_pending` 只是 Core0 读的**共享内存标志位**，Core0 自己不发任何请求。

---

## 4. 证据与记录（提交/答辩留档）

每项留三样：**命令、原始输出（journal 片段）、判定**。建议保存：

```sh
journalctl -u park-ui --since "10 min ago" --no-pager > /tmp/g7_parkui.log
journalctl -u core0-bus --since "10 min ago" --no-pager > /tmp/g7_core0.log
cp /etc/park/cloud.conf /tmp/g7_cloud.conf.copy    # 记得把 key 打码后再外发
python3 core1_ui/qt_gui/tools/check_static.py > /tmp/g7_static.log
```

KPI 量测口径（写进报告）：

| 指标 | 口径 | 期望 |
|---|---|---|
| 云请求超时 | 单次请求墙钟 | ≤ 5s（硬超时 abort） |
| 云兜底总时延 | 低置信判定 → shm `result_source=1` | 单次成功 ≈ 1~3s；失败 ≈ 5s（+1 次重试 ≤10s），且 Core0 6s 硬上限内必收敛 |
| UI 反映时延 | shm 变化 → 屏上芯片/弹卡 | ≤ 500ms（200ms 轮询） |
| 本地闭环 | 三种云故障下开闸 | 不受影响 |

## 5. 常见判读

| 现象 | 原因/处理 |
|---|---|
| 屏上**预览占满全屏、右栏车牌被挤出屏外**（2026-09-11 真机遇到，已修） | linuxfb 无 WM：某个 QLabel 文本太长时，`QLayout` 会把窗口的 `minimumSize` 顶大 → 窗口被撑到 >1024，多出来的部分在屏外（没有 WM 能滚回来）。修法 = ① 窗口显式 `setMinimumSize(320,200)`（显式最小值优先于布局）；② 状态栏芯片/事件行/预览 `QSizePolicy::Ignored` + `minimumWidth(0)`（文本裁切而不是顶布局）；③ 芯片与事件行按像素/字数 elide。加新控件到状态栏/底栏时**别删这些策略**（静态门禁有断言）。 |
| `WIFI` 芯片能显示 SSID，但设置页 signal 一直是 `-1` | 本板内核**没有 `/proc/net/wireless`**（无 WEXT）→ 已改为异步 `wpa_cli signal_poll` 取 RSSI 折算百分比 |
| `CLOUD:--` 一直不变 | 云客户端没接上（`statsChanged` 未触发）→ 先按一次「测试连接」；再看 journal 是否 `cloud: config ...` |
| `CLOUD:no key` | `/etc/park/cloud.conf` 的 `api_key=` 空且无 `$DEEPSEEK_API_KEY`；设置页输入或 `vi` 填 |
| **测试连接/复检永远 5s 超时，但 python 同一条请求拿到 HTTP 200**（2026-09-11 真机定案） | **本板 Qt 5.12.8 + OpenSSL 1.1.1 在 TLS 1.3 协商上卡死**（python 0.2s 握手成功、Qt 烧满超时；网络/时钟/CA/key 都已逐项排除）。已内置 **`transport=auto`**：Qt 失败即自动改用 **python3 子进程**发请求，并在本进程内保持 python 通道 ⇒ 第一次点击约 6s（5s 探测 + 1s 成功），之后每次约 1s。想连那 5s 都省掉就写 `transport=python`。日志特征：`cloud: qt transport failed (timeout) - switching to python3` → `cloud: python transport ok in N ms (http 200)` |
| 云端一直 failed 且日志是 `tls handshake failed` | 看时间（`date -u` 必须是真实年份，本板无 RTC → `deploy/systemd/park-clock.service`）与 `CA=` 是否为 `NONE` |
| 设置页改了不生效/重启丢失 | `/etc/park` 不可写（权限/只读根文件系统）→ 看状态行是否 `SAVE FAILED`；`PARK_CLOUD_CONF` 指到可写路径 |
| `cloud: no K210 frame yet` | `/dev/ttyACM0` 没帧 → 查 K210 固件/`board-power.service`（USB 供电，见 AGENTS 记忆） |
| `CLOUD:兜底中` 卡住 | 不该发生：任何出口都会清 `cloud_pending`。若卡住 → 看 journal 是否有 `cloud: FAILED/unreadable`，以及 `IpcWriter::clearCloudPending` 是否被 `attachShm` 失败挡掉 |
| 改了 WiFi 后 SSH 断且不回来 | 回滚应在 20s 内发生；若没有 → 串口进去 `cp /etc/wpa_supplicant.conf.bak /etc/wpa_supplicant.conf` 再跑设置页 `RESTORE` |
| 芯片一直 `WIFI:down`，但串口里 `ip -4 addr show wlan0` 有地址 | `wlan0` 名字不同 → 用 `PARK_UI_WIFI=wlan1` 之类覆盖（`WifiManager::setInterface`）或改代码默认值 |
| 启动报 `QNetworkAccessManager` 相关未定义 | `.pro` 少了 `QT += network`，或 `make` 前没 `qmake` 重生成 Makefile |
| 屏上中文变豆腐块 | 字体问题（非本步），见 README §3 板上运行 |

## 6. 与后续步骤的接口

- 结果回注统一走 `IpcWriter::onCloudResult()`（`result_source=1` + `evt RESULT`），Core0 逻辑不区分来源 ⇒ 第8步全业务联跑时云结果与端侧结果同路径。
- MQTT 上报（P7-08~10，可选）应挂在 Core1（`park_ui`）而非 Core0（Core0 禁网络外呼的同类约束），复用 `WifiManager` 的网络就绪状态。
