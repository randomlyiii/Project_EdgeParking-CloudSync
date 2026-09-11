# qt_gui · park_ui（Core1 Qt 界面，PhaseMd/11 实现）

QWidget 实现（板上 Qt 5.12.8 已带 Quick/Controls2，但 QWidget 免 QML 导入插件依赖、软渲染更省 CPU）。规格映射：Q-02 布局 / Q-03 预览 / Q-04 车牌弹卡 / Q-05 状态区故障字，数据通路=规格 §2。

## 1. 代码结构

| 文件 | 职责 |
|---|---|
| `src/main.cpp` | 参数解析、字体、装配 link/ipc/UI |
| `src/mainwindow.{h,cpp}` | 1024×600 布局：状态栏 32px / 预览区 / 右栏车位·车牌 / 底栏闸+事件行；弹卡 3s（DENY 红） |
| `src/k210_link.{h,cpp}` | k210_link 接收线程：**text(console base64，现役链路) + binary(protocols §0/§2 帧协议) 双解析 auto 同跑** + file 模式；JPEG 软解在线程内，UI 只取最新帧（丢帧保最新） |
| `src/ipc_reader.{h,cpp}` | `/park_shm` 读端（seq 双读防撕裂）+ eventfd(P6-02) 或 200ms 轮询；**shm 不在时自动 demo 模拟器**（车位漂移/开闸/弹卡/事件） |
| `src/ipc_writer.{h,cpp}` | **`/park_shm` 写端（Core1 业务写端，core1_business spec §5）**：`O_RDWR` 挂载 + 版本自检；1s 心跳 `hb_core1`、识别结果回写（`plate/confidence/result_source/result_valid` + `evt RESULT`）、低置信/失败置 `cloud_pending`、远程开/关闸脉冲 `req_gate_*`、200ms 消费 `evt_c0`；**只写 Core1 归属字段**（静态检查 tools/check_static.py 有负向断言） |
| `src/park_shm.h` | `park_shm_t` 布局（PhaseMd/07 P6-01），Core0 落地后与 `core0_service/ipc_shm` 合并为公共头 |
| **`src/cloud_settings.{h,cpp}`** | 第7步云端配置持久化：`/etc/park/cloud.conf`（`$PARK_CLOUD_CONF` 可覆盖、`$DEEPSEEK_API_KEY` 兜底注入），`QSaveFile` 原子写 + `.bak`，越界值夹取 + 告警；**key 只以掩码进日志** |
| **`src/cloud_client.{h,cpp}`** | 第7步云兜底客户端（Qt Network 异步 POST，非 libcurl）：JPEG q70 → base64 → OpenAI 兼容 `/chat/completions`；5s `QTimer` 超时 + `abort()`、传输类错误重试 ≤1、三级容错解析（去围栏→括号配对→`QJsonDocument`）、401/402/404/429/5xx 分类提示、断网演练（不出网卡）+ 假结果模式；**不碰 shm**（由 main.cpp 接 `IpcWriter`） |
| **`src/wifi_manager.{h,cpp}`** | 底栏 WIFI 芯片 + 设置页网络页签：`ip link up` + `wpa_supplicant -B -D nl80211` + `udhcpc`（厂商三步 recipe），写配置前备份 `.bak`、**20s 无 IP 自动回滚**；无 `iw` 时退 `wpa_cli scan`；板端无 `pgrep/pkill` → `/proc` 扫描 |
| **`src/softkeyboard.{h,cpp}`** | 无键盘板端的全屏软键盘（`SoftKeyboard::getText()`，密码框掩码 + SHOW 切换），供 API key / SSID / 密码输入 |
| **`src/settingspage.{h,cpp}`** | 齿轮进入的全屏设置页（云端/网络/诊断三页签）：阈值 ±0.05、API 地址/模型（含预设轮换）/key、5 个开关、测试连接、云端复检、WiFi 连接/断开/扫描/恢复、诊断命令；Core0 `conf_threshold` 只读展示 |

中文 UI 文案一律 UTF-8 转义写死（源码保持纯 ASCII，板端规则）。

## 2. 运行模式（命令行）

```
park_ui [-d /dev/ttyACM0] [-b 115200] [-m auto|text|binary|file|none]
        [-f <图片或目录>] [--demo on|off|auto] [--eventfd <fd>]
```

- `--mode auto`（默认）：串口字节流同时喂 text/binary 两个解析器，K210 换杜邦线二进制帧后无需改代码。
- `--mode file -f /path`：本地图片循环当预览（PC 开发 / 板上无 K210 演示），~5fps。
- `--demo on`：强制模拟数据；`auto`（默认）= 无 `/park_shm` 写端时模拟、Core0 起来后自动切真数据。

## 3. 构建（Qt 5.12.8 对齐，规格 Q-01 修订版）

