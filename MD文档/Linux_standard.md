# Linux 端（A7）工程规范 — STM32MP157 / 100ASK-MP157（5.4.31 BSP）

> 适用范围：本项目 **A7 侧 Linux**（core0_service 业务守护、core1_ui Qt 应用、deploy/systemd、
> 板端脚本与排障）的开发、编译、部署与运行。
> 最后更新：2026-09-11（创建；内容全部来自**本板真机实测**，见每条"证据"列）
> 关联文档：`MD文档/a7-m4_standard.md`（A7↔M4 RPMSG 链路）、`docs/protocols.md`（协议权威定义）、
> `PhaseMd/`（分步任务）、`deploy/README.md`（部署脚本用法）
>
> **编写纪律（本文件的核心约束）**：只写**在本板真机复现过**的事实与配方，每条给出证据命令/输出。
> **凡是没实测、或只在文档/推测里出现过的，一律不写**——宁缺勿错。未确认项集中列在 §8，供后续补测。

---

## 1. 目标平台与已确认环境（逐条实测）

| 项 | 实测结论 | 证据（板端执行） |
|---|---|---|
| 内核 | **5.4.31**（100ASK BSP，非 6.6） | 读 `/proc/config.gz` 与 BSP 配置核对，见 `a7-m4_standard.md` §3.2 |
| 编译器 | **板上没有 gcc/make**；任何 C 代码必须交叉编译后拷入 | `which gcc make` 无输出 |
| python3 | `/bin/python3` 可用（stdlib 齐全；板端临时验收首选） | `python3 -V` |
| curl | **没有** | `which curl` 无输出 |
| CA 证书库 | **没有** `/etc/ssl/certs`（但有 `libssl.so.1.1`/`libcrypto.so.1.1`）⇒ HTTPS 必须自备 CA | `ls /etc/ssl/certs` 报 No such file |
| 网络/无线工具 | `ip`/`iw`/`wpa_cli`/`wpa_supplicant`/`udhcpc` **全在 `/sbin`** | `ls /sbin/wpa_supplicant /sbin/udhcpc` |
| procps 工具 | **无 `pgrep`/`pkill`/`timeout`** ⇒ 杀进程/限时必须自实现 | `which pgrep pkill timeout` 无输出 |
| `/proc/net/wireless` | **不存在** ⇒ 取信号强度只能 `wpa_cli signal_poll` | `cat /proc/net/wireless` 报 No such file |
| RTC | **无可用 RTC**：开机时钟停在 2020 ⇒ 未校时前 TLS 必失败 | 开机 `date -u` → `Fri Feb 7 16:23:47 UTC 2020` |
| root | **无 sudo**，直接以 root 运行；`/tmp` 重启即清 | `id`；重启后 `ls /tmp` |
| Qt 运行库 | **Qt 5.12.8**；插件目录 `/usr/lib/qt/plugins/platforms`（含 `libqlinuxfb.so`） | `ls /usr/lib/libQt5Core.so.5.12.8` |
| 显示 | `/dev/fb0` 驱动 `stmdrmfb`，**1024x600 / 16bpp**；环境 `QT_QPA_PLATFORM='linuxfb:fb=/dev/fb0'` | `cat /sys/class/graphics/fb0/{name,virtual_size,bits_per_pixel}` |
| 共享内存 | `/park_shm` 的文件系统路径是 **`/dev/shm/park_shm`**（不是 `/park_shm`） | `ls -l /dev/shm/park_shm` |
| USB 串口 | K210 那路 = **CH9102 走 `cdc_acm` → `/dev/ttyACM0`**（`c 166,0`），**不需要 ch341** | `lsusb` + `ls -l /dev/ttyACM0` |
| USB Host 供电 | 由 **GPIO 82(PF2) / 139(PI11)** 经 sysfs 拉高控制（consumer 名为 `sysfs`，厂家脚本手工 export） | `cat /sys/kernel/debug/gpio`；`echo 1 > .../value` 后 `lsusb` 多出 `0424:2514`/`1a86:55d4` |
| 手动跑 Qt | 必须 `< /dev/null` 重定向 stdin，否则后台进程读终端被 **SIGTTIN 停住**（表现为 `kill` 报 Exit 1，不是崩溃） | 不带重定向时 `ps` 状态为 `T` |
| 内核 rpmsg | `CONFIG_RPMSG_TTY=y`（内置）、`CONFIG_RPMSG_CHAR` **未开** ⇒ 通道即 `/dev/ttyRPMSG0` | `/proc/config.gz` |
| remoteproc | 固件名/状态经 `/sys/class/remoteproc/remoteproc0/{firmware,state}`，**重启后 firmware 名需重设** | 见 `a7-m4_standard.md` §3.1 |
| 交叉工具链 | `arm-buildroot-linux-gnueabihf-`（100ASK SDK；gcc 8.4 / glibc ≈2.31） | 交叉编译产出 ARM 32-bit ELF |

