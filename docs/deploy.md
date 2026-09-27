# deploy —— 板端部署层（systemd 全链路起动 + 运行环境）

> 对端参考：PhaseMd/05（Core0 业务守护 G4）、PhaseMd/04（M4 RPMSG）、core1_ui/qt_gui（park_ui）、k210_fw（K210 端 main.py）。
> 板端规范：脚本纯 ASCII；root（本板无 sudo）。

## 全链路启动顺序

```
m4-load.service      remoteproc start：加载 M4 固件
                     (1) ensure_can_clock  → m_can 保持绑定 + can0 up@500k
                         （否则内核把 fdcan_k 时钟关掉 → M4 的 HAL_FDCAN_Init 卡死）
                     (2) echo start > /sys/class/remoteproc/remoteproc0/state
                     (3) driver_override + bind rpmsg_tty → /dev/ttyRPMSG0
        ↓
core0-bus.service    core0_business：/park_shm(v3) 宿主 + RPMSG(0x11/0x12/0x13/0x21/0x22/0x23/0x7E)
                     + 7 态业务状态机 + 配置热加载
        ↓
park-ui.service      park_ui (Core1)：linuxfb 全屏 UI，读 /park_shm 与 /dev/ttyACM0(K210)
```

## 结构

| 文件 | 用途 |
|---|---|
| `systemd/m4-load.service` | M4 固件加载（oneshot，幂等；`SuccessExitStatus=0 1 2` 容忍 flaky 首次失败） |
| `systemd/core0-bus.service` | core0 业务守护（`Restart=always`；缺 `/dev/ttyRPMSG0` 也能起，链路 DOWN 自恢复） |
| `systemd/park-ui.service` | park_ui 开机自启（linuxfb fb0、`Restart=always`、可配 `/etc/park-ui.env`） |
| `systemd/install_all.sh` | **全链路安装**：拷二进制/配置/工具 + 装并 enable 三个 unit + 禁用厂商 HMI |
| `systemd/install_park_ui.sh` | UI-only 安装（旧命令兼容，内部调 `install_all.sh` 且关掉 M4/core0） |
| `systemd/board-power.service` | USB Host 供电（GPIO 82/139 拉高；禁用 myir 后必须补） |
| `systemd/wifi-up.service` + `wifi_up.sh` | **开机把 wlan0 拉起来**（厂商三步：`ip link set up` → `wpa_supplicant -B -D nl80211` → `udhcpc`，3 轮 DHCP） |
| `systemd/park-clock.service` + `set_clock.py` | 无 RTC 板子用明文 HTTP `Date:` 校时（否则 HTTPS 证书"尚未生效"） |
| `systemd/park-clock.timer` | 校时的**第二次机会**：开机 3 分钟后 + 每 15 分钟再校一次（开机那次因 DHCP/DNS 未就绪而失败时兜底） |
| `sample_cloud.conf` | 第7步云端配置模板（拷到 `/etc/park/cloud.conf`，模式 600；**真 key 永不入 git**） |
| `sample_wpa_supplicant.conf` | WiFi 配置模板（拷到 `/etc/wpa_supplicant.conf`，模式 600；**真 PSK 永不入 git**） |
| `sample_park-ui.env` | UI 运行时环境模板（拷到 `/etc/park-ui.env`，全部注释掉 = 不改任何行为） |

安装布局：

```
/opt/core0/core0_business            业务守护
/opt/core0/core0.conf                本地配置（白名单/车位/超时/阈值）＝唯一事实源，热加载
/opt/core0/tools/load_m4.sh          M4 加载脚本（m4-load.service 调用）
/opt/core0/tools/rpmsg_link_test.py  RPMSG 链路检测（python3，零编译）
/opt/park_ui/park_ui                 Qt UI
/lib/firmware/m4_fw.elf              M4 固件
/etc/park/cloud.conf                 云端兜底配置（API key/阈值/模型，600；不在仓库内）
```

## 安装（板上 root）

