# K210 固件（CanMV MicroPython）

> 本目录 = 第5步（K210 ↔ Linux）的 K210 端固件，路线 **2026-09-07 定版：CanMV MicroPython + CanMV IDE**（原 Kendryte standalone SDK(C) 方案降为备选，见 `PhaseMd/06` P5-01）。
> 帧协议权威定义：`docs/protocols.md` §2（草案母本 `PhaseMd/10`）。

## 目录结构（四模块语义不变，载体为 .py）

| 文件 | 对应 PhaseMd/06 | 职责 |
|---|---|---|
| `boot.py` | — | 上电引导（保持极简，业务入口在 main.py） |
| `main.py` | P5-04/05/07 | 主循环编排：预览流常开、UART 收帧分发、识别触发、结果/心跳上报 |
| `config.py` | P5-各步调优点 | 集中参数：波特率/分块/质量/阈值/冷却（首次上板在此核对硬件常量） |
| `uart_proto.py` | P5-02 | 帧协议层（纯 Python，**无板级依赖**，PC 可跑自检） |
| `capture.py` | P5-03 | 摄像头采集 + JPEG 编码（**硬件 API 待真机填实**） |
| `preview.py` | P5-04 | 预览流拆帧：0x01 块 + 0x02 帧尾（4B 总长 + 1B 用途） |
| `infer.py` | P5-06 | 车牌识别（路径 A：kmodel；路径 B 默认：仅占位→云兜底） |
| `tools/selftest.py` | P5-02 验收 | PC 端协议自检（CRC 向量/组帧/拆帧/坏帧容错），无需板子 |

## 开发 / 上传流程（PC）

1. 装 **CanMV IDE**（嘉楠官方，GitHub `kendryte/canmv` Releases）；Windows 首次插 USB 装驱动。
2. 板子 USB 插 **PC**；IDE 连接后先跑官方 **sensor 摄像头例程**，确认固件版本与 `sensor`/`machine.UART` API（`capture.py` 的 TODO 点以此为准回填）。
3. 把 `config.py / uart_proto.py / capture.py / preview.py / infer.py / main.py` 传到板子文件系统（CanMV IDE 或 REPL 文件操作）；`boot.py` 可选。
4. 复位后 `main.py` 自启；IDE 串口终端（REPL，115200）可 Ctrl-C 中断、`import main` 手动跑。

> ⚠️ **多文件 import 的坑**：CanMV IDE 的「运行」只投递**当前打开的这一个文件**，跨文件 `from config import ...` 能否命中和模块文件**有没有落到固件实际搜索的目录**有关（一般是 flash 根；传到 `/sd` 或子文件夹会 `ImportError: no module named 'config'`）。`main.py` 已在顶部把 `/flash`、`/sd` 补进 `sys.path`。
> 若不想折腾上传：直接运行 **`main_single.py`**（全内联单文件，逻辑一致），IDE 只跑它即可。

> 数据 UART（921600 起步）走板级引脚、与 USB REPL **物理分离**，二者不混用。
> 板子 USB 插到 MP157 时，Linux 侧出现 `/dev/ttyACM0`（=K210 REPL），可作为后续 A7 侧联调通道。

## PC 端协议自检（无需板子）

```
python tools/selftest.py
```

校验：CRC16-XMODEM 标准向量（"123456789"→0x31C3）、组帧/逐字节喂入拆帧、坏 CRC 丢弃重同步、0x02 帧尾示例。

## 真机填实清单（拿到板子后逐项勾）

