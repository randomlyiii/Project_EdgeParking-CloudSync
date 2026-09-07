# k210_link —— A7 侧 K210 预览流接收（第5步 P5-09 / P5-13 雏形）

> 目标：把 K210 的 JPEG 预览流在 **100ASK-MP157 的 Linux** 上接收并显示到 LCD，验证 K210 拍摄能力。
> 帧协议见 `docs/protocols.md` §2（K210 UART，CRC16=XMODEM，0x01 分块 + 0x02 帧尾）。
> 依赖：MP157 上的 python3（纯标准库，不需要 pyserial）。

## 一、前提：确认数据链路

K210↔MP157 两条可选链路，由 `k210_fw/main_single.py` 顶部 `LINK` 决定：

| LINK | 物理链路 | MP157 侧设备 | 说明 |
|---|---|---|---|
| `"cdc"`（当前） | K210 USB → MP157 USB 口 | `/dev/ttyACM0` | 复用你已接的 USB；与 REPL 同一条 CDC，**运行中不可连 CanMV IDE**；K210 侧仅发不收 |
| `"uart"` | K210 板级串口 TX/RX 杜邦线 → MP157 串口 | `/dev/ttySTM*` 或 `/dev/ttyS*` | 架构主线（921600 数据 UART）；需按板卡接线 |

> `cdc` 链路最省事（已有 USB），本 README 用 cdc 演示；`uart` 同理，只把脚本的设备参数换掉即可。

## 二、K210 侧（让板子脱离 PC、独立发帧）

1. 打开 `k210_fw/main_single.py`，确认顶部：
   - `LINK = "cdc"`
   - `JPEG_QUALITY = 65`（目标单帧 8~15KB；若 `[stat] jpeg=` 仍 >100KB → 改 `JPEG_HW = True` 走硬件 JPEG）
   - `LCD_PREVIEW = False`（关掉 K210 板载屏刷新，省 20~30ms/帧把 fps 让给链路）
2. 把 `main_single.py` **另存为板子 flash 根目录的 `main.py`**（CanMV IDE 文件管理上传为 main.py，覆盖或保存），使 K210 上电自启它、不再依赖 IDE。
3. **断开 PC 上的 CanMV IDE**（cdc 模式下与 IDE/REPL 共用，冲突），把 K210 的 USB 插到 **MP157 的 USB 口**。
4. K210 上电复位 → 开始向 CDC 发预览帧。此模式 K210 不打印（静默，避免污染帧流），fps 看 MP157 侧统计即可。

## 三、MP157 侧（收帧 + 落盘 + 屏显）

在 MP157 的 Linux 终端（串口登录）：

```bash
# 1) 确认设备出现
ls /dev/ttyACM*                # 应见 /dev/ttyACM0；否则 dmesg | tail 查枚举

# 2) 收帧并存 JPEG（跑几秒后 Ctrl-C）
python3 k210_preview_rx.py /dev/ttyACM0 --baud 921600
#    输出如：frame#1 kind=0 14823B -> /tmp/k210_frame.jpg (dropped=0)
#    若 dropped 持续增长 -> 波特率/接线/丢帧问题；若 3s 无帧 -> 端口或波特率错

# 3) 把最后一帧显示到 LCD 帧缓冲（设备名按你板子：/dev/fb0 或 /dev/dri/card0）
fbi -a -d /dev/fb0 /tmp/k210_frame.jpg
#    fbi 未装则: apt-get install -y fbi 或改用 feh/Qt 方案
```

> 若想连续刷屏（临时验证视频感）：
> ```bash
> while :; do fbi -a -d /dev/fb0 /tmp/k210_frame.jpg; done
> ```

## 四、帧率说明（重要）

- 你在 CanMV IDE 里看到的 `fps=2` 含 **IDE 帧缓冲回传限速**，不是真实链路帧率；**验收以 K210 脱离 IDE、MP157 侧实测为准**。
- 真实链路瓶颈是**每帧字节数**：921600bps≈92KB/s。目标 JPEG 8~15KB/帧 → 约 6~10fps；若每帧仍 ~200KB，将压到 ~1fps。**先把 JPEG 压到 8~15KB**（改 `JPEG_QUALITY`/`JPEG_HW`）。

## 五、待确认/下一步

- MP157 的 LCD 是 framebuffer（`/dev/fb0`）还是 DRM（`/dev/dri/card0`）？决定 `fbi` 用法。（`ls /dev/fb* /dev/dri/`）
- fbi 是否已安装？
- 若后续要完整的 Qt 预览窗口 + 车牌/车位界面，走 P5-13 → 第6步 core1_ui `qt_gui`（本目录只做串口前端雏形）。