```sh
# 前置：先在 book 交叉编译出 ARM 二进制（板子无 gcc）
#   cd ~/core0_service && make CROSS_COMPILE=arm-buildroot-linux-gnueabihf- core0_business
#   （park_ui 见 core1_ui/qt_gui/README.md）

# PC/Windows → 板：deploy + 二进制 + 固件
#   scp -r deploy root@<board>:/root/
#   scp core0_business sample_core0.conf root@<board>:/root/core0_service/
#   scp bin/park_ui root@<board>:/root/park_ui
#   scp m4_fw/CM4/Debug/m4_fw_CM4.elf root@<board>:/lib/firmware/m4_fw.elf

sh /root/deploy/systemd/install_all.sh          # 全链路
sh /root/deploy/systemd/install_park_ui.sh      # 只要 UI（兼容旧命令）

systemctl start m4-load core0-bus park-ui
systemctl status m4-load core0-bus park-ui --no-pager
```

启动后自检：

```sh
ls -l /dev/ttyRPMSG0 /dev/shm/park_shm          # 通道 76 字节
python3 /opt/core0/tools/rpmsg_link_test.py     # RESULT: LINK OK
journalctl -u m4-load -u core0-bus -u park-ui -n 40 --no-pager
```

## 可选开关（env）

```sh
INSTALL_M4=0      sh install_all.sh     # 不装 M4 unit
INSTALL_CORE0=0   sh install_all.sh     # 不装 core0 unit
INSTALL_UI=0      sh install_all.sh     # 不装 UI
CORE0_SRC=/path/core0_business  UI_SRC=/path/park_ui  M4_FW=/path/m4_fw.elf
```

`install_all.sh` **不会覆盖**已存在的 `/opt/core0/core0.conf`（配置文件是操作员的事实源），
同样**不会覆盖** `/etc/park/cloud.conf`（里面有 API key）；只在文件不存在时放一份模板。

## 开机自动联网（`wifi-up.service`，2026-09-11 修）

**需求**：每次开机自动执行厂商那三条 —— `ip link set wlan0 up` / `wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf` / `udhcpc -i wlan0`。

**为什么以前不自动**：`park_ui` 只在操作员按 LCD 设置页的 `CONNECT` 时才拉起 wlan0；而原本在开机时联网的厂商桌面服务（`myir.service`）被我们**故意禁用**了 ⇒ 之后没人干这件事 ⇒ 每次上电都得手敲三条，**而且链路没起来时云检测失败，屏幕上看着像"云端/传输坏了"**。

**现在**：`install_all.sh` 的 `[4/9]` 步装 `/opt/park_ui/tools/wifi_up.sh` + enable `wifi-up.service`（oneshot）。启动链：

```
board-power -> wifi-up -> park-clock -> m4-load -> core0-bus -> park-ui
```

手工操作与验证：

```sh
systemctl status wifi-up --no-pager
journalctl -u wifi-up -n 20 --no-pager     # 期望: wifi-up: wlan0 online (ssid=<你的SSID>)
systemctl restart wifi-up                  # 改完 /etc/wpa_supplicant.conf 后重跑
cat /etc/park-ui.env                       # 可选: PARK_UI_WIFI=wlan0 / PARK_WPA_CONF=...
```

脚本行为要点：wlan0 先 `up` → 用 `/proc` 扫描杀掉旧 `wpa_supplicant`（本板无 pgrep/pkill）→ 起新的 → `udhcpc -i wlan0 -n -q -t 5 -T 3` **最多 3 轮** → 只把「有没有租约 / SSID」写进 journal（**不打印密码、不打印 IP**）→ 无论如何 `exit 0`（没网也必须让本地业务起来）。设置页「诊断」页签第 2 项就是 `wifi-up`。

**LCD 上也能量手动拉起**（2026-09-11 加）：设置页「网络」页签的 **`联网`** 按钮 = 用**当前** `/etc/wpa_supplicant.conf` 跑同样三步（`ip link set wlan0 up` → `wpa_supplicant -B -D nl80211` → `udhcpc`），**不写配置文件、不触发 20s 回滚**（配置没变，没什么可回滚的），最多 20s 后报「有没有租约」。用途：板子是装 `wifi-up.service` 之前刷的、或开机那次同步失败、或手工改了 `/etc/wpa_supplicant.conf` 想立刻生效——都不必再敲命令。

**网络页签三个动作的分工**（2026-09-11 明确）：

