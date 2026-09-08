# m4_fw RPMSG —— Linux(A7/Core0) ↔ M4 OpenAMP/RPMSG 通讯（第3步，M4 侧）

> 对端：A7-Linux `core0_service/rpmsg/`（协议权威定义 `docs/protocols.md` §3，母本 PhaseMd/10 §2）。
> M4 角色 = **CAN↔RPMSG 网关**：A7 的 0x11/0x12/0x13 下行 → CAN 0x100；C8T6 的 0x200/0x210
> 上行 → 0x21/0x22 转发 A7；状态查询 0x13 → 0x23。进度：**代码就绪（2026-09-10），
> 待 CubeMX 勾 OPENAMP Regenerate 后可编译；板端联调待做**（验收门 G3）。

## 1. 文件地图（M4 侧）

| 文件 | 职责 | 来源 |
|---|---|---|
| `CM4/Core/Inc/rpmsg_types.h` | 协议常量/结构（type、payload 上限、上行布局） | `core0_service/rpmsg/rpmsg_types.h` **逐字节拷贝副本**（单一事实源在 A7） |
| `CM4/Core/{Inc,Src}/rpmsg_proto.{h,c}` | CRC16-XMODEM、组帧、增量拆帧状态机（粘包/坏帧重同步/seq） | 同上，A7 拷贝副本（含 decode 死代码便于两端 diff） |
| `CM4/Core/{Inc,Src}/rpmsg_bridge.{h,c}` | **网关桥业务**：`Rpmsg_Task`（OpenAMP 单任务模型）+ 事件队列 + 心跳/0x22/0x23 + Bus_Off 恢复 | 手写，独立于 CubeMX regen（放 Core/，生成物不覆盖） |
| `CM4/OPENAMP/`（CubeMX 生成） | openamp.{c,h} / openamp_conf.h / rsc_table.{c,h} / mbox_ipcc.{c,h} / openamp_log.{c,h} | 勾选 Middleware→OPENAMP 后 Regenerate 生成，**勿手改** |
| `tools/load_m4.sh`（A7 侧） | remoteproc start/stop/status + 等 `/dev/ttyRPMSG0`；**start 前幂等释放 FDCAN2** | core0_service/tools/ |

## 2. CubeMX 步骤（用户操作，一次性）

1. CubeIDE 打开 `m4_fw.ioc`（M4 视图）。
2. **Middleware → OPENAMP → Mode: OpenAmp_Activated**（虚拟外设 `VP_OPENAMP_VS_OPENAMP`）。
   勾选自动带出：IPCC 外设 + NVIC `IPCC_RX1/TX1_IRQn` + hal_conf 使能 + 生成上面 `CM4/OPENAMP/` 8 文件，
   并在 main.c 插入 `MX_IPCC_Init(); MX_OPENAMP_Init(RPMSG_REMOTE, NULL);`（USER CODE 区外，regen 稳定）、
   it.c 插 IPCC 两个 IRQHandler、.cproject 自动加 include。
3. 确认 FreeRTOS 堆 **32768**（已随代码同步：`FreeRTOSConfig.h` + `m4_fw.ioc`）。
4. **Regenerate** 后先编译：`rpmsg_bridge.c` 顶部有
   `#error "openamp.h not found ..."`——生成 OPENAMP 前编译故意失败，属预期哨兵。

**生成后核查清单**：① `MX_OPENAMP_Init` 在 `osKernelStart()` 之前被调；② it.c 有
`IPCC_RX1/TX1_IRQHandler → HAL_IPCC_RX/TX_IRQHandler(&hipcc)`；③ `rsc_table.c` vring 地址为 -1
（`LINUX_RPROC_MASTER`：**vring/buffer 由 Linux 端分配**，M4 不硬编码）；④ hal_conf 使能
`HAL_IPCC_MODULE_ENABLED`/HSEM。

