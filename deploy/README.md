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
| `cloud.conf.example` | 第7步云端配置模板（拷到 `/etc/park/cloud.conf`，模式 600；**含 API key，永不入 git**） |

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
#   scp core0_business core0.conf.example root@<board>:/root/core0_service/
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
  的 `[5/7]`+`[6/7]` 两步）。手动验证：`for g in 82 139; do echo $g > /sys/class/gpio/export;
  echo out > /sys/class/gpio/gpio$g/direction; echo 1 > /sys/class/gpio/gpio$g/value; done`，
  然后 `lsusb` 应出现 `0424:2514`(HUB) + `1a86:55d4`(K210 CH9102 串口)。
  另外：**本板没有 `pgrep`/`pkill`**，unit 里杀 mxapp2 已改为 `/proc` 扫描。
- **K210 预览链路**：设备节点 `/dev/ttyACM0`（CH9102 走 cdc_acm；可用 `/etc/park-ui.env` 的
  `PARK_UI_TTY=` 覆盖）。K210 端 `main.py` 需在跑（`LINK="console"`、`CONSOLE_PREVIEW=1`），
  且**必须用 CanMV IDE「保存到设备」**（点"运行"不写盘）。UI 方向已定案为「不做任何翻转」
  （固件 `CAM_SW_HMIRROR=True` 一处修正同时修好两个屏）。
- **两个进程抢 `/dev/ttyRPMSG0`**：同一时刻只能有一个读者（core0_business 或
  rpmsg_demo 或 rpmsg_link_test.py）。跑检测脚本前先 `systemctl stop core0-bus`。