| 按钮 | 写 `/etc/wpa_supplicant.conf` | 动链路 | 会回滚吗 |
|---|---|---|---|
| `保存` | ✅ 写成厂商布局（`ctrl_interface`/`update_config=1`/`ap_scan=1` + `network{ssid,psk,key_mgmt}`） | ❌ | ❌（`.bak` 只在首次保存时生成，不会覆盖厂商原件） |
| `联网` | ❌ | ✅ 厂商三步 | ❌（没改文件没得回滚） |
| `连接` | ✅ | ✅ 写后再拉起 | ✅ 20s 无租约 → 恢复 `.bak` 并重放旧配置 |

所以"只想改文件里那个 SSID/密码"就用 **`保存`**——以前只有 `连接` 一条路，改错了密码 20s 后文件会被回滚，看着像"没保存成功"。

## 时钟 = 云端的隐形前置（2026-09-11 真机事故）

**症状**：面板/日志出现 `cloud: FAILED ... [SSL: CERTIFICATE_VERIFY_FAILED] certificate verify failed: certificate is not yet valid`——看着像云坏了，其实**网络与 CA 都正常**，只是板子时间还停在 **2020-02-07**（无 RTC，出厂时间）。

**一分钟判定**（见 `[7f]` 判读顺序的第一步）：

```sh
date -u          # 不是当前年份 -> 就是这个原因
```

**两步修**（**也可以全在 LCD 上做**：齿轮 → 「诊断」页签有一行 UTC 时间 + `同步` / `改时间` 两个按钮）：

- `同步` = 跑同一个 `set_clock.py`（HTTP `Date:` 头，带重试）；
- `改时间` = 软键盘输入 `YYYY-MM-DD HH:MM:SS`（UTC），严格校验后直接以 argv 交给 `date -u -s`（不经 shell）；
- 年份 < 2025 时那一行会追加 `NOT SET: cloud TLS will fail (certificate is not yet valid)`，一眼可见。

```sh
# 1) 立刻生效（不用重编、不用重启）
date -u -s "YYYY-MM-DD HH:MM:SS"        # 填当前 UTC 时间
date -u

# 2) 持久（重启后自动恢复）
sh /root/deploy/systemd/install_all.sh   # 装 park-clock.service + park-clock.timer
systemctl status park-clock.timer --no-pager
journalctl -u park-clock -n 10 --no-pager   # 期望: set_clock: clock set to ... UTC
```

**本次加固**（原来校时失败是"静默"的，板子就留在 2020）：

- `set_clock.py` 新增 `--retries/--delay`（unit 里用 `--retries 8 --delay 10`，覆盖开机时 DHCP/DNS 未就绪的窗口），成功/失败都**无条件**写 journal：`set_clock: clock set to ... UTC` 或 `set_clock: FAILED after N attempt(s) ... date -u -s "..."`；
- `park-clock.service` 的 `TimeoutStartSec` 40 → **180**（给重试留时间）；
- 新增 **`park-clock.timer`**（`OnBootSec=3min`、`OnUnitActiveSec=15min`）：开机那次没成功，3 分钟后再校一次，之后每 15 分钟一次；
- 云端失败分类新增 `certificate verify failed` 桶，事件行直接提示查 `date -u` 与 `/etc/park/ca.pem`。

## 样例文件与隐私（`sample_` 约定，2026-09-11 定）

**规则**：仓库里只放 **`sample_<原名>`** 模板（占位值、可公开）；模板拷过去生成的**真文件一律不入库**
（`.gitignore` 已覆盖、且**不允许**再出现第二套命名如 `*.example`）。

| 真文件（板端/本地，gitignored） | 模板（入库） | 里面有什么 |
|---|---|---|
| `/etc/park/cloud.conf` | `deploy/sample_cloud.conf` | API key（`api_key=` + `key_<provider>=`） |
| `/etc/wpa_supplicant.conf` | `deploy/sample_wpa_supplicant.conf` | **WiFi SSID + 密码（PSK）** |
| `/etc/park-ui.env` | `deploy/sample_park-ui.env` | `PARK_UI_TTY` / `PARK_UI_K210_FLIP` / `PARK_UI_WIFI` / `PARK_WPA_CONF` / `PARK_CLOUD_CONF` |
| `/opt/core0/core0.conf` | `core0_service/sample_core0.conf` | 车位/超时/阈值（无机密，但同样不入库） |
| `云端API调用测试参考/key.txt` | 同目录 `sample_key.txt` | API key（工具用） |
| `core1_ui/ca.pem` | 无（公开 CA 包） | 仅本地副本，不进库 |
| `code.txt` | 无（草稿） | 板端粘贴命令 + 板子 IP |