`qt_gui.pro` 现在带 **`QT += network`**（第7步云兜底用 `QNetworkAccessManager`）——板上 rootfs 自带 `libQt5Network.so.5.12.8`（**2026-09-11 实测在**），交叉编只要 SDK 里有 QtNetwork 头/库即可，**不需要 libcurl**（板端也**没有 curl**）。

> ⚠️ **HTTPS 必须自带 CA**：该 rootfs **没有** `/etc/ssl/certs/ca-certificates.crt`（只有 `libssl.so.1.1`/`libcrypto.so.1.1`）⇒ 要么把一份 bundle 放到 `/etc/park/ca.pem`（程序自动优先使用）、要么在 `/etc/park/cloud.conf` 写 `ca_file=`、要么演示时开 `insecure_tls=1`。启动日志打印 `supportsSsl` / SSL 库版本 / 实际 CA 路径，设置页「云端」底部同步显示。细节见 `G7_ACCEPTANCE.md` §2.1~2.2。
> 另一条板级事实：`ip` / `iw` / `wpa_cli` / `udhcpc` / `wpa_supplicant` / `systemctl` 都在 **`/sbin`**，而 systemd 交给服务的 PATH 不保证含 `/sbin` ⇒ 代码里一律经 `toolPath()` 解析成绝对路径，**别改回裸名字**。

实测可用的 book 交叉编译配方（Qt 5.12.8 对齐板端，2026-09-11 定稿）——**推荐直接用脚本，别手敲**：

```sh
# book 上，一条命令搞定（老路径软链 + qmake + 修 Makefile + touch + make + 自检）
cd ~/core1_ui/qt_gui
sh tools/build_arm.sh
# 也可以显式指定 SDK 根目录：
# sh tools/build_arm.sh /path/to/arm-buildroot-linux-gnueabihf_sdk-buildroot
```

脚本处理的三个坑（手敲版见下）：

1. **这份 Buildroot SDK 的 Qt mkspec 把当年编译时的绝对路径烧死了**：
   `/home/book/stm32mp157/ST-Buildroot/output/host`。`qmake` 在缺 `.qmake.stash` 时会**跑编译器探测**，老路径不存在就直接
   `Project ERROR: Cannot run target compiler '.../ST-Buildroot/.../g++'`。
   脚本用**软链把老路径指到现在的 SDK**（无需 sudo，不碰 `/home/book` 之外）——编译器、sysroot、ar/ld 全部一次解决。
2. `qmake` 写出的 `--sysroot=` 指向老 sysroot，脚本改回真实路径；
3. 只改 `.cpp/.h` 也必须重跑 `qmake`（否则 `make` 用旧 Makefile 里的老路径 → `g++: Command not found`）。

等价手敲版（脚本报错时照这个排查）：

```sh
export SDK=/home/book/100ask_stm32mp157_pro-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot
export PATH=$SDK/bin:$PATH
SYS=$SDK/arm-buildroot-linux-gnueabihf/sysroot
LEGACY=/home/book/stm32mp157/ST-Buildroot/output/host
mkdir -p $(dirname $LEGACY) && ln -s $SDK $LEGACY     # 若 $LEGACY 已存在则跳过
cd ~/core1_ui/qt_gui
rm -f Makefile .qmake.stash
qmake QMAKE_CC=$SDK/bin/arm-buildroot-linux-gnueabihf-gcc QMAKE_CXX=$SDK/bin/arm-buildroot-linux-gnueabihf-g++
sed -i "s|$LEGACY|$SDK|g; s|--sysroot=[^ ]*|--sysroot=$SYS|g" Makefile
touch src/*.cpp src/*.h
make -j4
ls -l bin/park_ui                                     # ARM 32-bit ELF
strings bin/park_ui | grep -c 'python transport'      # 第7步标记，0 = 编出来的是旧代码
```

> 其他坑：① 用 OpenSTLinux SDK 的 Qt 5.14.1 编出来的二进制在板上报 `argToQString ... version Qt_5`，**必须用这份 100ASK SDK（Qt 5.12.8）**；② `local_features/force_asserts.prf` 缺失时补空文件；③ 环境变量/qt.conf 混入别家 sysroot 会链错库；④ 判断板上跑的是不是新二进制：看启动日志有没有 `cloud: proxy mode=... transport=...`，或 `strings /opt/park_ui/park_ui | grep -c 'python transport'`。

部署（板上 `/opt/park_ui/park_ui`，与 `deploy/systemd/park-ui.service` 一致）：

```sh
scp bin/park_ui root@<board>:/root/park_ui
ssh root@<board> 'mkdir -p /opt/park_ui && cp /root/park_ui /opt/park_ui/park_ui && chmod +x /opt/park_ui/park_ui'
ssh root@<board> 'systemctl restart park-ui'      # 自启链 m4-load -> core0-bus -> park-ui
```

手动跑（调试用）：`QT_QPA_PLATFORM=linuxfb:nocursor=1 /opt/park_ui/park_ui < /dev/null`（**必须 `< /dev/null`**，否则后台进程读终端被 SIGTTIN 停住）。