**Fallback（无 OPENAMP 勾选项或生成物不全）**：从 FW 包手工导入中间件
`C:\Users\iosran\STM32Cube\Repository\STM32Cube_FW_MP1_V1.7.0\Middlewares\Third_Party\OpenAMP\{open-amp\lib,
libmetal\lib, virtual_driver\virt_uart.c, mw_if\...}`，参考 100ASK 例程
`E:\download\100ASK-MP157\100ASK_STM32MP157_M4_Code\22_A7_M4_UserModeComm\rpmsg_user\` 的工程组织
（中间件放 `Middlewares/Third_Party/OpenAMP/`、生成物放 `CM4/OPENAMP/`、.project 逐文件 link + 三处 include）。

## 3. 软件模型（bridge 已实现，无需再写）

- **线程**：`Rpmsg_Task`（512 words, Normal，freertos.c RTOS_THREADS 区创建）为唯一 OpenAMP 用户：
  2ms 轮询 `OPENAMP_check_for_message()`；VIRT_UART RX 回调**只 memcpy+置标志**（禁 FreeRTOS API）。
- 下行：0x11/0x12 → `CAN_Master_RequestCmd(0x01/0x02)`；0x13 → 回 0x23；0x7E 保鲜；未知丢弃计数。
- 上行：CAN 事件经 can_master 新钩子 `CAN_Master_SetEventHook` 入队（深16，满丢）→ 0x21（17B，
  id u32 LE + dlc + data[8] 原样 + tick u32 LE）；C8T6 在线边沿 → 0x22；1s 维护查
  `HAL_FDCAN_GetProtocolStatus` Bus_Off → Stop/Start 恢复 + `bus_off_cnt` 并入 0x23 的 can_err。
- 发送 per-type seq、失败重试 2 次后丢弃计数；0x7E 双向 500ms；A7 侧 1s 无帧判 LINK_DOWN 由其自理。
- 监视：`g_rpmsg_bridge_mon`（vuart_ready/a7_alive/tx/rx/evt/hb/…，调试器 live watch）。

## 4. 板端流程（A7 侧，用户执行）

```sh
# 1) 加载 M4（脚本已含 FDCAN2 释放：can0 down + unbind 4400f000.can）
cp <构建的 m4_fw.elf> /lib/firmware/m4_fw.elf
sudo ./core0_service/tools/load_m4.sh start     # 等到 /dev/ttyRPMSG0

# 2) 编译并跑 A7 demo（收到 M4 0x7E 心跳/应答）
cd core0_service && make
sudo ./rpmsg_demo          # q=查询(→0x23) o/c=开/关闸(端到端到 SG90)

# 3) 坏帧/断链/长稳（P3-07）
sudo ./tools/rpmsg_cli/rpmsg_cli    # bad 21 注入坏 CRC → 计数、链路不崩
```

**首次启动主要风险点**：内核 CONFIG_RPMSG_TTY（→ /dev/ttyRPMSG0）、remoteproc 支持、
vring 保留内存（`ls /proc/device-tree/reserved-memory/` 找 vdev0vring0/1+vdev0buffer）；
缺则 dmesg + `/proc/config.gz` 排查（PhaseMd/13 链条），必要时设备树覆盖。

## 5. 验收（G3 摘要）

- `q` → 0x23 打印 gate/node_online/can_err；`o`/`c` → SG90 实际动作；
- 手遮 BH1750 → 0x21 ev/lux/drop 上行、C8T6 OLED `CAN:Sended`（0x110 不回归）；
- 断链：remoteproc stop → A7 ≤1s LINK DOWN；start → 自动重连 + 0x13 → 0x23；
- C8T6 断电 ≤3s → 0x22 离线；恢复 → 0x22 在线；
- `rpmsg_cli bad` 坏 CRC 注入不崩；10 分钟 soak 零丢计数 → 勾 G3。

## 6. 同步纪律 / 备注

- 协议改：母本 PhaseMd/10 §2 → `docs/protocols.md` §3 → A7 原件 → M4 副本 → 两处变更记录。
- regen 安全：rpmsg_types/proto/bridge 放 `Core/`，CubeMX 生成物放 `CM4/OPENAMP/`，互不覆盖；
  bridge 用 `__has_include("openamp.h")` 做生成哨兵（未生成编译即失败提示）。
- M4 持 FDCAN2 期间 A7 不得用 can0（load_m4.sh 已固化释放）；A7 要收回 can0 需 rebind/重启。
- 板端粘贴脚本保持纯 ASCII（用户既定规范）；仓库 .c/.md 不受限。
