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

安装布局：

```
/opt/core0/core0_business            业务守护
/opt/core0/core0.conf                本地配置（白名单/车位/超时/阈值）＝唯一事实源，热加载
/opt/core0/tools/load_m4.sh          M4 加载脚本（m4-load.service 调用）
/opt/core0/tools/rpmsg_link_test.py  RPMSG 链路检测（python3，零编译）
/opt/park_ui/park_ui                 Qt UI
/lib/firmware/m4_fw.elf              M4 固件
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

`install_all.sh` **不会覆盖**已存在的 `/opt/core0/core0.conf`（配置文件是操作员的事实源）。

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
- **K210 只有 UI 没有画面**：`ls -l /dev/ttyACM0`；K210 端 `main.py` 需在跑
  （`LINK="console"`、`CONSOLE_PREVIEW=1`）。UI 方向已定案为「不做任何翻转」
  （固件 `CAM_SW_HMIRROR=True` 一处修正同时修好两个屏）。
- **两个进程抢 `/dev/ttyRPMSG0`**：同一时刻只能有一个读者（core0_business 或
  rpmsg_demo 或 rpmsg_link_test.py）。跑检测脚本前先 `systemctl stop core0-bus`。
