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
- [ ] 识别：路径 A 准备/转换车牌 kmodel（参考正点原子 DNK210 CanMV·车牌识别实验，nncase 转换见 P5-06）或维持路径 B（仅检测→云兜底）
- [ ] 触发联调：0xC1 → 一次 0xC2 或（0x01/0x02 用途=1 JPEG + 0xC3）
- [ ] 波特率实测定值（921600 误码则回退，记录于 P5-14）

## 待办 / 已知占位

- `capture.py` 与 `infer.py` 的硬件/模型部分为**结构占位**（标注 TODO），协议层已定稿可独立测试。
- 本地仅生成 .py 文本，无需交叉编译；固件本体升级才用 kflash（出厂一般已带 CanMV）。