- [ ] CanMV IDE 联机、sensor 例程出图；记录固件版本
- [ ] `config.py`：UART_ID/引脚、`sensor` pixformat 硬件 JPEG 是否可用（`JPEG_HW`）、`capture_jpeg()` 的 compress/bytes 语义
- [ ] 预览流实测：单帧 JPEG 大小 8~15KB（调 `JPEG_QUALITY`）、≥5fps、10 分钟稳定
- [x] 识别路径 A 代码就位：`main.py` INFER_MODE="A" 已接入 DNK210 双模型（`参考代码-车牌识别实验/` 提供的 `lp_detect.kmodel`/`lp_recog.kmodel`/`lp_weight.bin`，YOLOv2 检测 + 逐位识别，输出 UTF-8 中文车牌如 粤B12345）
- [ ] 识别路径 A 真机验证：SD 卡建 `/sd/KPU/` 拷入三模型文件 → 上电看 IDE 串口 `[KPU] models ready`；对车牌拍照看每 3s `K2:OK:{...plate...}`；漏检调 `KPU_DET_THRESHOLD`，字符反了调 `RECOG_HMIRROR`/`CAM_*FLIP`
- [ ] 触发联调：0xC1 → 一次 0xC2 或（0x01/0x02 用途=1 JPEG + 0xC3）
- [ ] 波特率实测定值（921600 误码则回退，记录于 P5-14）

## 待办 / 已知占位

- 参考例程注明需 **Lite 版 CanMV 固件**（双 KPU 模型 + 权重吃内存）：板载 makerobo 固件若 `[KPU] load fail` 且报缺方法（`init_yolo2`/`regionlayer_yolo2`/`lp_recog`/`pix_to_ai`），需换带 DNK210 KPU 扩展 API 的 CanMV 固件。**✅ 2026-09-11 实测：板载固件已带全部这些 API（`missing: []`），此担心排除。**
- `capture.py` 与 `infer.py` 的多文件版仍为结构占位（标注 TODO）；**当前实机入口是单文件 `main.py`（已含全逻辑）**。
- 本地仅生成 .py 文本，无需交叉编译；固件本体升级才用 kflash（出厂一般已带 CanMV）。

## 单机联调记录（2026-09-11，K210 只连 PC，无 MP157）

### 板子 / 固件事实
- 固件：`MicroPython v1.0.4-22-g0f1e00b-dirty on 2023-02-14; CanMV_Board with kendryte-k210`；资料包里的对应镜像是
  `E:\download\k210\2.开发环境\CanMv K210固件\创乐博（makerobo）canmv-K210固件 2023-2-13.bin`（**用 kflash_gui 重刷后行为/横幅完全一致**）。
- **车牌 KPU 扩展 API 齐全**：`load_kmodel / init_yolo2 / run_with_output / regionlayer_yolo2 / lp_recog / lp_recog_load_weight_data` → `hasattr` 探测结果 `missing: []`。**不需要另找"Lite 固件"。**
- `/flash/config.json`（341B，固件读它做板级初始化）：
  ```json
  {"type":"makerobo","kpu_div":1,
   "sdcard":{"cs":29,"mosi":28,"sclk":27,"miso":26},
   "board_info":{"BOOT_KEY":16},
   "freq_cpu":416000000,"freq_pll1":400000000,
   "lcd":{"width":320,"height":240,"dir":160,"dcx":38,"ss":36,"rst":37,"clk":39,
          "offset_x1":0,"offset_x2":0,"offset_y1":0,"offset_y2":0,"invert":0}}
  ```
  ⇒ 固件是 makerobo 定制版，**SD 引脚就写在配置里**（SPI：cs29/mosi28/sclk27/miso26，对应原理图 `SD_CS/SD_MOSI/SD_SCLK/SD_MISO`）。
- `/flash` 三个文件：`freq.conf`(16B)、`config.json`(341B)、`main.py`。**前两个是固件配置，不要删。**

### ⛔ SD 卡：这台机器上始终挂不上（未解决）
- 现象：卡插槽内、**FAT32**、模型已按厂家约定放成 `F:\KPU\{lp_detect, lp_recog, lp_weight}`，**冷启动**后
  `uos.listdir("/")` 恒为 `['flash']`，`uos.listdir("/sd")` / `uos.statvfs("/sd")` 报 **`[Errno 19] ENODEV`**。