**门禁**：`python3 core1_ui/qt_gui/tools/check_static.py` 会检查
① 每个 sample 都存在且**纯 ASCII**；② 里面没有 `sk-` 真 key / 非占位 `psk="..."` / `192.168.*` 字面量；
③ `sample_park-ui.env` 里写的每个 `PARK_*` 变量**源码里真的读**（防止文档写了、代码没接）；
④ `.gitignore` 覆盖了所有真文件名。改样例后必跑，**故意填真值会 FAIL**。

## 第7步云端兜底（`/etc/park/cloud.conf`）

**板端能力实测（2026-09-11，`root@100ask` 现场探针）**：

| 项 | 结果 | 影响 |
|---|---|---|
| `libQt5Network.so.5.12.8` | ✅ 在 | Qt Network 方案可用，**不需要 libcurl** |
| `libssl.so.1.1` / `libcrypto.so.1.1` | ✅ 在 | Qt 的 TLS 后端有库 |
| `/etc/ssl/certs/ca-certificates.crt` | ❌ **不存在** | **HTTPS 默认握手失败** → 必须装 CA 或临时开 `insecure_tls` |
| `curl` | ❌ 无 | 原"退化到 curl/libcurl"的路子作废（python3 在 `/bin/python3`） |
| `iw` / `wpa_cli` / `udhcpc` / `wpa_supplicant` | ✅ 在（**都在 `/sbin`**） | WiFi 检测/改配置全支持；代码已改成**绝对路径调用**（systemd 单元的 PATH 不保证含 `/sbin`） |
| `/etc/wpa_supplicant.conf` | ✅ 厂商原文件（`ctrl_interface`/`update_config`/`ap_scan=1` + 一个 network） | 程序写配置时保留这三个全局项，且写前备份 `.bak` |

### 先解决 CA（不然后面全是 TLS 报错）

```sh
# 从 book / PC 拷一份 Mozilla CA bundle（Ubuntu 类系统自带）
scp book@<book-ip>:/etc/ssl/certs/ca-certificates.crt /tmp/ca.pem
scp /tmp/ca.pem root@<board>:/etc/park/ca.pem     # park_ui 自动优先使用

# 或者临时演示（不校验证书，仅 Demo 用）
sed -i 's/^insecure_tls=.*/insecure_tls=1/' /etc/park/cloud.conf
```

启动日志会直接说明现状（`supportsSsl` / SSL 库版本 / 实际 CA 路径），设置页「云端」页签底部也有一行 `tls=...`。

```sh
# 1) 装完模板后填 key（或直接 export DEEPSEEK_API_KEY=/etc/park-ui.env）
vi /etc/park/cloud.conf          # api_key=sk-...
chmod 600 /etc/park/cloud.conf

# 2) 重启 UI 让它读新配置（配置在启动时读入，也可在 LCD 设置页里改）
systemctl restart park-ui
journalctl -u park-ui -n 20 --no-pager | grep -E 'cloud|TLS'
#   cloud: config /etc/park/cloud.conf (loaded) api=... key=sk-abcd...wxyz timeout=5000ms retry=1
#   cloud: Qt TLS supportsSsl=yes (OpenSSL 1.1.x ...), CA=/etc/park/ca.pem
#   cloud: Qt TLS supportsSsl=yes (...), CA=NONE - install /etc/park/ca.pem or set insecure_tls=1
#   cloud: no API key - set it on the LCD settings page      <- key 没读到
```