---

## 2. 交叉编译与部署标准流程

### 2.1 C 侧（core0_service）

```sh
# 在开发机（book）的仓库根目录
cd core0_service
make CROSS_COMPILE=arm-buildroot-linux-gnueabihf- core0_business core1_stub
scp core0_business core1_stub root@<board-ip>:/root/     # 文档里不写死本机地址
```

- **`-lrt` 必需**：`shm_open()` 在 glibc<2.34 属于 librt（Makefile 已按宿主区分，MinGW 宿主不加）。
- **宿主语法检查 ≠ 交叉编译**：`make host-check-win`（MinGW + `tools/hostcheck/` 桩头文件）只做语法体检；
  真 arm glibc 的头文件/链接差异只有交叉编译才会暴露（曾漏掉 `app_config.h` 缺 `<stddef.h>`、`shm_open` 链接失败）。
- 整套服务/单元文件部署用 `sh deploy/systemd/install_all.sh`（9 步；**不覆盖已存在的 `/etc/park/cloud.conf`**）。

### 2.2 Qt 侧（core1_ui/qt_gui）

```sh
cd core1_ui/qt_gui && sh tools/build_arm.sh      # SDK 软链 + 修 sysroot + make
scp bin/park_ui root@<board-ip>:/root/park_ui
# 板端
cp /root/park_ui /opt/park_ui/park_ui && chmod +x /opt/park_ui/park_ui
systemctl restart park-ui
```

- **必须用与板端同版本的 Qt（5.12.8）编译**；用 OpenSTLinux SDK 的 Qt 5.14.1 编出来的二进制，
  板端启动即报 `QtPrivate::argToQString ... version Qt_5`（符号版本不兼容）。
- 该 SDK 的 qmake 需要一个空的 `local_features/force_asserts.prf` 绕过缺失 feature（`build_arm.sh` 已处理）。
- **防伪检查**：`strings bin/park_ui | grep -c "python transport"`（新功能特征串）——非 0 才说明拷上去的是新二进制。
  板上"改了没生效"最常见的根因就是**跑的还是旧二进制**（`/opt/park_ui/park_ui` 没换或服务没重启）。

### 2.3 板端验收脚本

- 首选纯 stdlib `python3 - <<'PYEOF' ... PYEOF`（即贴即跑，`/tmp` 被清也不要紧）；
- 或跑现成脚本：`python3 /opt/core0/tools/rpmsg_link_test.py --listen=6`（RPMSG 链路体检，退出码 0 = PASS）。

---

## 3. 板端运行与排障铁律

1. **板端粘贴的脚本/命令必须纯 ASCII**（注释、print、报错字符串一律英文）。
   中文经 SSH 粘贴会被截断/转码，python3 直接报 `Non-UTF-8 code starting with '\xe8'` 或
   `invalid character in identifier`——即使中文只出现在注释里也一样坏。
2. **不要用 `pgrep`/`pkill`/`timeout`**（不存在）。杀进程用 `/proc` 扫描 + `kill`，例如
   `for p in /proc/[0-9]*; do grep -q mxapp2 $p/cmdline 2>/dev/null && kill -9 ${p#/proc/}; done`；
   给命令限时用后台 + `sleep` + `kill`。
3. **服务日志看 journal**：`journalctl -u park-ui -b --no-pager | tail -50`、
   `journalctl -u core0-bus -b`、`journalctl -b | grep cloud`。
