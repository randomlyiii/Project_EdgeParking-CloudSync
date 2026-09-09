# qt_gui · park_ui（Core1 Qt 界面，PhaseMd/11 实现）

QWidget 实现（板上 Qt 5.12.8 已带 Quick/Controls2，但 QWidget 免 QML 导入插件依赖、软渲染更省 CPU）。规格映射：Q-02 布局 / Q-03 预览 / Q-04 车牌弹卡 / Q-05 状态区故障字，数据通路=规格 §2。

## 1. 代码结构

| 文件 | 职责 |
|---|---|
| `src/main.cpp` | 参数解析、字体、装配 link/ipc/UI |
| `src/mainwindow.{h,cpp}` | 1024×600 布局：状态栏 32px / 预览区 / 右栏车位·车牌 / 底栏闸+事件行；弹卡 3s（DENY 红） |
| `src/k210_link.{h,cpp}` | k210_link 接收线程：**text(console base64，现役链路) + binary(protocols §0/§2 帧协议) 双解析 auto 同跑** + file 模式；JPEG 软解在线程内，UI 只取最新帧（丢帧保最新） |
| `src/ipc_reader.{h,cpp}` | `/park_shm` 读端（seq 双读防撕裂）+ eventfd(P6-02) 或 200ms 轮询；**shm 不在时自动 demo 模拟器**（车位漂移/开闸/弹卡/事件） |
| `src/park_shm.h` | `park_shm_t` 布局（PhaseMd/07 P6-01），Core0 落地后与 `core0_service/ipc_shm` 合并为公共头 |

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

```
# PC 开发（桌面预览，需装 Qt 5.12.x——板上是 5.12.8，别用 5.15 编）
qmake && make && ./bin/park_ui --mode file -f ./testimgs --demo on

# 板（100ASK/ST SDK 交叉，只编自家 app，Qt 用板上现成 5.12.8 运行库）
source <SDK>/environment-setup-cortexa7t2hf-neon-vfpv4-... 
qmake && make
scp bin/park_ui root@<board>:/usr/bin/
```

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
| 云状态 | 第7步接入，现显示 `CLOUD:--`（demo 显示 OK） |

## 5. 待办

- Core0 落地 P6-01/02 后：合并非 `park_shm.h` 双副本、接 `--eventfd`、`--demo` 退场
- 第7步：cloud 线程状态接入 CLOUD 状态位；抓拍图（0x02 purpose=1）转发云兜底（`snapshotCaptured` 信号已预留）
- K210 busy 位（0x7E bit0）接识别徽标（worker 已解析，信号未引出）