板上运行（无合成器，规格 §3）：

```
export QT_QPA_PLATFORM=linuxfb:nocursor=1   # 主路线，MP157 无 GPU 不折腾 eglfs
export QT_QPA_FONTDIR=/usr/share/fonts      # 确认目录下有 CJK 字体，无则拷一款
export LD_LIBRARY_PATH=/usr/lib
park_ui --demo on                           # 首次验证用 demo；正式随 systemd 自启(第8步)
```

板上首次验证清单（规格 §3 三件）：① `ls /usr/lib/qt5/plugins/platforms/` 有 `libqlinuxfb.so`；② CJK 字体在位（中文无豆腐块）；③ `park_ui --mode file -f <img>` 出画面即 Q-01 验收过。

## 4. 数据通路现状（对齐规格 §2）

| 元素 | 现状 |
|---|---|
| 预览视频 | ✅ k210_link 线程解码、100ms 取最新帧；text 链路即现役 console base64 |
| 车牌/置信度/来源 | 弹卡：真机走 K210 0xC2/0xC3 帧（binary 链路）或 demo；常驻栏读 shm `plate/confidence/result_source` |
| 车位/闸/故障字 | shm `free/used/gate_state/link_flags`（eventfd 0x03 到位后自动事件驱动，`--eventfd` 接入；现在 200ms 轮询，KPI ≤500ms 满足） |
| 云状态 | ✅ 三级优先：demo > shm `cloud_pending=1`（`CLOUD:兜底中`，Core0 镜像，单一事实源）> 第7步云客户端健康（idle/ok/no key/超时/断网演练/失败分类） |
| 识别结果回写 | ✅ `IpcWriter`：K210 结果 ≥`conf_threshold` → 写 shm + `evt RESULT`；<阈值或失败 → `cloud_pending=1` + `cloudFallbackRequested` → 云兜底 → `onCloudResult`（`result_source=1`）或 `clearCloudPending()` |
| 心跳/存活 | ✅ `IpcWriter` 1s 自增 `hb_core1`（主线程 QTimer，进程挂起即停）→ Core0 3s 判活不再误判 Core1 离线 |
| 远程开/关闸 | ✅ 底栏「开闸/关闸」按钮（触摸）+ `O`/`C` 热键 → `req_gate_open/close` 脉冲（spec 5.6「来源可插拔」，写路径统一） |
| 网络状态 | ✅ 底栏 WIFI 芯片（2s 轮询 wlan0：无租约红 `WIFI:down`，有租约绿 `WIFI:<ssid>`；**只上屏 SSID，本机 IP/网关绝不上屏、不落盘、不进 journal**——公开 LCD 暴露地址易被攻击） |
| 云端运维 | ✅ 底栏齿轮 → 设置页（云端/网络/诊断）；底栏「云端复检」= 用最近一帧手动问一次云 |

## 5. 板上验收（P6-04 本地链路 / P7 云端）

先跑静态检查（无需 Qt 工具链；纯 ASCII + 信号签名 + Core0/Core1 字段归属 + 第7步负向断言）：

```
python3 tools/check_static.py     # 期望 RESULT: PASS
```

板上（Core0 已在跑，`/dev/shm/park_shm` v3）：

```
systemctl restart park-ui
journalctl -u park-ui -n 20 --no-pager        # 期望 ipc-writer: /park_shm attached (v3)
python3 -c "import mmap,os,struct;b=mmap.mmap(os.open('/dev/shm/park_shm',os.O_RDWR),76,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE);print('hb=%d gate=%d link=0x%02X'%(struct.unpack_from('<I',b,48)[0],b[20],b[21]));a=struct.unpack_from('<I',b,48)[0];import time;time.sleep(3);print('hb after 3s=%d'%struct.unpack_from('<I',b,48)[0])"
```

判读：`hb` 3 秒内至少 +2（Core0 3s 判活窗口）→ **屏上 `CORE1:在线`**；日志里出现 `ipc-writer: gate open requested` / `[remote] gate ... (ui)` 配对即回写链路通。手遮 BH1750 走完「车到位→识别→超时降级」时，`CLOUD:兜底中` 只会出现在低置信/失败路径。

**第7步（G7）逐项验收步骤见 `core1_ui/qt_gui/G7_ACCEPTANCE.md`**（云端阈值/手动复检/模型切换/WiFi 改造与回滚/断网演练/假结果/自动兜底七项 + 日志期望原文）。

## 6. 待办

- 抓拍图（0x02 purpose=1）转发云兜底：K210 现役是 console 文本链路、无下行抓拍通道，云请求用 `latestFrame()`（同一画面最新帧）等价替代
- K210 busy 位（0x7E bit0）接识别徽标（worker 已解析，信号未引出）
- MQTT 云上报（P7-08~10，可选）未实现，配置结构已预留扩展位