4. **`/etc/profile` 里的环境变量对 systemd 服务无效**：给服务传参走 `EnvironmentFile=/etc/park-ui.env`。
5. **手动前台跑 GUI/守护时 `< /dev/null`**（见 §1 SIGTTIN 条）。
6. **时钟先于 TLS**：无 RTC ⇒ 开机先校时（`park-clock.service`/`park-clock.timer`，或设置页「诊断 → 同步」）。
   年份 <2025 时屏上会提示 `NOT SET: cloud TLS will fail (certificate is not yet valid)`。
7. **串口/字符设备用 `O_NONBLOCK` + raw termios 时不要设 `VMIN=0/VTIME=0`**：
   内核 `n_tty_read()` 在 `timeout==0` 时返回 0，会被上层当成 EOF（曾导致 RPMSG 链路每秒 UP/DOWN 假断链）。
   保持 `cfmakeraw` 的 `VMIN=1/VTIME=0`。
8. **`/dev/shm/park_shm` 是共享内存的真路径**；用外部脚本戳"远程开闸/关闸"就是写它的偏移 52/53
   （字段表见 `docs/protocols.md` §4）。

---

## 4. systemd 服务与启动链（当前定版）

```
board-power → wifi-up → park-clock → m4-load → core0-bus → park-ui
```

| 单元 | 作用 | 关键点 |
|---|---|---|
| `board-power.service` | 拉高 **GPIO 82/139** 给板载 USB HUB/主机供电 | 禁用厂商 HMI 后必须自己补，否则单板 USB 全哑 |
| `wifi-up.service` | 开机拉起 wlan0（`ip link up` → 杀旧 wpa_supplicant → `-B -D nl80211` → `udhcpc` 最多 3 轮） | 原属厂商桌面服务的活；日志**不打印 IP/密码** |
| `park-clock.service` / `.timer` | 开机校时 + 每 15min 再校（`TimeoutStartSec=180`） | 覆盖 DHCP/DNS 未就绪窗口 |
| `m4-load.service` | `load_m4.sh start`（含 `ensure_can_clock` + driver_override/bind） | `SuccessExitStatus=0 1 2` 容忍 M4 flaky |
| `core0-bus.service` | `core0_business -c /opt/core0/core0.conf` | `Restart=always`；缺 ttyRPMSG0 也能起（内部重试） |
| `park-ui.service` | Qt 全屏 linuxfb | `After/Wants` 前置全部就绪 |

- **禁用任何厂商服务前，先查它附带的外设初始化**：`myir.service` 除了霸屏（mxapp2 eglfs 独占 DRM），
  还负责 USB 供电——`systemctl disable --now myir.service` 会连带把 USB Host 关掉（`lsusb` 只剩 root hub）。
  这是本项目踩过的坑，`board-power.service` 就是它的补丁。
- 部署：`sh deploy/systemd/install_all.sh`（布局 `/opt/core0/...`、`/opt/park_ui/park_ui`、
  `/lib/firmware/m4_fw.elf`；已存在的配置文件不覆盖）。
- 只装 UI 用 `install_park_ui.sh`（薄壳）；UI-only 变更 = 重编 + `scp` + `cp` + `systemctl restart park-ui`。

---

## 5. 网络、TLS 与云（板级事实）

- **网络单链路（仅板载 WiFi）**：`ip link set wlan0 up` → `wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf`
  → `udhcpc -i wlan0 -n -q -t 5 -T 3`。写配置前留 `.bak`，**20s 内拿不到租约自动回滚**（改 WiFi = 拆自己脚下的 SSH 梯子）。
- **CA 必须自备**：把 PC 上的 CA bundle 拷成 `/etc/park/ca.pem`（或用 `cloud.conf` 的 `ca_file=`），否则 HTTPS 必失败；
  临时排障可用 `insecure_tls=1`（不长期开）。
- **Qt 5.12.8 + OpenSSL 1.1.1 在本板 TLS 1.3 协商会卡死**（同一条请求 python3 ~1s 拿到 200）⇒
  云通道默认 **`transport=auto`**：Qt 失败自动切 `python3` 子进程回退并保持该通道；也可强制 `transport=python`。
