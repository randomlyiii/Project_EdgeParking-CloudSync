# deploy —— 板端部署层（systemd/init、运行环境、k210+qt 业务拉起）

> 对端参考：PhaseMd/11（Qt 规格）、core1_ui/qt_gui（park_ui 工程）、k210_fw（K210 端 main.py 自启）。
> 板端规范：脚本纯 ASCII；root 无 sudo。

## 结构

| 文件 | 用途 |
|---|---|
| `systemd/park-ui.service` | park_ui 开机自启（linuxfb fb0、`Restart=always`、可配 `/etc/park-ui.env`） |
| `systemd/install_park_ui.sh` | 板上安装：拷二进制 + 装 unit + 打印"默认页面"排障清单 |

## 安装（板上 root）

```sh
# PC: scp -r deploy <board>:~/
# PC: scp core1_ui/qt_gui/bin/park_ui <board>:~/
sh deploy/systemd/install_park_ui.sh        # 自动装 unit；SRC=~/park_ui
systemctl start park-ui
journalctl -u park-ui -f
```

## LCD「默认页面」问题（2026-09-10 待板上定位）

现象：开机后 LCD 停在厂商默认页面（非本 UI）；park_ui 起来后仍被盖住；
仅 `poweroff` 时闪现一下 park_ui 画面 → **另有进程占着显示**（vendor Qt demo /
fbcon 控制台 / eglfs 抢 DRM），它先于/盖住 park_ui 的 linuxfb 输出。

定位与处置（板上，逐条）：

```sh
ps -ef | grep -iE 'qt|demo|eglfs|weston|lcd|splash'     # 谁在画
ls /etc/init.d/ && cat /etc/inittab 2>/dev/null          # init 脚本/自启
cat /sys/class/graphics/fb0/name                         # stmdrmfb 预期
```

处置：停掉/禁用该进程或 init 脚本后 `systemctl restart park-ui`。
（若确认是 fbcon/console 抢占，可 `con2fbmap 1 0` 或把 console=tty1 从 cmdline 移除，
详见后续实测记录。）

## K210 联动

- K210 端：`k210_fw/main.py`（LINK=console，base64 文本流）已在 K210 flash 自启，
  板上设备 `/dev/ttyACM0`（USB CDC）。
- park_ui `--mode auto` 同时喂 text/binary 两个解析器：现役 console 文本链路无需改参。
- K210 不在场：`--mode file -f <图>` 或 `--demo on` 均可演示 UI。