- **LCD 内全部可改**：底栏齿轮 → 设置页（云端/网络/诊断三页签）。置信度阈值、API 地址、模型名、key（软键盘输入）、测试连接、云端复检都在里面；底栏「云端复检」按钮 = 用最近一帧 K210 画面手动问一次云。
- **网卡只有 WiFi**：设置页「网络」页签可改 SSID/密码，写 `/etc/wpa_supplicant.conf` 前自动备份 `.bak`，**20s 没拿到 IP 自动回滚**（改 WiFi 等于拆自己的 SSH 梯子，建议同时留串口）。
- **断网演练**：设置页勾「断网演练」后每个云请求在板上本地失败（1.2s），用来演示"云挂了本地照常"；勾「假结果」则离线返回 `TEST001/0.98`（白名单可开闸）用于无网演示。

## 排障

- **云检测失败，屏幕/日志出现 `python transport` 或 `cloud: FAILED`（2026-09-11）**：
  先看链路，不要先怀疑云。新版失败行会带上链路状态：
  `cloud: FAILED no network (URLError: ... name resolution) [wifi:lease=no ssid=]`
  —— `lease=no` 就是**没有网络**（多半是开机没联网）。修法：
  ```sh
  systemctl restart wifi-up          # 或手敲厂商三条命令
  journalctl -u wifi-up -n 20 --no-pager
  ```
  若 `lease=yes` 仍然失败，再按 `journalctl -u park-ui | grep cloud` 的分类原因查
  （`401 unauthorized` = key/厂商不匹配；`timeout`/`tls` = 时钟或 CA，见 §第7步）。
  旧版把"没网"错报成 `python transport failed`，这就是那次误判的来源。
- **M4 起不来**（已知 flaky，实测成功率先低后高）：`systemctl restart m4-load`，
  两次之间**等 ≥2 分钟**；或 `sh /opt/core0/tools/load_m4.sh stop` → 等待 → `start`。
  失败时 core0 仍会正常跑（`[rpmsg] link DOWN`），业务面板照常，只是不能开闸。
- **LCD 停在厂商「默认页面」**：屏霸是 `myir.service`（→ `mxapp2` eglfs）。
  `install_all.sh` 会 `systemctl disable --now myir.service`，`park-ui.service` 的
  `ExecStartPre` 还会 `pkill -9 -f mxapp2` 兜底。若仍被盖住：
  ```sh
  ps -ef | grep -iE 'qt|demo|eglfs|weston|mxapp'
  systemctl disable --now myir.service && systemctl restart park-ui
  ```
- **K210 只有 UI 没有画面 / 任何 USB 设备都不认（2026-09-11 定案）**：
  `lsusb` 里只有三个 `1d6b:` root hub、`/dev/ttyACM*` 不存在 ⇒ **板载 USB HUB 没上电**。
  根因：`/usr/bin/start.sh`（`myir.service` 跑的）除了启 mxapp2，还会把 **GPIO 82(PF2) 与
  139(PI11) 拉高**给 USB Host 供电/HUB 使能；我们 `disable --now myir.service` 后这两个脚
  就没人管了（GPIO 输出是保持态，所以"重启前一直好好的、重启后突然不行"）。
  → `board-power.service` 就是复刻这两条写操作，**它必须和 myir 的禁用一起装**（`install_all.sh`
  的 `[6/9]`+`[7/9]` 两步）。手动验证：`for g in 82 139; do echo $g > /sys/class/gpio/export;
  echo out > /sys/class/gpio/gpio$g/direction; echo 1 > /sys/class/gpio/gpio$g/value; done`，
  然后 `lsusb` 应出现 `0424:2514`(HUB) + `1a86:55d4`(K210 CH9102 串口)。
  另外：**本板没有 `pgrep`/`pkill`**，unit 里杀 mxapp2 已改为 `/proc` 扫描。
- **K210 预览链路**：设备节点 `/dev/ttyACM0`（CH9102 走 cdc_acm；可用 `/etc/park-ui.env` 的
  `PARK_UI_TTY=` 覆盖）。K210 端 `main.py` 需在跑（`LINK="console"`、`CONSOLE_PREVIEW=1`），
  且**必须用 CanMV IDE「保存到设备」**（点"运行"不写盘）。UI 方向已定案为「不做任何翻转」
  （固件 `CAM_SW_HMIRROR=True` 一处修正同时修好两个屏）。
- **两个进程抢 `/dev/ttyRPMSG0`**：同一时刻只能有一个读者（core0_business 或
  rpmsg_demo 或 rpmsg_link_test.py）。跑检测脚本前先 `systemctl stop core0-bus`。