- 云请求**只允许在 Core1（park_ui 进程内）** 发起；Core0 一行云代码都没有（静态门禁 `check_static.py` 会拦）。
- 密钥/隐私：真配置文件（`cloud.conf`/`wpa_supplicant.conf`/`park-ui.env`/`core0.conf`/`key.txt`）**一律不入库**，
  仓库只放 `sample_<原名>` 模板；日志里只出现掩码 key（`sk-abcd...wxyz`），**本机 IP 不上屏、不落盘**。

---

## 6. A7 ↔ M4 / 共享内存（只列入口，细节见专文）

| 主题 | 权威文档 |
|---|---|
| RPMSG 链路（`/dev/ttyRPMSG0`，0x11/0x12/0x13/0x21/0x22/0x23/0x7E） | `docs/protocols.md` §3 + `MD文档/a7-m4_standard.md` |
| CAN（C8T6↔M4，0x100/0x110/0x200/0x210；接口 v2 语义事件） | `docs/protocols.md` §1 + `c8t6/can.md` |
| 共享内存 `park_shm` v3（76B，字段/偏移/归属） | `docs/protocols.md` §4；Core1 **必须 `O_RDWR`** |
| 云端 HTTP（请求体/超时/重试/容错解析/阈值口径） | `docs/protocols.md` §5 |

**A7 侧两条硬规则**：
1. `can0` **保持绑定 + `ip link set can0 type can bitrate 500000` + `can0 up`**（点亮 fdcan_k 时钟），
   A7 **静听不发**；⚠️ 不要 `unbind 4400f000.can`——会关时钟导致 M4 的 FDCAN 初始化卡死
   （根因与完整 recipe 见 `a7-m4_standard.md` §4.6）。
2. 只允许**一个**读者打开 `/dev/ttyRPMSG0`：同时跑 `core0_business` 和验收脚本会把字节流劈成两半，两边看起来都"坏"。

---

## 7. 交付纪律（本项目级）

1. **不自动 git 提交**：改完只留工作区，等明确指令再提交；提交信息临时文件写到仓库外。
2. **隐私样例**：任何含密钥/密码/本地地址的配置文件，仓库里只能有 `sample_` 模板（纯 ASCII + 占位值）。
3. **板端-facing 的脚本/字符串纯 ASCII**（§3.1）；GUI 源码同样保持纯 ASCII（静态门禁会检查）。
4. **静态门禁先跑**：`python core1_ui/qt_gui/tools/check_static.py`（Qt/Core1 侧）；
   `make host-check-win selftest`（Core0 纯逻辑侧）；两者通过 ≠ 板上能跑，仍需交叉编译 + 板验。
5. **改协议先改母本**：`docs/protocols.md`（草案母本 `PhaseMd/10`）→ 再改两端代码 → 两处变更记录各留一行。

---

## 8. 尚未实测确认（**因此本文件正文不写**，补测后再回填）

| 待确认项 | 为什么现在不写 |
|---|---|
| 启动引导用 `extlinux.conf` 还是 `uEnv.txt`（影响 `bootargs` 改法） | 未在板上逐项核对 `/boot` 结构 |
| A7/M4 核隔离（`isolcpus`/`nohz_full`/IRQ 亲和/systemd `CPUAffinity`） | 方案已定但**未实施、未板验**，收益与副作用未知 |
| 24h 长稳、冷启动 ×3、六类故障注入 | 未执行 |
| K210 侧二进制流通道参数（波特率极限/流控/接线） | 未接线实测（当前走 console 文本行） |
| 板端 SD 卡挂载（K210 模型加载前置） | 现卡仍 `ENODEV`，未换卡验证 |
| MQTT 上报链路 | 未实现（协议仅占位） |
| 板端是否存在 `sqlite3` 库/命令行 | 未核对；本项目已定调**不用数据库**，不影响主线 |

---

## 变更记录

| 日期 | 变更 |
|---|---|
| 2026-09-11 | 创建：把本项目 A7 侧已验证的板级事实、交叉编译/部署配方、运行铁律、systemd 启动链、网络/TLS 事实与交付纪律固化为规范；未实测项集中列在 §8，不混入正文 |