- 已排除：① 路径名（`main.py` 现已自动在 `/sd/KPU`、`/sd`、`/flash/KPU`、`/flash`、`/` 五处找）；② 卡落位；③ FAT32 格式；
  ④ 固件构建（重刷厂家镜像无效）；⑤ Python 手动挂载（`machine.SDCard` 是**空壳类**，实例化报
  `TypeError: cannot create 'SDCard' instances`；`uos.mount/umount` 存在但没有块设备对象可挂）。
- 厂家**自己的例程**大量直接用 `/sd`（实验17 读 `/sd/PICTURE/`、20 读 `/sd/MUSIC/`、21 写 `/sd/RECORDER/`、22 写 `/sd/PHOTO/`、23 读 `/sd/VIDEO/`）
  ⇒ 设计上这块板子的 SD 应当是**开机自动挂载**的。
- **仍未试**：(a) 换一张卡（尤其小容量老卡）；(b) 卡是否插到底 / 触点是否脏；(c) `help('modules')` 找 `sdcard`/`storage` 类模块；(d) 改 `config.json` 里的 sdcard 引脚后冷启动试探。

### 内部 flash 备选（Plan B）
- `uos.statvfs("/flash")` = `(131072,131072,21,21,21,0,0,0,0,128)` ≈ **2.6MB**；三模型合计 **2,536 KiB**，**极紧**。
- 能否装下必须用**实写探测**：循环写 `/flash/probe.bin` 到异常为止，再删。
- 上传手段：PC 侧 Python 3.14 **无 pip、无 pyserial** ⇒ 用 PowerShell 的 `[System.IO.Ports.SerialPort]` 直接开 COM 口灌数据（无需装包）；
  或先试 CanMV IDE 自带的文件管理功能（**已确认 IDE 能删除设备上的文件**）。

### CanMV IDE 的一个大坑
- **IDE 点"运行"是流式执行编辑器里的脚本，不写盘** ⇒ "正在运行的代码" 与 "开机自启的 `main.py`" 可能完全不同版本。
  实测 `/flash/main.py` = **15276B**，而我们当前版本是 **40843B**、厂家例程是 3523B。
- 因此改动后要用 IDE 的**"保存到设备"**落到 `/flash/main.py`，再确认 `uos.stat("/flash/main.py")[6]` 等于本地文件大小。

### 方向（镜像）问题的结论
- 症状：**水平镜像（左右反），竖直正常**。
- **传感器层的 `sensor.set_hmirror/set_vflip` 在这套固件上实测无效**（开了 `CAM_HMIRROR=True` 画面依旧镜像，排查模式四个组合画面也不变）。
- 方案：改为**软件层翻转** `CAM_SW_HMIRROR=True`（`capture_frame()` 里 `img.replace(hmirror=True)`，参考例程同款用法，纯像素操作必然生效），
  位置在一切消费之前 ⇒ 板载屏 / 上行 JPEG / 检测 / 识别看到的是同一份已修正画面。`[BOOT]` 横幅会打印 `sw_vflip/sw_hmirror`。
- 显示端（`core1_ui/qt_gui/src/k210_link.cpp`）已改成 `K210_VIEW_HMIRROR=0`（不变换，marker `K210-ORIENT-NONE`），**固件修好方向后显示端不该再翻**；仓库里的 `bin/park_ui` 仍是旧的 HMIRROR 版本，待板子回来重编。

### 下次继续（优先级）
1. 跑一次 **`/flash` 实写探测**拿到真实容量 → 决定"模型进内部 flash"是否可行。
2. 若可行：用 PowerShell 串口脚本把三个模型灌进 `/flash`（`main.py` 已自动认 `/flash`），冷启动看 `[KPU] model dir=/flash ... models ready` → `[RECOG] det=... OK plate=...`。
3. 若不可行：继续查 SD（换卡 / 查卡槽 / `help('modules')` / 改 `config.json` 引脚）。
4. 用 IDE"保存到设备"把当前 `main.py`（40843B）落到 `/flash/main.py`，保证开机自启的是最新版。
