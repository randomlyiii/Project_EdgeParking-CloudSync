# AGENTS.md history archive

Lines moved out of AGENTS.md on 2026-09-11 because the workspace-instruction
injection budget is 65536 bytes and AGENTS.md had reached it (the tail was
being truncated, i.e. silently lost). Nothing was rewritten: the blocks below
are byte-identical to what used to live in AGENTS.md.

These are RESOLVED / SUPERSEDED records kept for lookup - read them when you
need the full reasoning behind a past decision.

---

- **⚡ K210 预览帧率优化（2026-09-07）**：真机 CanMV IDE（固件 1.0.4）sensor 出图正常但 FPS 低；瓶颈=纯 Python 逐位 CRC16（每帧 JPEG 10~20KB 全量 ×8 次内层循环，估算秒级/帧）+ 每轮 gc.collect + IDE 帧缓冲回传限速。已改 `uart_proto.py` 与 `main_single.py`（两版同步）：crc16 优先 `binascii.crc_hqx`（C 实现，XMODEM 同参，**结果不变→协议不破**）+ 256 项查表回退；主循环 gc 改 2s 低频；新增每 5s `[stat] fps/jpeg大小/mem_free` 串口打印。**`tools/selftest.py` 首次真跑（沙箱有 Python 3.13），暴露并修复 test_roundtrip 漏收首次 feed 返回值的测试 bug**（快路径全过 + meta_path 拦 binascii 强制回退路径 200 组随机数据等价验证过）。IDE 帧缓冲回传本身限速，验收帧率以脱离 IDE 独立运行为准（README ≥5fps）；CanMV 的 `binascii` 若被裁剪则自动走查表，板上需看 `[stat]` 实测。**板载屏预览已加（2026-09-07）**：`LCD_PREVIEW` 开关（config.py/main_single.py），`lcd.init/display` API 已由用户实机跑官方 Hello World 例程确认；**`lcd_display` 必须在 `compress()` 之前调（compress 原地改写 img）**——capture 已拆成 `capture_frame()/lcd_show()/jpeg_from()` 三函数（单文件版与 capture.py/main.py 同步），`capture_jpeg()` 保留为抓拍包装（不经屏）；JPEG_HW 与 LCD_PREVIEW 勿同开（lcd 不能直显 JPEG），lcd.display 约 20~30ms/帧会拉低 fps。
- **⭐ A7 侧预览接收端已建（2026-09-07）**：`core1_ui/k210_link/k210_preview_rx.py`（python3 纯标准库：收 /dev/ttyACM0 921600 → 拆帧/CRC16-XMODEM/重组 0x01+0x02 → 落盘 /tmp/k210_frame.jpg；无 pyserial 依赖）+ `core1_ui/k210_link/README.md`（cdc/uart 两链路步骤、K210 存 main.py 自启、显示命令）。**链路已定 cdc**（K210 USB→MP157 → /dev/ttyACM0，用户确认）；MP157 LCD 同时有 `/dev/fb0` 与 `/dev/dri/card0`，先按 **/dev/fb0 + fbi** 显示（fbi 需确认已装，未装则 apt install fbi）。JPEG_QUALITY 已 88→65（main_single.py/config.py）；若实测 `[stat] jpeg=` 仍 >100KB 则为未压缩 raw → 改 `JPEG_HW=True`。
- **⭐⭐ K210→MP157 链路验证通过（2026-09-07，"成了"）**：本固件(makerobo CanMV-K210 / CMVBlock，固件 v1.0.4)实测 **machine.UART 二进制帧到不了 USB**（hexdump 只见到 console 文本、无 aa55）；USB(CH9102→UART0)只可靠承载 **console(print) 文本**。**最终方案 = console 文本传图**：K210 `main.py`（LINK="console"）把 JPEG base64 分行走 print（`K2:IMG:<off>:<b64>` … `K2:END:<len>`），MP157 `core1_ui/k210_link/k210_preview_rx_text.py` 解析重组存 `/tmp/k210_frame.jpg` → `fbi -a -d /dev/fb0` 上屏。QVGA 一帧约 1~2s（115200），零杜邦线、必通。其余关键事实：sensor.JPEG 报不支持→软件 compress；QQVGA(160x120)显示到 320x240 LCD 会"花纹"→用 QVGA 320x240+helloworld_1.py 同款初始化(lcd 先于 sensor+skip_frames)屏才干净；os.dupterm 只有 index 0 有效（index 1 报 invalid）；现走 `k210_fw/main.py` 单文件（用户已清掉模块版），另有 `k210_fw/debug_boot.py`、`helloworld_1.py`；`E:\download\k210` = makerobo K210 全资料（固件 bin/GPIO 图/原理图）。正式二进制协议(UART 帧)仍需 2~3 根杜邦线走 MP157 ttySTM1（预留）。
- **🔧 M4 代码审计结论（2026-09-08，A7 直连全通后按用户要求复查）**：① **位时序按 100MHz 设计而板实测 62.5MHz → M4 实际波特率=312.5k，与 C8T6 必不通**（fdcan.c Prescaler=25/Seg1=5/Seg2=2 是 8tq@100M=500k 的配法；62.5M 下需 5tq=Prescaler25/Seg1=3/Seg2=1）；② fdcan.c `AutoRetransmission=ENABLE` 与 can_master.h 注释"NART=DISABLE 单发不重传"自相矛盾（设计=单发，应改 DISABLE，.ioc 同步）；③ MspInit 的 `IS_ENGINEERING_BOOT_MODE()` 段会把 FDCAN 时钟切到 **PLL3Q（当前未使能）** → 工程模式下 init 会失败/无时钟，应跳过该切换直接用 boot 默认源（62.5MHz）；④ 注释 FDCAN1 字样/位时序数字与代码不符（改名残留）。**用户实测认识：A7 用过 FDCAN2 后必须释放（`ip link set can0 down` + unbind `4400f000.can`，把驱动与时钟门全放掉）M4 才能独占**；A7 要再用回需 rebind 或重启。**已按用户选的方案 A 修正落地（同日）**：`fdcan.c`（Seg1=3/Seg2=1@62.5MHz、AutoRetransmission=DISABLE、去掉 PLL3Q 切换段）、`m4_fw.ioc` 同步、can_master/freertos 注释 FDCAN1 残留清除、`docs/protocols.md` §1 位时序行更正。**⚠️ 2026-09-08 二次更正（重要）**：M4 两种运行环境 FDCAN 时钟不同——**工程模式(CubeIDE 调试)=PLL3Q 100MHz**（.ioc RCC 段 `FDCANFreq_Value=100000000` + main.c 工程 SystemClock），**Linux 引导/remoteproc=62.5MHz**。按 62.5MHz 的静态改法在工程模式反成 800k=不通；已改为 **运行时 Autotune**（`HAL_RCCEx_GetPeriphCLKFreq` 实测→自动算 500k 位时序，watch 变量 `g_fdcan_meas_hz/g_fdcan_cfg_pre/seg1/seg2`），恢复工程模式 PLL3Q 选择段，静态默认回 100MHz 配置（25/(1+5+2)=75%）。另：Linux 下裸寄存器发帧脚本（m_can 驱动配好后写 TX FIFO@0xCC 触发）实测可通，佐证 MRAM 在 0x44010000（驱动 TXBC 给出的字偏移）。
### M4 侧 RPMSG 网关（第3步 P3-01~P3-04，2026-09-10 代码写一半暂停，用户喊停）
- **📍 进度快照（下次从这里续）**：已完成 3/9 步。① 协议副本 3 份已拷：`CM4/Core/Inc/rpmsg_types.h`、`Inc/rpmsg_proto.h`、`Src/rpmsg_proto.c` = A7 侧 `core0_service/rpmsg/` 同名文件**完整拷贝**（decode 死代码保留为的是两端可 diff；单一事实源在 A7 侧：改协议走 母本§3→A7→本副本→两处变更记录）；② `CM4/Core/{Inc,Src}/rpmsg_bridge.{h,c}` 已写完主体：`Rpmsg_Task`（栈 512 words、2ms 轮询 `OPENAMP_check_for_message`；vuart 回调只 memcpy+置标志**禁调 FreeRTOS API**）→ 拆帧分发（0x11/0x12→`CAN_Master_RequestCmd`，0x13→置标志回 0x23，未知丢弃计数）+ 500ms 0x7E + 事件队列(深16)→0x21 + 1s 维护（online 边沿→0x22、Bus_Off 恢复）；**全部 OpenAMP 交互收敛在 Rpmsg_Task 单任务**（OpenAMP 非线程安全）；发送 per-type seq、重试 2 次丢弃计数；监视快照 `g_rpmsg_bridge_mon`；开头 `#if !__has_include("openamp.h") → #error`（未 regen 编译故意失败并提示）。**③⚠️ bridge 有一处待修正**：`periodic_check` 的 Bus_Off 用 `hfdcan2.Instance->PSR & FDCAN_PSR_BO` 直读——已查实 MP1 HAL **有** `HAL_FDCAN_GetProtocolStatus`（`FDCAN_ProtocolStatusTypeDef.BusOff` 字段）与 `HAL_FDCAN_GetErrorCounters`（TxErrorCnt/RxErrorCnt/CELU），续作时改 HAL API（直读也能编译，纯风格统一）。
- **⏭️ 待办（按序）**：① 上面的 PSR→HAL API 小改；② `can_master.{c,h}` 加 `CAN_Master_SetEventHook(fn)`——Poll 收帧循环里（DLC 校验后、switch 前）回调 `(id, dlc=(uint8_t)(rh.DataLength>>16), data, now)`，bridge 的 can_evt_hook 注册后入队；③ `freertos.c` USER CODE：Includes 加 rpmsg_bridge.h + RTOS_THREADS 建 `Rpmsg_Task`（512 words，main.c 不用动）；④ 堆 8192→**32768**（FreeRTOSConfig.h + .ioc 同步，对齐 ST 例程）；⑤ 写 `m4_fw/rpmsg.md`（bringup 文档，仿 c8t6/can.md：CubeMX 勾选步骤/fallback/联调验收）；⑥ `core0_service/tools/load_m4.sh` 加幂等 CAN 释放（start 前 `ip link set can0 down` + unbind `4400f000.can`）；⑦ 文档同步（PhaseMd/04 P3-01~04 状态、protocols.md §3 状态行、core0_service/rpmsg/README）+ AGENTS.md + git 提交。**✅ 待办①~⑥ 已完成（2026-09-10）**：① Bus_Off 直读 PSR → `HAL_FDCAN_GetProtocolStatus(pst.BusOff)`；② can_master 新增 `CAN_Master_SetEventHook`（Poll 帧校验后回调 id/dlc/data/now，bridge 注册入队）；③ freertos.c RTOS_THREADS 建 `Rpmsg_Task`(512w, Normal)；④ 堆 8192→**32768**（FreeRTOSConfig.h + .ioc）；⑤ 新建 `m4_fw/rpmsg.md`（CubeMX OPENAMP 步骤/核查清单/fallback/板端验收）；⑥ `load_m4.sh` start 前幂等释放 FDCAN2；⑦ 本次完成。**已提交（本地）：`8ba7f85` feat(m4_fw/rpmsg) + `49519ee` docs**。**剩余（需用户）：CubeMX 勾 Middleware→OPENAMP Regenerate → 编译（bridge `__has_include("openamp.h")` 哨兵解除）→ 板端联调 G3（m4_fw/rpmsg.md §5 验收）**。
- **✅ RPMSG 链路打通（2026-09-10 板验两次全通，G3 传输层通过）**：根因链分两段——(1) M4 端点创建改"任务启动 1s 首试+失败每 2s 重试"（rpmsg_bridge.c/h 补丁，mon.vuart_init_rc/attempts 可见），解决首个 NS announce 被吞（旧症状：mbox IRQ 恒 1、无 channel）；(2) **5.4 BSP 双怪癖**：NS 通道不自动绑 rpmsg_tty，**且直接 bind 报 "No such device"（5.4 rpmsg id 匹配 bug），必须先 `echo rpmsg_tty > $d/driver_override` 再 bind 才生效**——load_m4.sh `bind_channel()` 已按此修正（先 override 后 bind，幂等）。**启动 recipe**：`echo start` → ~2s 后 dmesg `creating channel rpmsg-tty addr 0x400` → override+bind → /dev/ttyRPMSG0。板验证据：dyn debug 可见内核完整收到 NS 帧（"rpmsg-tty"+addr 0x400 逐字节正确）→ creating channel → Received；python3 验收两次通过（hb~9/6s、0x13→0x23×3、CRC 0 错）。**⚠️ M4 重启 flaky（未根治，实测成功 2/9）**：channel 建立时好时坏，失败特征=mbox IRQ 78 恒 1（M4 首 kick 后内核无后续处理，dyn debug 下连 NS 帧都没有）；已证伪：缓冲地址假说（成功/失败 kernel buffers dma 都是 0xd8042000 DDR）、冷/软重启差异；粗略规律：静默失败会话**静坐几分钟后再 stop/start 更易成**（118→786 隔 668s、37→350 隔 311s 均成），活跃会话停止后立刻重启基本必败（786→2005 败），失败会话快速重启也败（87~160s 间隔）。**开发建议**：M4 起来后勿频繁重启；重启失败→stop→等 2~5 分钟→再 start，或直接重启系统；dyn debug 方法留档（`echo 'file drivers/rpmsg/virtio_rpmsg_bus.c +p' > /sys/kernel/debug/dynamic_debug/control` 可看到 NS 帧/收包计数）。**遗留**：① C8T6 端到端（0x11/0x12→CAN 0x100→SG90、手遮→0x21、断链 0x22）；② core0_service C demo 板端无 make/gcc——验收用 python3 heredoc，正式 A7 服务待交叉编译方案（100ASK SDK 在 E 盘资料或换镜像）；③ 用户提及共地线疑似松动——C8T6/CAN 联调前先查接线与供电。
- **⚠️ 沙箱无 Linux C 编译器**：`make host-check` 与板端编译未执行，代码未经编译验证；板端流程 = `sudo ./tools/load_m4.sh start` → `make`（或 `make CROSS_COMPILE=arm-openstlinux-linux-gnueabihf-`）→ `sudo ./rpmsg_demo`。
- **✅ OLED 黑屏根因已解 + 屏点亮（2026-09-06，v2~v6 诊断留档）**：现象=屏黑但 PC13 心跳闪（固件/时钟/bit-bang 全正常）。**根因 = `SSD1306_Init` 只发命令字节、从不发参数字节**（旧 `cfg[][2]` 表只取 `cfg[i][0]`）→ `0x8D`(电荷泵) 把下一条命令当参数 ⇒ 内部升压被关、面板无 VPP 全黑，而 I2C 应答完全正常（完美解释"硬件 I2C 黑 + 软件 I2C 黑 + Keil 正常 + 探测有 ACK"）。**修复**：init 改逐字节平铺序列（照抄参考工程 `OLED_Config`：AE/D5 80/A8 3F/D3 00/40/A1/C8/DA 12/81 CF/D9 F1/DB 30/A4/A6/8D 14）+ 尾部先 Fill+UpdateScreen 再 `0xAF` + 上电忙等 20-30ms。**经验**：诊断期把线检/探测细分成数字码（`g_DiagCode`，6=总线健康+OLED 在线）极有效；`SW_I2C_Probe` 未做总线空闲电平防护，坏屏钳低时会假 ACK；`configASSERT` **开启**（断言失败静默死循环）；Keil 工程 72MHz 已确认，排除时钟差异。
- **✅ 屏已点亮（2026-09-06）+ 显示 API 已换成参考工程同款**：v6 修复 init 后 HelloWorld 上屏成功。显示层重做：新增 `Core/Inc/oled_font8x16.h`（8x16 字模 95 字符），`ssd1306.{c,h}` API 改为 `SSD1306_ShowChar/ShowString(line 1~4, column 1~16)`（与 Keil `OLED_ShowString` 同语义），删除旧 6x8 字体/SetCursor/WriteString/WriteStringPadded；内部仍是 1024B 影子显存 + `UpdateScreen()` 整屏上屏。`freertos.c` 布局宏改为 `OLED_LINE_TITLE/LUX/GATE/CAN`（1/2/3/4 行）。
- **⭐ 字模表已换成双重验证的标准江协表**：用户反馈 w/o/r"缺顶"+屏上多出感叹号。逐字节比对结论：①Keil 参考工程的 `OLED_Font.h` 是**被改过的残缺变体**——与标准江协表差 15 字节，真损坏的是 `"` `'` `,` `;` `>` `@` `m`(byte-shift) `n` `~`，但 **H/e/l/l/o/W/o/r/l/d 与标准完全一致**；②标准表已交叉验证：qlqqs/STM32G4-OLED-SSD1306-I2C-HAL 与 MrWei95/STM32-OLED-LL-Library-Demo（jsdelivr CDN 可拉，raw.githubusercontent 时断时续）两份独立源逐字节一致，已重新生成 `oled_font8x16.h`。③标准字形事实：大写占 rows 3-13，小写 x-height（o/e/r 等）只占 **rows 7-13（7px）**——"缺顶感"是江协字体本身的比例，不是 bug；HelloWorld 全部字形无 rows 14-15 像素（渲染脚本曾误报）。④**"第四行的感叹号"未定位**：代码只写 line 1、字模无底部碎片，待用户拍照确认（或用户自己改过字符串/位置）。渲染校验工具：awk+rshift 解析 `oled_font8x16.h` 逐行打印点阵（注意 gawk 无 `>>` 运算符，数组是 1-based row=idx+1）。
- 诊断三件套（保留，确认稳定后可拆）：①`main.c` USER CODE 2 调度器启动前裸机点屏 + 自检 + 探测，结果存 `g_DiagCode/g_DiagLine/g_DiagProbe`；②`defaultTask` PC13 LED 闪 g_DiagCode 次（6=总线健康+OLED 在线）；③`sw_i2c.c` 每边沿 ~1-2µs 延时、`SW_I2C_SendByteAck/SW_I2C_Probe/SW_I2C_LineCheck`（开漏下读 IDR 取引脚真实电平）。
- **✅ BH1750 已接入共享总线（2026-09-06，按 `MD文档/oled_standard.md` 规范，参考 `参考资料/bh1750.c/.h`）**：`BH1750Task` 已取消挂起并与 OLED 集成——①`bh1750.c` 升级：Init 采用参考工程验证序列（POWER_ON→10ms→RESET→10ms→CONT_HRES→180ms 等首次测量，阻塞约 200ms）；地址字节用 `SW_I2C_SendByteAck` 做 **NACK 检测**（器件掉线即报错，`BH1750_ERR_VALUE` 哨兵路径从死代码变为生效）；②**总线互斥升级**：`OledMutexHandle` 语义扩为"软件 I2C 总线互斥"，BH1750Task 的 Init/读数与 OLED_Task 的 Init/整屏刷新全部持锁，杜绝两任务并发 bit-bang 插坏事务；③**自愈**：连续失败 25 次（≈5s，`BH1750_REINIT_FAILS`）自动重发 Init 序列，支持传感器后插上/掉电恢复；采样周期宏 `BH1750_READ_PERIOD_MS=200` 进了 bh1750.h（未来 config.h 可收拢）。④OLED_Task 栈 **256→384 words**（oled_standard.md 6.3 实测教训：sprintf+刷屏任务 <384 会爆栈），`freertos.c` 与 `c8t6.ioc`（FREERTOS.Tasks01）已同步，**CubeMX regen 后需确认未回退**。⑤界面：行1 保留 HelloWorld + 行2 `Lux:%u`/`Lux:ERR`（snprintf 限 16 列，oled_standard.md 6.2），Gate/CAN 行随 SG90/CAN 阶段再加，刷新周期 200ms。`main.c` 裸机诊断段未动。**待烧录验证**：接 BH1750 后应见 Lux 实时数值，拔传感器显示 Lux:ERR，重插 ~5s 自愈。
- **待办（C8T6，截至 CAN 阶段后）**：① 编译烧录验证 = 行4 CAN 状态 / 行3 Gate 翻转 / 手遮出 `CAN:EVT`（事件帧），对端(M4 或分析仪)抓 0x200/0x210 字段；② 遮光参数按现场调 `config.h`；③ SG90（TIM2 PWM）后加，把 `CAN_Node_GateOpen()` 接到 CCR 目标；④ 收尾：拆除 main.c 裸机诊断三件套 + defaultTask 闪码改心跳灯（P1-03，已于 2026-09-08 完成，见下条）。
- **✅ C8T6 已收尾回"正常模式"（2026-09-08）**：联调诊断三件套已拆——main.c 裸机诊断段（LineCheck/Probe/g_DiagCode）移除、defaultTask 闪码改 PC13 1Hz 心跳灯、OLED 由 "CAN DBG"(R/T/RX/G) 页恢复正式四行界面（行1 HelloWorld / 行2 Lux+D% / 行3 Gate:OPEN\|CLOSE / 行4 CAN:OK\|EVT(1s闪)\|ERR）；can_node.c 帧二次校验（0x100 标准帧 DLC=8）恢复；**F1 CAN_ESR 位段勘误 REC=[23:16]/TEC=[15:8]/LEC=[6:4]/BOFF=[2]**（旧 >>24/>>16 读取致 REC 恒 0 假象）。经验已入 `MD文档/can_standard.md` §7「停车场落地记录」（原 §7 相关文档顺延 §8）。
- **⭐ 新增 0x110"事件确认"握手（2026-09-08）**：M4 主端每收到 C8T6 的 0x200（事件或查询应答）自动回 **0x110 确认帧**（d[0]=回显事件码，`can_master.c` `CAN_Master_SendAck`，mon `tx_ok_count` 同步计数）；C8T6 收 0x110 记 `s_ack_tick`（`CAN_Node_LastAckTick()`），**OLED 行4 显示特殊标识 `CAN:*OK*` 约 1s**（`OLED_ACK_FLASH_MS`）；行4 状态集 = `CAN:OK` ｜ `CAN:EVT`(已发未确认) ｜ `CAN:*OK*`(板子已确认) ｜ `CAN:ERR`。协议已同步：PhaseMd/10 母本 → `docs/protocols.md` §1 → `c8t6/can.md` §4.1/§4.2a → 双端代码。**⚠️ 提交状态（2026-09-08）：用户要求撤销 31b7982 之后的提交**（`git reset --mixed 31b7982`，3 个 ack 提交已摘除），**代码改动保留在工作区未提交**，等双端（C8T6 + M4 带 `CAN_Master_SendAck`）重编烧录后真机验证 `CAN:*OK*` 再重新提交。**待双端重编烧录真机验证**（手遮 → C8T6 行4 出 `CAN:*OK*`）。**📌 2026-09-08 收工**：用户已把行4 特殊文案改为 `CAN:Sended`（`c8t6/Core/Src/freertos.c` 工作区本地改动，未提交，**勿覆盖**）；真机仍只出 `CAN:EVT`/`CAN:OK` → Sended 分支从未进入 ⇒ **0x110 疑似没到 C8T6**。下次排查顺序：① C8T6 板上固件是否含 0x110 放行版（帧校验需 0x100 与 0x110 双 ID；若板上仍是 31b7982 的 0x100-only 严格版会把 0x110 静默丢帧）；② M4 固件是否带 `CAN_Master_SendAck` 且在跑（先走 A7 释放流程）；③ 看 `g_can_node_dbg.rx_total` 区分"帧压根没到"还是"到了被校验丢弃"。**✅ 根因找到并已修复（2026-09-09）**：`c8t6/Core/Src/can_node.c` 把 `case CAN_ACK_ID`(0x110=272) 写进了 `switch(data[0])`——uint8 data[0] 最大 255 **永不匹配** → `s_ack_tick` 从未置位 → Sended 分支永不触发（这就是"只显示 EVT/OK"的**代码级原因，与 M4/固件无关**）；且 d[0] 回显码 0x01 会与开闸指令码混淆的隐患一并排除。已改为在指令解析**前**按 `rh.StdId==CAN_ACK_ID` 单独分发（置 ack_tick + continue）。修复在工作区未提交；**待重烧 C8T6 后手遮验证 `CAN:Sended`，通过即补 3 组提交**。**✅ 真机验证通过（2026-09-09）**：C8T6 重烧后手遮 BH1750 → OLED 行4 出 `CAN:Sended`（约 1s），0x110 事件确认握手闭环完成；3 组提交已补回（本地，未推云端）。
- **✅ 阶段3 已由 SG90 落地取代（2026-09-09）**：原"0x100 收→SG90 开闸"当时 SG90 硬件未加、只更新逻辑状态+OLED 行3；现在 `gate.c` 真执行（缓动到位），0x200/0x210 状态位 bit0 反映真实闸位——本条仅留档。
- **✅ 已生成 `c8t6/实现步骤.md`**（分阶段路线图）：阶段1=FreeRTOS+单 OLED（已亮屏，收尾删诊断）；阶段2=+BH1750，遮光用**百分比掉点**状态机（基线 EMA、`drop% ≥ SHADE_DROP_PERCENT` 连续 `SHADE_CONFIRM_N` 次翻转、回滞释放），**待新建 `Core/Inc/config.h`**（遮光敏感度/SG90 CCR/CAN ID/显示行宏；`OLED_LINE_*` 建议从 freertos.c 收拢过去）；阶段3=+CAN(0x100 收指令→SG90 开闸，0x200 遮光上报，0x210 心跳)+OLED 全状态。CAN=bxCAN（句柄 `hcan`，需配 `CAN_FilterTypeDef` 过滤器 + `HAL_CAN_Start` + 回调 `HAL_CAN_RxFifo0MsgPendingCallback`）。SG90 建议 TIM2_CH1/PA0（PSC71/ARR19999=50Hz，CCR=脉宽µs）。
- **2026-09-06 OLED 疑难结论**：①字模 `oled_font8x16.h` 已与权威源(qlqqs STM32G4-OLED-SSD1306-I2C-HAL `OLED_Data.c`)逐字节核对一致（含 `!`=00 00 00 F8..|..33 30..）→ **w/o/r “缺顶”是江协 8x16 小写 x-height(仅占中部 7px)的字体设计，非数据 bug**；②字符串确为 `"HelloWorld"`（无 `!`）→ **第 4 行渐现的碎“感叹号”+渐进乱码 = 整屏每秒 bit-bang 重绘(每页 128B 约 1.7ms)被 RTOS/中断抢占**，落在 Stop/ACK 窗口 → 从机丢字节/页指针漂移。**修复**：`ssd1306.c` 的 `SSD1306_WriteCmd/SSD1306_WriteData` 已包 `__disable_irq()/__enable_irq()` 临界区（单事务<2ms，页间恢复可调度）。待实测：若仍有则改 OLED_Task 为“行内容变化才刷新”并核对 Stop 后总线空闲。


---

## K210-2026-09-11 （从 AGENTS.md 原样搬移：单机联调 / 方向定案 / SD 卡排查 / 工具与环境）

- **🔎 K210 单机联调（2026-09-11）**：单机直连看到的"倒转"根因 = 当时翻转只做在 Qt 侧、固件无补偿；已改为**固件软件层一处修正**（见下条"方向最终定案"）。新增**方向排查模式** `ORIENT_PROBE=1`（每 4s 轮换 4 组合，编号画在画面 + 串口 `[ORIENT] combo=n`）与可观测性（`[BOOT]`/`[KPU] files`/`[RECOG]`/overlay 叠画/`CONSOLE_PREVIEW`，细节见 `k210_fw/README.md`「单机联调记录」）。**常驻测试 `k210_fw/tools/test_plate_decode.py`**（CPython 仿真、无需硬件，**42 项全过**）。**沙箱 Python：`C:\Users\iosran\AppData\Local\Programs\Python\Python314\python.exe`**（不在 PATH）。**模型目录不敏感解析**（`KPU_DIRS` = `/sd/KPU`→`/sd`→`/flash/KPU`→`/flash`→`/` + `_resolve_models()`，失败原因 `kpu_err` 直接上屏）——即此前 `model_unavailable` 的修法，用例见 README。
- **✅ K210 方向最终定案（2026-09-11 四次板验）= 只在固件侧翻一次，Qt 侧不翻**：① 本 CanMV 固件(v1.0.4) `sensor.set_hmirror/set_vflip` **实测无效**（4 组合画面都不变），只能用**软件层** `img.replace()`；② 固件最终 `CAM_SW_HMIRROR=True / CAM_SW_VFLIP=False`，在 `capture_frame()` 拍完即翻、位于一切消费之前（板载屏/上行 JPEG/检测/识别方向一致），**这一处修正同时让 K210 自带屏与 MP157 大屏都正确**；③ 故 Qt `k210_link.cpp` 默认 `K210_VIEW_FLIP=0`（marker `K210-ORIENT-NONE`），**不再叠任何翻转**。**定案方法（值得复用）= 单轴探针**：`vflip` → 竖直反/水平对，`hmirror` → 水平反/竖直对 ⇒ 每个单轴翻转只弄反自己那个轴，只有"未变换本就正确"才可能。**别靠"看起来反了"猜轴向**——round5 HMIRROR → 09-11 不变换(从未部署验证) → vflip 三次反复都因缺单轴对照。**运行时开关（免重编）**：`/etc/park-ui.env` 写 `PARK_UI_K210_FLIP=none|hmirror|flip180|vflip` + `systemctl restart park-ui`（启动 `qWarning` 打印生效值）。
- **⛔ SD 卡始终挂不上（2026-09-11 排查，未解决；用户已选择先忽略、不用模型）**：卡在槽、FAT32、冷启动后 `listdir("/")` 恒为 `['flash']`、`/sd` 报 `[Errno 19] ENODEV`。**已排除**路径/落位/FAT32/重刷厂家固件/`machine.SDCard`；**SPI 底层已证实正常** ⇒ 疑为该高速卡与固件挂载流程兼容性，**下一步换 ≤8G Class4/6 低速老卡(FAT32)**。**⚠️ `/flash/main.py` 驻板必须用 CanMV IDE「保存到设备」**（点"运行"只流式执行编辑器内容、不写盘，故"运行的代码"≠"开机自启的代码"）；`freq.conf`/`config.json` 是固件配置**勿删**。
- **🔧 K210 单机可控性/环境事实（2026-09-11 补齐）**：① **固件带全部车牌 KPU 扩展 API**——REPL 实测 `hasattr` 检查 `load_kmodel/init_yolo2/run_with_output/regionlayer_yolo2/lp_recog/lp_recog_load_weight_data` 结果 **`missing: []`** ⇒ **不需要另刷"Lite 固件"**（此前"makerobo 固件可能缺 API"的担心排除）；② 固件标识 `MicroPython v1.0.4-22-g0f1e00b-dirty on 2023-02-14; CanMV_Board with kendryte-k210`，CPU 416MHz/PLL1 400MHz(`config.json`)；③ **PC(Windows) 侧 Python 3.14 无 pip、无 pyserial** ⇒ 若将来要做 PC→K210 串口灌文件(内部 flash 放模型)，**用 PowerShell 的 `[System.IO.Ports.SerialPort]` 直接开 COM 口**，不需要装任何包；④ 容量事实：`uos.statvfs("/flash")` = `(131072,131072,21,21,21,0,0,0,0,128)` ≈ **2.6MB**(与三模型合计 2,536KiB 极接近，能否装下 **必须用实写探测**：写 `/flash/probe.bin` 直到异常，再删)；⑤ CanMV IDE **能删除/管理设备上的文件**(用户已在用其删 `/flash` 里的旧 main.py)。
- **🎯 厂家资料包重大发现（2026-09-11，本会话）**：查 `E:\download\k210`（=**正点原子给创乐博/makerobo CanMV-K210 的资料包**，readme 是 ALIENTEK 的）：① **`9.原理图\CanMV K210嵌入式模组原理图.pdf` 解压 Flate 流后确认板上有 `MicroSD_1`/`SD1` 卡槽**，信号 `SD_CS/SD_MOSI/SD_SCLK/SD_MISO`（**SPI 方式接 SD**）；② `4，程序源码\实验38 车牌识别实验\` 里三个模型 + `main.py` 与我们手上的**逐字节相同**（main.py MD5 均为 `B77DDFB57E428AE4CE7BCD80C68F796A`，模型 460456/697512/1498500）⇒ 我们用的就是这套厂家例程；③ 厂家固件 `创乐博（makerobo）canmv-K210固件 2023-2-13.bin`(2.07MB) 二进制含全部车牌 API 名与 `/sd`/`mount` ⇒ **既带车牌 API 也带 SD 挂载**（与板上 banner 版本/日期吻合）；④⑤ 同包还含 CanMV IDE 2.9.2 / kflash_gui / CH9102 驱动，模型目录约定 `/sd/KPU/`（我们的多目录解析已兼容卡根目录）。**⇒ 首要排查 = 卡必须"上电前插好"（SD 仅上电挂载，热插无效，REPL 里 `listdir("/")` 应出 `['flash','sd']`）；若上电前已插好仍 ENODEV，则用上面的 kflash_gui 把这份厂家固件重刷**（重刷会清掉 `/flash` 里的 main.py，需用 CanMV IDE 重传；模型在 SD 上不受影响），此举可**一次性同时解决"SD 不挂载"与"可能缺 KPU 扩展 API"两个隐患**。
- **✅ K210 SD 卡 SPI 底层实证 + 根因锁定（本会话收尾）**：`machine.SPI(1, baudrate=400000, polarity=0, phase=0, sck=27, mosi=28, miso=26, cs0=29)` 构造成功，手动发 SD **CMD0/CMD8 均回 `R1=0x01`** ⇒ **SPI 物理通路（MOSI/MISO/SCLK/CS）与卡的 SPI 协议层完全正常**，`ENODEV` 根因 **100% 锁定 = 16G C10 高速卡（闪迪 Ultra）与固件完整挂载流程（ACMD41/读容量/文件系统）的 SPI 兼容性**，非线路/接触/格式问题。**K210 SPI/引脚 API 踩坑记录**：spi id 用 **1**（3 报 `spi id error( > 0 & !=3 )`）；CS 关键字是 **`cs0`~`cs3`** 不是 `cs=`（`cs=29` 报 `extra keyword arguments`）；`dir(machine.SPI)` = `CS0~CS3,SPI0,SPI1,SPI2,SPI_SOFT,MODE_MASTER(_2/_4/_8),read,readinto,write,write_readinto`；**`machine` 无 `Pin`**（引脚走 `fpioa_manager`，`import fpioa_manager` 后属性是 `FPIOA`/`fm`，无 `fm.fpioa`）。**`/flash/main.py` 现状更正**：本次实测 = **667B 厂家出厂欢迎屏 demo**（"Welcome to Makerobo CanMV"），既非车牌版 40843B 也非旧记忆的 15276B ⇒ **车牌版 main.py 至今未正确驻板**（IDE 点"运行"不写盘，必须"保存到设备"）。**结论：换 ≤8G Class4/6 低速老卡（FAT32），软件层无可调余地；换卡后三模型拷卡 + main.py「保存到设备」驻板即可闭环**。
- **K210 运行前提**：真实预览需 K210 `k210_fw/main.py` 已写入 Flash 并自动运行，当前 USB CDC 文本链路使用 `LINK="console"`、板端设备 `/dev/ttyACM0`、Qt 参数 `--mode text --baud 115200`；仅测试 LCD 可用 `--demo on --mode none`，不依赖 K210。


## SD-2026-09-13 （从 AGENTS.md 原样搬移：SD 卡裸 SPI 直读 / FAT32 读取器 / 挂载层修复的完整记录）

- **✅✅ SD 卡读写真相查明（2026-09-13；此前"卡坏/格式坏/要换卡"的结论全部作废）**：**卡（闪迪 16G A1）、卡座、线材都正常，是驱动时序错了**。① 固件挂载层确实不可用、不必再试：`machine.SDCard()` → `TypeError: cannot create 'SDCard' instances`（stub 类）、官方 FAQ 的 `SDCard.remount()` **死等不返回**、`/sd` 恒 `ENODEV`；K210 的 SDIO 引脚固定 IO18/19 而卡座接在 SPI 的 IO26~29 ⇒ 固件走 SDIO 必然等空。② **正道 = 裸 SPI 直读**，正确参数（错一个就"卡死"）：`machine.SPI(1, baudrate=100000, polarity=0, phase=0, bits=8, sck=27, mosi=28, miso=26, cs0=29)`；**每个数据块读完必须 flush 64 字节**（把卡还攥着的 2 字节 CRC 时钟出去，否则下条命令错位、卡状态机乱掉、MISO 恒 0xFF）；**等 0xFE 数据令牌预算要 ≥2s**（实测单块随机读 47~50ms，原先 5ms 必然"卡死"且把系统拖到串口都没反应——只能断电恢复）；CSD 容量 = `(((CSD[7]&0x3F)<<16)|(CSD[8]<<8)|CSD[9])+1)*1024` 扇区；根目录（簇 2）LBA = `rsvd + nfat*fatsz + spc`（**不是** `rsvd+nfat*fatsz`，这个 off-by-one-cluster 我踩过）。③ **已实测跑通**（`k210_fw/sd_probe2.py` → `RESULT: PASS`）：握手 OK、CSD=31601→15801MiB、LBA0 `55AA`、part[0] `type=0x0C start=8192`、FAT32 `spc=64 rsvd=294 nfat=2 fatsz=3949`、根目录 5 项含 **`LP_DET~1.KMO / LP_REC~1.KMO / LP_WEI~1.BIN / MAIN.PY`**。④ **三件套一直在卡上**，只差读出来；真名在 LFN 条目里，读取器必须重组 LFN 并截到 UTF-16 NUL。⑤ 新增 `k210_fw/sd_spi_fat.py` + `sd_probe2.py`（只读）+ 宿主回归 `tools/test_sd_spi_fat.py`（合成卡含 LFN 与碎片簇链 9→10→4）与 `tools/test_sd_vfs.py`（分区偏移/块设备 ioctl 契约/VFS 读写与 seek），**全部全绿**。⑥ **挂载层修复（2026-09-13 续）**：`uos.mount(BlockDev, "/sd")` 在本固件报 **`OSError(1)` EPERM**（块设备协议不受支持）；改为 **`uos.register_vfs(VfsFat32(fs,"/sd"), "/sd")`** —— 自写极简只读 VFS（open/read/seek/listdir/ilistdir/stat/statvfs/chdir），**`main.py` 就能用 `open('/sd/...')` 按路径加载模型**，无需固件支持；块设备只暴露**分区**（block 0 = 分区起点，固件 FAT 驱动无法指定 MBR 偏移）；`ioctl` 未知名必须返回 **-1**（返回 0 会被当成功）。⑦ **实测发现：那张 16G 卡现在是"空 FAT32"**——根目录只有 Windows 生成的 `WPSettings.dat`(12B)/`IndexerVolumeGuid`(76B) 与 `.`/`..`，**三模型与 `MAIN.PY` 都不在了**（此前读过 `LP_DET~1.KMO` 等），所以下一步必须**先从 PC 把三件套拷回卡**（或拷进 `/sd/KPU/`）。**教训**：`sd_check.py` 的 `probe_mount()` 会动挂载层、可能弄坏 SPI 引脚状态，纯 SPI 路线下别跑它；同一上电周期里"挂载"和"SPI"不能混着试。


## 第7步-2026-09-11 （从 AGENTS.md 原样搬移：云端兜底 + LCD 运维面实现细节 / 评审 / 板端探针 / 逐条修复）

- **需求（用户原话）**：① LCD 改云端检测阈值（置信度）；② LCD 主动要求云端检测；③ LCD 换云端 API/模型名；④ LCD 自动检测 WiFi 并能改 SSID/密码（参考厂商 `ip link set wlan0 up` / `wpa_supplicant -B -D nl80211 -i wlan0 -c /etc/wpa_supplicant.conf` / `udhcpc -i wlan0`）；⑤ 其余自选。**做完先说方案、别 git 提交**。
- **架构定调**：云模块落在 **Qt 应用 `park_ui` 内部**（原 PhaseMd/08 写的独立 `core1_ui/cloud_api/` 作废——Core1 就是 park_ui 进程，再拆一个 HTTP 客户端只多一条 IPC）；**传输层用 Qt Network（`QNetworkAccessManager`）而非 libcurl**（板端 Qt 5.12.8 自带 QtNetwork、异步不阻塞 UI、交叉编译零新依赖）。**Core0 仍然一行云代码都没有**（`check_static.py` 会扫 `core0_service/**` 有无 curl/QNetwork/deepseek，发现即 FAIL）。
- **新增文件（`core1_ui/qt_gui/src/`）**：`cloud_settings.{h,cpp}`（`/etc/park/cloud.conf` 持久化，`QSaveFile` 原子写+`.bak`，越界值夹取，key 只以掩码进日志）、`cloud_client.{h,cpp}`（异步 POST：JPEG q70→base64→`data:image/jpeg;base64,`，5s `QTimer` 超时+`abort()`，传输类错误重试 ≤1，三级容错解析=去围栏→括号配对→`QJsonDocument`，401/402/404/429/5xx 分类，断网演练+假结果模式，**不碰 shm**）、`wifi_manager.{h,cpp}`（wlan0 状态/wpa_cli/iw 扫描/改配置，**写前备份 + 20s 无 IP 自动回滚**，无 `pgrep/pkill` 用 `/proc` 扫描）、`softkeyboard.{h,cpp}`（无键盘板端软键盘，密码掩码+SHOW）、`settingspage.{h,cpp}`（齿轮进全屏三页签：云端/网络/诊断）。改：`ipc_writer`（`cloudFallbackRequested` 信号 + `clearCloudPending()`）、`ipc_reader`（`confThreshold`/`cloudPending` 进快照）、`k210_link`（`latestFrame()` 非消费取帧供云用）、`mainwindow`（WIFI/CLOUD 芯片、齿轮、底栏「云端复检」）、`main.cpp`（全装配）、`qt_gui.pro`（`QT += network` + 5 模块）。
- **三个阈值口径（不新增端侧阈值）**：Core0 的 `conf_threshold`（shm 偏移 56，**设置页只读展示**）+ Core1 的 `trigger_conf`(0.60 触发云) / `accept_conf`(0.50 接受云答案)。
- **收敛性保证**：低置信/失败 → `cloud_pending=1` + `cloudFallbackRequested` → 云请求；**任何出口**（成功/无法识别/失败/开关关闭/无 K210 帧）都落到 `onCloudResult()` 或 `clearCloudPending()` ⇒ `cloud_pending` 必归零 ⇒ Core0 的 3s→6s 延长必收敛（这是本步最关键的不变量）。
- **配置**：`/etc/park/cloud.conf`（600，`$PARK_CLOUD_CONF` 换路径、`$DEEPSEEK_API_KEY` 兜底注入）——**在仓库外 ⇒ 天然不入 git**。模板 `deploy/sample_cloud.conf`；`install_all.sh` 新增 **[8/9]** 步 `install -d -m 700 /etc/park` + 不覆盖式放模板（**已存在则保留 key**），并把 `pkill -9 mxapp2` 换成 `/proc` 扫描（板端无 pkill）。
- **文档**：`docs/protocols.md` **新增 §5 云端 HTTP 通道**（请求体/超时/重试/三分类/阈值口径/WiFi recipe，§6 保留"尚未拷入"=MQTT 可选）、`PhaseMd/08` 全面回填（P7-01 改 Qt Network、P7-02 图像源按现状、P7-12 新增 LCD 运维入口、G7 勾选状态、避坑补 Qt 5.12 无 `setTransferTimeout` 等）、`core1_ui/qt_gui/README.md`（结构表补 5 模块 + **实测可用的 book 交叉编译配方**）、**新建 `core1_ui/qt_gui/G7_ACCEPTANCE.md`**（L1 静态门禁 / L2 板端起来+能力探针 / L3 用例 G7-A~G7-I：阈值持久化、测试连接、手动复检回写、自动兜底时序、模型/API 切换、软键盘、**WiFi 改错密码 20s 回滚**、断网演练+假结果+无 key 红线、Core0 不涉云佐证 / KPI 量测 / 常见判读表）、`deploy/README.md`（cloud.conf 用法+探针）。
- **静态门禁**：`python3 core1_ui/qt_gui/tools/check_static.py` 覆盖 20 个 GUI 源文件 + 第7步断言（QtNetwork/超时/容错/掩码/wifi 回滚/pro 完整性 + 负向：日志出现 apiKey、源码出现 `sk-` 字面量、core0_service 出现 curl/QNetwork、设置页写 Core0 `conf_threshold`）→ **PASS**。
- **本步只动 Core1** ⇒ **只需要重编 park_ui 一次**，core0/M4/C8T6 二进制与固件都不用动（第7步红线：Core0 禁云请求）。
- **🔍 编译风险评审（两轮子代理只读）**：1 个硬错（Qt 5.12 无 `Qt::SkipEmptyParts`）+ 9 个真 bug（eventfd 挂 stdin、wifi 阻塞 UI、apply 无超时、云重试窗口、自超时后仍重试、cancel 关不掉定时器、QSaveFile 丢 0600 等）——**每条都已是 `check_static.py` 断言**。
- **📡 板端探针（2026-09-11）**：QtNetwork+libssl 在（**不需 libcurl**）；`/etc/ssl/certs` 与 `curl` 都没有 ⇒ 必须自带 `/etc/park/ca.pem`；`iw`/`wpa_cli`/`udhcpc`/`wpa_supplicant` 全在 `/sbin` ⇒ 代码用**绝对路径**；厂商 wpa 配置的三个全局项必须保留（写前 `.bak` + 20s 回滚）；启动日志打 `supportsSsl`/CA 路径。已回填 `PhaseMd/08`、`protocols §5.4`、`deploy/README`、`qt_gui/README §3`、`G7`、`code.txt`。
- **🐞 板验第一轮（2026-09-11）**：① 真机编译/运行通过（journal `cloud: config ... (loaded)`、`supportsSsl=yes`、CA=`/etc/park/ca.pem`——**CA 必须自备**：把 PC 的 `Git\mingw64\etc\ssl\certs\ca-bundle.crt` 拷成 `/etc/park/ca.pem`）；② **长文本会顶爆 linuxfb 布局** ⇒ 窗口显式 min/max + 芯片 `Preferred`+`minimumWidth(0)` + elide（教训：往状态栏加控件先想"会不会出现长文本"）；③ 本内核**无 `/proc/net/wireless`** ⇒ RSSI 走异步 `wpa_cli signal_poll`；④ **板端粘贴命令必须纯 ASCII**（中文注释会吃掉整行）。
- **🕐 无 RTC（开机 2020 ⇒ 证书"尚未生效"）**：`park-clock.service` + `set_clock.py`（明文 HTTP `Date:` → `date -u -s`，多主机回退 + 300s 容差）已在启动链中；临时可 `insecure_tls=1`。
- **🔧 同时修的观测缺口 + PRESET 联动**：① 云请求的**失败/成功以前只上屏、不进 journal**（所以 `journalctl | grep cloud` 只能看到 `timeout after 5000 ms -> abort`，看不到分类原因）→ `main.cpp` 现在对 `failed`/`unreadable`/`finished` 全部 `qWarning`（含 `cloud: FAILED <reason> (<detail>)`、`cloud: accepted '<plate>' conf=.. in .. ms -> write-back`）；② 设置页 **`PRESET` 按钮以前只换模型名不换端点**（必然 404）→ 现在**一次点击同时切 model+api_base**：`deepseek-chat`/`deepseek-reasoner` → `api.deepseek.com/chat/completions`，**`qwen-vl-max`/`qwen-plus` → `dashscope.aliyuncs.com/compatible-mode/v1/chat/completions`**（用户指定用通义千问），`gpt-4o-mini` → `api.openai.com/v1/chat/completions`；`deploy/sample_cloud.conf` 补三家对照。**注意 `timeout_ms` 上限 ~6000**：Core0 在 `cloud_pending=1` 时只把 3s 延到 **6s 硬上限**（P7-04），再大没意义。
- **🎯 云"最后卡点" = Qt 5.12.8 + OpenSSL 1.1.1 在 TLS 1.3 协商上卡死**（同一条请求 python ~1s 拿到 200）⇒ 已实现 **`transport=auto|qt|python`**：Qt 失败即切 python3 子进程（内嵌 `kPyTransport`、job 文件 0600 且退出即删、只用掩码进日志）并保持该通道，另有 TLS1.2 一次性重试、代理强制直连。
- **🔌 LCD 手动联网按钮 `联网` + 时钟加固 + 云错误诊断三项**：① `WifiManager::bringUp()`（设置页「网络」页签第 1 个按钮）= 用**当前** `/etc/wpa_supplicant.conf` 跑厂商三步（复用 `buildScript()`），**不写文件、不 arm 回滚**（`m_bringUp` 标志让 `onApplyFinished`/`settle` 走"只报告"分支），20s 硬上限，结束报 `link up (ssid X)` / `give no lease`。② **时钟**：真机确诊 `date -u`=2020 + `certificate is not yet valid` ⇒ `set_clock.py` 加 `--retries 8 --delay 10` + 成功/失败**无条件**打 journal、`park-clock.service` `TimeoutStartSec` 40→180、**新增 `park-clock.timer`**（`OnBootSec=3min` + 每 15min 再校）。③ **云错误诊断**：`extractContent()` 对 HTTP 200 空 content 给出 `finish_reason` + "reasoning-only ⇒ 换非思考模型/提 max_tokens"；失败 detail 升级为 **`body 795B: <响应体片段>`**（`collapseForLog()`，永不含 key）；`max_tokens` 64→**256**（思考型模型会吃光预算导致 content 空，真机 `FAILED empty content: body 795B` 的成因）。门禁加 9d/9e/9f 断言。
- **🕒 LCD 改时间**：设置页「诊断」页签新增一行 `UTC yyyy-MM-dd HH:mm:ss`（**年份 <2025 追加 `NOT SET: cloud TLS will fail (certificate is not yet valid)`**）+ 两个按钮：**`同步`** 跑 `/opt/park_ui/set_clock.py -v --retries 3 --delay 3`（异步 `QProcess`）、**`改时间`** 用软键盘输入 UTC，**严格正则 `^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}$` 校验后以 argv 直接调 `/bin/date -u -s`**（不经 shell，无注入面）；工具路径用 `clockToolPath()` 解析（/bin、/usr/bin、/sbin、/usr/sbin）。门禁 9g 断言。
- **💾 网络页签 `保存` 按钮 = 只写 `/etc/wpa_supplicant.conf`**：`WifiManager::saveConfig(ssid, psk)` = 校验（ssid 非空、psk 空或 8..63）→ **`.bak` 只在不存在时生成**（反复保存不覆盖厂商原件，保住 `CONNECT` 回滚用的那份）→ `writeConf()`（厂商布局：注释头 + `ctrl_interface=/var/run/wpa_supplicant` + `update_config=1` + `ap_scan=1` + `network{ ssid=… psk=… key_mgmt=WPA-PSK|NONE }`，`QSaveFile` 原子写、UTF-8、转义 `"`/`\`）→ 刷新状态，**不动链路、不 arm 回滚**；状态行 `saved to /etc/wpa_supplicant.conf - press CONNECT (or bring up) to activate`。**三个动作分工**：`保存`=只写文件、`联网`=只动链路（bringUp）、`连接`=写+应用+20s 无租约回滚（以前只有这一条路，改错密码 20s 后文件被回滚，看起来像"没保存成功"）。门禁 9h 断言。

## K210-2026-09-13-SD与内存（从 AGENTS.md 原样搬移：SPI 提速 / GC 堆砖机 / 相机内存 / 软重启）

按时间顺序的完整推理链，结论在 AGENTS.md §八，这里只留证据。

- **① "打完 `model size 697512` 就不动了"根本不是内存，是读盘太慢**。`setup_sd_vfs()`
  重写成单文件时**漏了"挂载后提速"这一步**（旧的外部模块版有 `sd.set_baud(BAUD_FAST)`）。
  而本板是 `uos.mount(我们的 BlockDev)` ⇒ **固件 FatFs 每读 512B 回调一次 Python
  `readblocks()`**，SPI 停在挂载用的 100kHz 时：
  `识别 697512B = 56s / 权重 1498500B = 120s`。实测日志坐实：
  `SDX:[SD] read 270 KB in 25652 ms` = 10.5KB/s、`rec.load_kmodel returned in 65092 ms`。
  修法：`/sd ready` 那一刻 `sd.set_baud(BAUD_FAST)`；并加读进度打印（每 256KB 一行），
  让"在慢慢读"与"真死了"在日志上分得清（挂住时最后一行会停在某个 KB 数）。
- **② `spi.init(...)` 本固件不认，只有重建对象才行**：`sd.set_baud()` 原来只有一种写法
  `spi.init(mode=…, baudrate=…)`，异常被吞 ⇒ 只看到 `[SD] baud stays 100000`。改成三种
  逐个试并打印结果，**实测成功的是第 3 种 `machine.SPI(1, …, baudrate=1MHz)`**：
  `SDX:[SD] set_baud(1000000) ok via new SPI()` ⇒ `read 525 KB in 5531 ms`（95KB/s）、
  `rec.load_kmodel returned in 6987 ms`（**65s → 7s**）。
- **③ 提速顺手把"等待预算"砍了 10 倍（新的卡死点）**：`TOKEN_TRIES = 20000` 的注释写着
  "~2 s at 100 kHz" —— 提速到 1MHz 后同样 20000 次循环只有 ~0.2s。表现：识别模型读成功
  之后，**紧接着的一次 `uos.stat()`（`_load_weight` 的第一句）把板子挂死**，
  `[KPU] weight needs …` 那行再没出来。修法：预算一律**毫秒制**
  （`TOKEN_MS=2000`/`R1_MS=250`）、失败扇区**重试 3 次**（重发 CMD17）、提速后**自检**
  （读两次 LBA0 比对，不一致自动退回 100kHz 继续）、并且**模型文件大小用读盘前量到的
  `sizes[]`，大段读完之后不再 `stat`**。
- **④ "缩 GC 堆换系统堆"是伪命题，而且会把板子写死**：写入 `gc_heap_size(N)` 之后
  **两次冷启动 `sys_free` 一个字节没变**（2514944→2514944）⇒ 本固件两块内存不互相让；
  那个"+848KB"的实测是 **Lite 固件**上的（我串台了）。更糟的是**压到 253952 之后开机只剩
  `MemoryError: memory allocation failed, allocating 160 bytes`** —— main.py 的**编译期
  峰值**（语法树+字节码）远大于常驻的 110976B，248KB 不够编译。实测 404KB/464KB/512KB 都能起。
  ⇒ 整定函数改成**只读体检 + 只许抬高（<384KB 抬回 512KB）**，权重重试里那条"压到 256KB
  再试"也整条删掉（同一个砖机路径）。**救砖工具 `k210_fw/gc_restore.py`**（2.8KB 纯 ASCII、
  无条件写 512KB）当 main.py 存上去 → 冷启动 → 再存回 main.py，实测救回。
- **⑤ 软重启会伪装成"内存不够"**：在 IDE 里点「运行/Ctrl-D」是软重启，**不归还**上次 KPU
  占的模型内存（实测 `sys_free` 2.5MB→**147456**）。于是同一次开机里：`/sd already usable`
  （固件自己挂的卡 => 我们没句柄、提不了速）→ `load_kmodel 65092 ms` → `weight
  slack=-1351044` 必然失败 → `[KPU] load fail(retry)` **每 10s 一次，每次漏一个 KPU 实例**
  → 十几分钟后 `mem_free=6112` → 最后 `Exception('IDE interrupt')`。修法：`sys_free <
  1.8MB` 直接判定软重启（打印 `UNPLUG AND REPLUG`，跳过模型）；重试封顶 `KPU_MAX_TRIES=3`；
  **内存不够类失败 `permanent=True` 立即放弃**；同一条失败原因只打一次。
- **⑥ 相机/LCD 的内存账（三者不可兼得）**：`识别 761856B + 权重 1499136B = 2208KB`，
  系统堆 2504KB ⇒ **只剩 ~300KB**；而 **LCD 面板缓冲实测 155648B（152KB）**、QVGA 相机缓冲
  要 ~300KB ⇒ `camera 320x240 failed: OSError(12,)`（带 LCD 时只剩 144KB）。
  **`lcd.deinit()` 之后再重配相机会硬故障**（`EPC 0x8006d722 / Cause 0x0`，板子直接死），
  所以 LCD 开关必须在进 `cam_init()` 前定死、之后再降尺寸也不碰 LCD。
  默认 `LCD_PREVIEW=False` 保 QVGA；置 1 则自动降 QQVGA(160x120)。
  检测模型 460456+68KB 必然装不下 ⇒ 改为**先看余量再决定**，不再去申请注定失败的连续内存。
- **⑦ main.py 的体积问题（用户问"是不是 main.py 太大 / 拆多文件会不会好点"）**：
  **不是**。实测 `[MEM] main.py holds 110976 B of the 524288 B GC heap` —— 108KB、占 GC 堆
  21%，而且**吃的是 GC 堆，不占模型用的系统堆**。拆多文件也不会更好：import 进来的模块
  一样常驻 GC 堆（每个模块还多一个 dict），唯一好处是**编译期峰值**变小、能压低"最小可启动
  GC 堆"——可那个数（512KB）是白送的，压小又换不来系统堆 ⇒ 零收益；而代价是板子只把
  `/flash/main.py` 当开机脚本、第二份文件刷固件就没了（`sd_spi_fat.py` 已被清掉过一次）。
  真正的墙是系统堆 2504KB 固定：识别 744 + 权重 1464 = 2208KB，只剩 ~300KB 给 LCD+相机。
- **⑧ 顺带踩到并修掉的**：`f"..."` 之外的东西不多，但有一条值得记 —— `_apply_gc_heap()`
  判定"是否当场生效"原来用"系统堆变大"，而**把被压坏的堆拉大时系统堆是变小的**，会被误判成
  deferred；判据改成只认"变没变"。另外内联驱动里引入了 `utime` 之后，
  `sd_spi_fat.py` 必须自己 `import utime`（宿主回归 `test_sd_spi_fat.py` 里补了 `utime` 桩，
  否则 `NameError`）。


## SD-2026-09-13（原样搬出 AGENTS.md，为 64KB 注入上限腾空间；结论仍有效）

> AGENTS.md §八 现在只留一句指针 + 🔌 那条提速结论；下面是当时的完整记录。

- **✅✅ SD 卡读写（2026-09-13；"卡坏/格式坏/要换卡"全部作废）**：卡（闪迪 16G A1）、卡座、线材都好，**是驱动时序问题**。① 固件挂载层不可用：`machine.SDCard()` 是 stub、`remount()` 死等、`/sd` 恒 ENODEV（K210 SDIO 固定 IO18/19，卡座在 SPI 的 IO26~29）。② **正道 = 裸 SPI**：`machine.SPI(1, baudrate=100000, polarity=0, phase=0, bits=8, sck=27, mosi=28, miso=26, cs0=29)`；每块读完 flush 64B；**等 0xFE 令牌/R1 应答一律按毫秒算预算**（`TOKEN_MS=2000`/`R1_MS=250`——按循环次数算，提速后会悄悄缩水 10 倍，就是"卡死"的根源）；根目录(簇2) LBA = `rsvd + nfat*fatsz + spc`。③ 实测 PASS（`sd_probe2.py`）：CSD→15801MiB、part[0] `start=8192`、`spc=64 rsvd=294 nfat=2 fatsz=3949`；文件名在 LFN 条目里要重组。④ **挂载两条路都在用**：固件若已挂 `/sd`（不稳定）→ 先 `uos.umount` 抢回来用我们自己的驱动；否则 `uos.mount(BlockDev(...))`——**本板实测 `uos.mount` 是能成的**（`[SD] uos.mount our BlockDev ok`）；本固件**没有 `uos.register_vfs`**。⑤ **提速有代价，见上条 `🔌`（09-14 起 `BAUD_TRY` 默认 0，因为 1MHz 在两个固件上都把板子挂死；1MHz 曾实测 10.5KB/s→95KB/s、识别模型 65s→7s）**。⑥ 卡上三件套在 `/sd/KPU/`（09-13 已在位）。⑦ 教训：`sd_check.py` 的 `probe_mount()` 会弄坏 SPI 引脚状态，别混试挂载与裸 SPI。**完整记录 → history §SD-2026-09-13 / §K210-2026-09-13-SD与内存**。


## P6-04-2026-09-11（原样搬出 AGENTS.md §八）

- **🆕 P6-04 Core1 业务写端（2026-09-11 晚；代码完成，静态门禁 PASS，待重编板验）**：新增 `core1_ui/qt_gui/src/ipc_writer.{h,cpp}`（规格 `.codeartsdoer/specs/core1_business/spec.md`）= **`O_RDWR`+`PROT_WRITE` 挂载 `/park_shm`**、magic/version 自检（不符拒绝写入 + ERROR + 一次性节流）、**1s `hb_core1`**、结果回写（`plate/confidence/result_source/result_valid` + `evt RESULT`，先置位后序号）、低置信/失败 → `cloud_pending=1`（不置 `result_valid`）、`req_gate_open/close` 脉冲、200ms 消费 `evt_c0`。**触发源（spec 5.6 可插拔）**：底栏新增**触摸按钮「开闸/关闸」**（板端无键盘，触摸经 libinput 即鼠标点击）+ `O`/`C` 热键 + 外部工具直写 shm，三者共用 `gateRequested()`。`IpcSnapshot` 增 `cloudPending`，CLOUD 芯片在 `cloud_pending=1` 显示 `CLOUD:兜底中`（读 shm 单一事实源）；`IpcReader::onTick()` 提到 **public slots** 供 writer 的 `snapshotRefreshRequested` 立即刷新。静态门禁 `python3 core1_ui/qt_gui/tools/check_static.py` **PASS**（9 文件纯 ASCII + 签名一致性 + **负向断言"绝不写 Core0 归属字段"**）。**待做：book 重编 park_ui → 板端验 `CORE1:在线`（`hb_core1` 3s 内 +2）+ 点按钮出 `[remote] gate OPEN ... (ui)`**。**✅ 板验通过（2026-09-11）**：触摸「开闸/关闸」双向动作正常、闸位状态跟随正确（含 1600ms 保护窗修正后无闪变），**LCD 上 M4↔C8T6 互动功能正常**；剩余小项：`/dev/ttyACM0`（K210 预览）待 USB 物理链路恢复。

## K210-2026-09-15 固定 ROI 直识别（策略来源与约束）

**提法**（用户）：闸机前是固定机位，车牌出现区域基本不变 ⇒ 固定检测框，K210 直接对
该区域跑识别模型，省掉检测模型。（用户已先行实现 `ROI_MODE` / `ROI_X/Y/W/H`。）

### 为什么这条策略在本项目成立

1. **场景是固定机位**：抓拍点在闸机前，停车到位的位置由道闸/地感/人工泊车决定，车牌在
   画面里的落点是统计意义上的常量。YOLOv2 逐帧全图找框，解的是一个**已经不需要解**的问题。
2. **内存账**：`lp_detect.kmodel` 460456B + `init_yolo2` ~68KB，合计约 0.5MB 系统堆。
   池子 4.31MB（Lite 固件），识别模型 697512 + 权重 1498500 = 2196012 ⇒ 省下的 0.5MB
   直接变成余量（QVA 相机 389KB + LCD 155648B 之后仍余 ~1.3MB）。
   `jpeg=0B` 那类"资源紧"的问题都出在这一侧。
3. **时间账**：每个识别周期少一次 320x240 的 YOLOv2 前向（外加一次 `regionlayer_yolo2`
   的后处理）。周期预算从"检测+识别"变成"只识别"。
4. **可靠性账**：`det=0`（检测层给 0 个框）这条失败路径**整个消失**——它曾经是本项目
   最难判的一类故障（`det=0` 分不出"没候选/低于阈值/没跑到检测"三种情况）。

### 代价与硬约束（不能只讲收益）

- **框必须紧**：识别模型吃的是"车牌刚好占满 208x64"的图（厂家检测框外扩 0.08 后再
  `cut→resize(208,64)`）。人工框如果比车牌大很多，字符在 208x64 里就更小，
  `lp_recog` 直接认不出。⇒ 现场标定要从大到小收，收到"车牌刚好占满黄框"为止。
- **车距/车位必须稳定**：车牌尺寸随车距变化。固定框只对"停到大致同一位置"有效；
  车距变化大（例如允许停远停近）就必须回 `ROI_MODE=0` 走双模型。
- **它不是一个"更聪明的检测"**：只是把检测换成人工先验。场景变了要重新标定。

### 复核时补掉的两个缺口（原有实现已正确，但这条策略还没闭环）

1. **模型搜索仍然要求三件齐全**（`_resolve_models` 里的 `min(_exists(p) ...)`）⇒
   被策略省掉的 `lp_detect.kmodel` 反而成了硬依赖：卡上没有它，`kpu_load()` 会报
   `sd models missing` 直接不装识别模型。已改为 **ROI_MODE 下只要求 recog+weight**，
   并把 det 路径按"命中目录 + 约定文件名"返回（保证 `found` 与路径同目录）。
2. **ROI 框在预览上没有区分度**：`overlay` 把 `boxes` 一律画绿色，而现场调框必须
   一眼看出"哪条线是 ROI"。已改为 ROI_MODE 下画**黄色**（检测框才是绿色）。

### 顺带的一个语义修正

`KPU_DET_EXTEND`（0.08）是给**检测框**用的：YOLO 出的框偏紧，外扩一点防止切掉字符。
人工 ROI 不需要这个——外扩只会让车牌在 208x64 里更小。⇒ 新增 `ROI_EXTEND`（默认 0），
识别循环里按模式选 `ROI_EXTEND if ROI_MODE else KPU_DET_EXTEND`。
`_extend_box(..., 0.0)` 仍然保留"钳到画面内"的作用，所以越界 ROI 依旧安全。

### 回归覆盖（`k210_fw/tools/test_plate_decode.py` §7）

ROI 框 == 人工框（不额外外扩）；同一组相对坐标在 320x240 与 160x120 上按比例缩放；
超大 ROI 被钳进画面且宽高不为 0；`kpu_load_detect()` 在 ROI_MODE 不加载、**不 stat**
检测模型且置 `_KPU_DET_TRIED`；`_resolve_models()` 在 ROI_MODE 下两个文件即可解析，
切回 `ROI_MODE=0` 仍要求三件齐全。


## 关闸-2026-09-11 与收工-2026-09-11（原样搬出 AGENTS.md §八）

- **🐞 远程关闸失效（2026-09-11 真机发现并已修，core0 侧）**：现象 = 触摸/脉冲 `/park_shm` 的「开闸」有反应、「关闸」完全没反应。根因 = `biz` 的 `gate_state` 是**观测态**，只由 M4 的 **0x23（查询应答）**更新，而 **M4 不会在命令后主动上报** ⇒ 开闸后 `gate_state` 仍停在 0 ⇒ 关闸请求命中 `gate_cmd()` 幂等分支 `[gate] CLOSE 0x12 skipped: gate already closed (observed)`，**0x12 根本没发出去**（开闸能过是因为 target=1 ≠ 陈旧 0）。**修法（四步，最终版）**：① 指令**发送成功后以目标态作为工作态**（`b->gate_state = target;` + `refresh_public(b)`，UI 立即跟随，且只在 `rc==0` 时置，失败不撒谎）；② `biz_periodic` 新增**延迟 0x13 重同步**：`GATE_SETTLE_MS=1600`——**这个值必须 > C8T6 的 1Hz 心跳 + SG90 0.2s 行程**，否则查到的是上一拍的闸位（真机实证：800ms 时日志出现 **"CLOSE 之后 M4 回 OPEN、OPEN 之后 M4 回 CLOSED"**，M4 缓存整整慢一拍 ⇒ 读回反值 ⇒ 之后关闸全被幂等吞、UI 长期"闸开着显示关"）；③ **新增 `M4_RESYNC_PERIOD_MS=2000` 周期 0x13**（M4 只在被问时答 0x23，不轮询就永远停在旧值；周期同步还能自愈外部改动/漏报，日志是 DEBUG 级不刷屏，仅在 `link_up` 时发）；④ `on_m4_state` 收到上报即清 `gate_query_at`。**教训：宿主 S7 自测没抓到，是因为它在开闸与关闸之间注入了 `post_m4(...)`（自发 0x23）——真机没有这种自发上报；已给 S7 补"中间无 0x23"的回归用例（现 146 项 0 失败）**。`G4_ACCEPTANCE.md` 的 L2-6 原来把 `skipped: already closed` 当预期，已改正并新增 L3 用例 G4-B2（远程关闸）。
- **🐞 闸位"闪变"（2026-09-11 同日第二次板验，已修）**：现象 = 点「开闸」后 LCD 走 **关 → 开 → 一瞬间关 → 开**。根因 = **周期/边沿的 0x13 可能在命令之前就发出**，它那条 0x23 答案带的是**命令前**的位置，回到 Core0 时正好把刚乐观置上的新状态盖掉，直到下一次轮询才纠正。**修法 = 新增 `GATE_REPORT_GUARD_MS=1500` 保护窗**：距上次**成功命令** 1.5s 内的 0x23 **只采纳 node/can_err、不采纳闸位**（settle 查询在 1600ms > 1.5s，答案正常采纳；`last_cmd_ms` 只在真发出命令时更新，"被幂等跳过"不建窗）。S7 补"命令后 400ms 的陈旧 0x23 不得覆盖命令态"用例（现 **151 项 0 失败**）。
- **🟢 2026-09-11 收工（本轮）**：① core0 在 book 交叉编译并入 `/opt/core0`，**VMIN 修复生效**——`[rpmsg] link UP` 稳定不抖、`[m4] CAN node (C8T6) back online`；② **LCD 1/4 屏修好并板验正常**（见上条，linuxfb 无 WM 下 `showFullScreen()` 不改窗口几何）；③ **M4↔C8T6 闸门链复测通过**：python 直接脉冲 `/dev/shm/park_shm` 偏移 **52/53**(`req_gate_open/close`)，core0 轮询 `ipc_shm_take_requests()` 取走 → RPMSG 0x11/0x12 → M4 → CAN 0x100 → C8T6 舵机动作 ✅（脚本 `/tmp/gate.py`；**shm 的文件系统路径是 `/dev/shm/park_shm`，不是 `/park_shm`**）。**板端新事实**：手动跑 Qt 要 `< /dev/null`，否则后台进程读终端被 **SIGTTIN 停住**（表现为 kill 时报 Exit 1，不是崩溃）；`/etc/profile` 里的 `QT_*` 变量对 systemd 服务无效；**没有 `pgrep`/`timeout`**。**下一步 = P6-05/06/07（分段埋点、五场景、界面 checklist）+ K210 车牌模型（SD 卡）。**

## K210-2026-09-15 SD 提速成功 + 固定 ROI 首跑（真机推理链）

### 1. SD 提速：从"1MHz 挂死"到"200kHz 可用 2x"

现象（本次冷启动，模型读盘计时）：

| 阶段 | 100kHz（09-14） | 200kHz（09-15） |
|---|---|---|
| `rec.load_kmodel(lp_recog)` | 65154 ms | **32959 ms** |
| `lp_recog_load_weight_data` | 139647 ms | **70579 ms** |

日志里的关键三行：

```
SDX:[SD] try init(full) at 200000 ...
SDX:[SD] try init(baudrate) at 200000 ...
SDX:[SD] try deinit+new SPI() at 200000 ...
SDX:[SD] set_baud(200000) ok via deinit+new SPI()
```

**结论修正**：此前把 1MHz 挂死归因于"频率太高"，现在看**更可能是"没 deinit 就重建第二个 SPI 对象"**——
本次成功走的正是新加的 `deinit() + machine.SPI(1, ...)` 路径，而 1MHz 那两次失败都停在**没有 deinit** 的
`new SPI()` 分支上。⇒ 待验证：把 `BAUD_FAST` 依次抬到 400000 / 1000000，看 `deinit+new SPI()` 是否还成立。
（每个变体前都打一行 `[SD] try <变体> at <baud> ...`，所以万一挂死，串口最后一行就是元凶。）

### 2. 固定 ROI 首跑：除识别外全通

```
[KPU] model dir=/sd/KPU ROI_MODE=1 -> recog=697512 weight=1498500 (bytes; det not needed)
[KPU] models ready mem_free=90848 sys_free=1503232
[CAM] 320x240 RGB565 sensor_vflip=False sensor_hmirror=False sw_vflip=False sw_hmirror=True
[MEM] camera 320x240 configured, sys_free=1114112
[BOOT] entering main loop
K2:IMG:0:/9j/4AAQ...   (base64 预览帧，jpeg≈4.8KB，fps 5~6)
K2:END:3780
[DBG] roi: fixed ROI=(15,71,288,95) img=320x240
[RECOG] det=1 ms=67 NG err=no_plate        <- 第一轮：模型真跑了
...
[DBG] recog_box: exception=TypeError("unsupported types for __mod__: '', 'tuple'",)
[RECOG] det=1 ms=9 NG err=no_plate         <- 之后每轮都在 9ms 处早死
```

### 3. 那句 TypeError 不是我们的代码

`unsupported types for __mod__: '', 'tuple'` 的意思是"有个对象不支持 `%`，右操作数是 tuple"。我们先用
**AST 扫全文件**排除自己：

    Mod sites with a NON-literal left operand: 3
        501  pos            %  cl_bytes
        597  off            %  512
       2364  self.img_seq   %  n

三处全是**整数取模**（分块偏移 / 帧计数），没有任何动态格式串；左操作数也不可能在运行时变成空串。
⇒ 这句是 **CanMV 的 C 层**（`KPU.run_with_output` / `KPU.lp_recog`）抛出来的。它一旦抛，框循环就被
`except` 吞掉、`best` 保持 None，于是**每一轮识别都在 9ms 处结束**——看起来"识别没了"，其实是被这
一句错误卡住。

### 4. 两个修复

1. **把"哪一次 C 调用"暴露出来**：原来框循环只有一个 `except`，只打 `recog_box: exception=...`。
   现在 `run_with_output` 与 `lp_recog` **各自兜底**（tag `recog_run` / `recog_out`），并把 crop 尺寸一起
   打出来 ⇒ 下一次冷启动一眼就能定位。
2. **照抄厂家例程的对象卫生**：参考例程每轮显式 `del lp_img / del resize_img`（然后 `gc.collect()`），
   我们以前把 `rimg` 挂在局部名上等 GC。"**第一轮能跑、之后每次早死**"与"上一轮的 AI 输入图没放掉、
   KPU 侧状态被搅乱"高度吻合 ⇒ 现在每轮 `finally: del rimg`。

### 5. 顺带修好的：预览流

同一轮 `K2:IMG/K2:END` 帧正常、`[stat] fps=5~6 jpeg=4800B`。之前那次 `jpeg=0B` 是 `compress()` 没成，
但被裸 `except` 吞了；现在 `jpeg_from()` 会有 `[DBG] jpeg_fail / jpeg_none` 兜底，不会再沉默。

### 6. 未解疑点

主循环期间仍周期出现 `SDX:[SD] read 2333/2589/2845 KB`（≈每 70s +256KB ≈ 3.7KB/s），
而**主循环里没有任何代码读 SD**（模型早已装完）。最可能是 **CanMV IDE 在轮询设备文件系统**
（它有文件浏览器）。无害，但别把它当成"模型还在读盘"的证据。下次可用"拔掉 IDE 只留串口终端"
对照一次来确认。

### 7. 下一步

对着真实车牌把黄框（`ROI_*`）收紧到"车牌刚好占满"，然后看：

```
[DBG] recog_run: ...           或   [DBG] recog_out: ...      <- 二选一，那就是根因所在
[RECOG] det=1 ms=<几十> OK plate=粤B...
```


## det0-2026-09-14（原样搬出 AGENTS.md §八）

- **🔍 `det=0` 怎么读（2026-09-14）**：`det=0` 本身分不出三种情况（没候选／低于阈值／检测没跑到）。新增 `[DET] n=… top=… thr=… img=WxH`（`top` = `regionlayer_yolo2()` 里的最高置信度 `p[5]`），只打一次不刷屏。**分辨手段 = 把 `KPU_DET_THRESHOLD` 临时降到 0.05**：出现 `n>0` ⇒ 阈值问题；仍是 `n=0` ⇒ 模型看不见车牌（画面里没有／方向不对）。⚠️ 厂家例程用 **0.7**、我们已经是 0.5 ⇒ **阈值大概率不是主因**。


## SD-2026-09-15：提速不是"能/不能"，是 flake（BAUD_TRY 置回 0）

**现象（同一份代码、同一张卡、同一天）**：

| 第几次开机 | 400 kHz 重定时 | 结果 |
|---|---|---|
| 1 | `try deinit+new SPI()` → `ok via deinit+new SPI` | 过；682KB kmodel 16753 ms（100kHz 时 65154 ms ⇒ **4x**） |
| 2 | 同上 | 同上（这一次后来的权重加载停在"打开文件"那一下） |
| 3 | `variant3: deinit() ...` 之后**再无输出** | **挂死**（断电重启） |
| 4 | 同上，停在"打开权重文件"之前 | **挂死**（断电重启） |

**关键判断**：

1. **不是频率决定论，也不是"没 deinit"决定论**。100 kHz 从不挂、200 kHz 与 400 kHz 各成功过，
   但 400 kHz 有 2/4 次死在**不同的**步骤上 ⇒ 属于**边界/亚稳态**，不是可复现的单一 bug。
2. 挂死点在**固件 SPI 驱动内部**（`deinit()` / `machine.SPI(1,…)` / 重定时后的**第一次真实收发**），
   Python 没有超时能救 ⇒ **每次尝试的代价是断电重启**，对"还要调识别"的阶段不可接受。
3. 因此 `BAUD_TRY` 置回 **0**：挂载 100 kHz、全程不提速，`2.1MB` 模型读盘 ~3.5 分钟，
   但**从不挂**。等识别链路跑通、ROI 标定完，再单独安排提速实验（从 **200 kHz** 起）。
4. 新增证据行（`variant3: deinit() / new SPI() / constructed, now the first read`）——
   下次提速时，最后一行就说明是**哪一步**没回来。
5. 驱动心跳从每 256KB 改成**每 64KB**：100 kHz 下约 6 秒一行、400 kHz 下约 1.6 秒一行，
   "在读 / 挂了"从此一眼可判；两条长读前还加了一行 ETA
   （`[KPU] weight: 1498500 B from SD at 100000 baud -> ~149 s`）。

**用户追问："是不是 main.py 太大了？"——不是，三条硬证据（2026-09-15）**

- **位置不对**：挂死发生在固件 SPI 驱动的 C 调用里（`deinit()` / 新建 SPI 对象 / 重定时后第一次收发），
  此时 main.py **早已编译完并常驻**，Python 一行都没在执行 ⇒ 文件大小改不了外设寄存器的行为。
- **时机不对**：两次挂死都发生在**整个开机里内存最宽裕的时刻**——刚 `uos.mount` 完
  （`gc_free=400000 / gc_heap=524288 / sys_free=3764224`）和刚读完识别模型
  （`sys_free=3002368`，权重加载后还剩 1.5MB）。真要是"太大"，应该死在最紧的时刻；
  而 main.py 的编译峰值发生在 **t=0、什么都还没加载**时（最松的时刻）。
- **性质不对**：大小问题有自己的报错长相——GC 堆压到 248KB 时是
  `MemoryError: memory allocation failed, allocating 160 bytes`（**开机就死、必现**），
  这台板子见过。而这次是**同一份文件 4 次开机 2 成 2 死、死点还不同** ⇒ 随机性，不是容量。
- 数字：`[MEM] main.py holds 121568 B of the 524288 B GC heap` = **23%**，而且吃的是 **GC 堆**，
  模型/KPU 缓冲用的是**系统堆**（`sys_free`），两者只在"开机 malloc"那一刻交易一次。
  **注释根本不进 bytecode** ⇒ 删注释只会降低编译期语法树峰值（当前不是瓶颈），
  **不会改变 121568 B 这个常驻数字**；拆成多文件同样零收益（import 进来一样常驻），
  这条 2026-09-13 已经量过。

**若还想"更快开机"（都不换板，按风险从低到高）**

1. `BAUD = 400000` + **`BAUD_TRY = 0`**：不提速、改成**一开始就在 400kHz 挂载**
   （SD 规范允许 CMD0/ACMD41 在 100–400kHz）⇒ **完全不碰"在活对象上重定时"那条路**。
   一次冷启动就知道成不成（失败会打 `[SD] … mount failed`）。
2. 两个模型放 `/flash`（SPIFFS），用 C API 的**地址分支**加载（绕开 SD，见上文）。
3. 以上都不行且"快且稳"是硬指标 ⇒ 换 K230。


**用户问："是不是换 K230 会好点？"——评估如下（2026-09-15，结论：先别买，除非 SD 变成硬约束）**

- **K230 能根治的东西**：K230（CanMV-K230，双核 C908 RISC-V 1.6GHz+800MHz，KPU INT8/INT16，
  512MB LPDDR3，microSD 走 SDIO 控制器，HDMI + 3 路 MIPI CSI）——SD 读盘变 MB/s 级、
  内存从 K210 的 4.3MB 变成 512MB、预览用硬件 H.264/JPEG ⇒
  **我们这两天打的仗（裸 SPI 驱动、4.3MB 堆里挤模型、resize/JPEG 帧率）几乎全部消失**。
  官方例程里本来就有车牌识别 demo。
- **代价（这才是关键）**：① **模型要重来**——手上三个文件（`lp_detect.kmodel`/`lp_recog.kmodel`/
  `lp_weight.bin`）是 **K210 KPU v1 工具链**的产物，K230 用另一套 nncase
  （见 [K230 nncase 开发指南](https://www.kendryte.com/k230/en/main/_sources/01_software/board/ai/K230_nncase_Development_Guide.md)），
  **不能直接跑**；要么找 K230 版车牌模型，要么自己用 nncase 转。② **固件要重写**：
  `main.py` 里 40KB 的 SD 驱动、`maix.KPU` 老 API、GC 堆整定、`[SDX]` 那一整套全部作废；
  可复用的只有**架构与协议**（固定 ROI 直识别、`K2:IMG`/`K2:OK:` console 行、ROI 标定方法）。
  ③ A7/Qt 侧**几乎不用改**（它只消费 `/dev/ttyACM0` 上的 console 行）——这是最便宜的迁移部分。
- **建议顺序**：先用 `BAUD_TRY=0` 把**识别**调通（100 kHz 只是慢，不阻塞功能）；
  若之后"3.5 分钟开机"成了硬约束，再考虑两条更便宜的路：
  **(a)** 提速实验从 200 kHz 起；(b) 把两个模型放进 `/flash`（SPIFFS）后用 C API 的
  **地址分支**（`lp_recog_load_weight_data(addr, size)` → `load_file_from_flash`）加载，
  彻底绕开 SD——可行性未知（需要 /flash 余量 ~2.1MB 与上传手段），一次冷启动即可验证。
  以上都不行、且必须"快 + 稳"，再上 K230。

## K210-2026-09-15b 端到端跑通 / no_plate 的两个来源 / 图片存储审计

### 1. 第二跑：第一次全程跑通（这就是"SD 不提速"的回报）

100 kHz、`BAUD_TRY=0`、固定 ROI、Lite 固件：

```
[SD] /sd ready -> ['System Volume Information', 'KPU']
[KPU] model dir=/sd/KPU ROI_MODE=1 -> recog=697512 weight=1498500 (bytes; det not needed)
[DBG] > rec.load_kmodel(/sd/KPU/lp_recog.kmodel)      model size 697512
  ... SDX:[SD] read <N> KB in <M> ms (still working) ×10  ...      → returned in 65251 ms
[KPU] weight: 1498500 B from SD at 100000 baud -> ~149 s
[DBG] > rec.lp_recog_load_weight_data(/sd/KPU/lp_weight.bin)
weight_data_size: 1498500                              ← 固件自己打的，证明文件已打开
  ... 22 行心跳 ...                                     → returned in 139749 ms
[KPU] weight ok
[KPU] models ready mem_free=342208 sys_free=1503232
[BOOT] kpu_load -> True ; [CAM] 320x240 RGB565 ... ; [BOOT] entering main loop
[DBG] roi: fixed ROI=(15,71,288,95) img=320x240
[RECOG] det=1 ms=68 NG err=no_plate      ← 首轮（含模型首次预热）
[RECOG] det=1 ms=11 NG err=no_plate      ← 之后每轮 9~15 ms
[stat] fps=3~5 jpeg=9~13KB mem_free=209888~371552
```

- 预览流正常（`K2:IMG:`/`K2:END:` 成帧、`jpeg≈5~13KB`、`fps 3~5`）。
- 上一轮那句 `[DBG] recog_box: exception=TypeError(...)` **一次都没出现** ——
  `del rimg`（照抄厂家例程的 `del lp_img`）之后，"识别第一轮就早死"消失了。
- 日志末尾 `Exception: IDE interrupt` + `free kpu model buf succeed`：用户手动停的，
  **KPU 缓冲这次被正常归还**（但仍建议下一步之前断电重来，别用 IDE 的 Run）。

### 2. 但 40 帧全部 `no_plate` —— 两条读数先纠正

**① `det=1` 不是"检测到车牌"，是结构值。**
`ROI_MODE=1` 时 `recognize_frame()` 走 `else` 分支，直接 `lps = [(x, y, w, h)]`（写死一个框），
`det = len(lps)` ⇒ **恒等于 1**。旁证：日志里**没有一句 `[DET] …`**
（`det_hit`/`det_empty` 只在 `_KPU_DET is not None` 时才打）⇒ 检测模型确实没加载。
所以拿 `det=1` 判断"有没有车牌"是错的；`ROI_MODE=0` 时代才是真框数。

**② 失败在纯决策层，不在 C 调用层。**
`ms=9~15` 说明 `cut → resize(208x64) → pix_to_ai → run_with_output → lp_recog`
**整条都跑完了**（任何一段抛异常都会留下 `recog_crop`/`recog_run`/`recog_out`/`recog_box`，
日志里一条都没有）。也就是说：crop 成功、模型跑了、`lp_recog` 有返回，
**却仍然判 no_plate**。

### 3. `no_plate` 有**两个来源**，而旧日志把两者写得一模一样

`recognize_frame()` 末尾（2026-09-15 之前）：

```python
return {"plate": None, ..., "error": "no_plate"}      # ① best is None
```
```python
if best and best[0] >= RECOG_CONF_TH: ...             # ② best[0] < 0.60
```

- **① `best is None`**：模型**压根没给出可用输出** —— crop 返回 None / `lp_recog()`
  空或太短 / 省份下标越界 / 框循环里抛过异常。
- **② `best[0] < RECOG_CONF_TH`（=0.60）**：模型**认出来了**，只是分数没过阈值。
  `best` 在 `if conf >= RECOG_CONF_TH: break` **之前**就更新了，所以它是"最像的那个答案"。

两者对策相反（① 查 ROI 与画面朝向；② 调阈值或收紧框），
而日志里都是 `NG err=no_plate` ⇒ **每熬一次 3.5 分钟冷启动，换来的是同一句无用信息**。

**改法（本轮，additive、不改行为）**：
- `no_plate` 的返回里多带 `best` / `best_ascii` / `best_conf`；
- `[RECOG]` 失败行分叉：
  - `NG err=no_plate best=粤B12345 conf=0.31 th=0.6` ⇒ **②，模型看得见**
  - `NG err=no_plate (model gave no usable output)` ⇒ **①，模型没吐东西**
  - `NG err=no_plate [<kpu_err>]` ⇒ 格式与原来完全一致（有 `kpu_err` 时不变）
- 回归：`test_plate_decode.py` §8 新增"近失答案被暴露且不被当成车牌"的断言
  + 源码 needle（`"best_conf"` / `best=%s conf=%s th=%s` / `model gave no usable output`）。

### 4. 一个真实的偶发故障：`recog_crop` 的 `MemoryError`

约 40 帧里出现一次：

```
[DBG] recog_crop: exception=MemoryError('Out of normal MicroPython Heap Memory!
      Please reduce the resolution of the image you are running this algorithm on ...')
      free=289088 sysfree=1126400
```

- `free=289088` 是 **GC 堆**（不是系统堆，`sysfree` 还有 1.1MB）。
- 当前 ROI 是 **288×95** ⇒ `img.cut()` 的 RGB565 临时图就要 `288*95*2 ≈ 55KB`，
  再 `resize(208,64)` 又 `≈27KB`，`pix_to_ai` 再一块 AI 缓冲（`208*64*3 ≈ 40KB`）⇒ 峰值 ~120KB。
- 而 `[stat]` 显示 `mem_free` 在 **209888 ~ 371552** 之间摆动 ⇒ 赶在低点上就是一次分配失败
  （GC 堆碎片 + 预览 JPEG 缓冲同时在世）。
- **结论：把黄框收紧到"车牌刚好占满"这件事，一箭双雕** ——
  (a) 字更大 ⇒ 识别率；(b) `cut` 缓冲小 3~4 倍 ⇒ 这个偶发 MemoryError 跟着消失。

### 5. 用户提问："拍到的图如果没传给 linux，会不会把空间挤爆？"——审计结论

**K210 侧：根本没有"存图"这回事，没有东西可以积压。**

- `main.py` 全文**不写任何文件**：没有 `open(path, 'w')`、没有把图像落盘；
  `COPY_TO_FLASH=0`；`/flash`（SPIFFS）在本板**连新建文件都不行**；`/sd` 我们只读模型。
- 预览是**同步、无缓冲**推出去的：`console_send_jpeg()` 把 base64 按 `CONSOLE_CHUNK=900`
  切成行，每行 `print()`，行间 `utime.sleep_ms(CONSOLE_PACE_MS=25)`。
  **没有队列、没有 backlog、没有重试缓存。**
- ⇒ 所以失效模式是**反过来的 fail-stop**：Linux 不读时 `print()`（USB CDC）会阻塞，
  主循环连同识别一起停住；reader 回来就自愈。
  **不是"空间被挤爆"，是"链路断了就停"** —— 这两个是不同的东西，别混。

**Linux / Qt 侧：`k210_link` 本来就是"只留最新一帧"的有界设计。**

| 位置 | 上限 | 依据 |
|---|---|---|
| `K210LinkWorker::m_latest` | **1 帧** | 注释即 "publishes only the newest decoded QImage"；刻意不做队列，来不及就丢（有丢帧计数） |
| `m_lineBuf` | 64 KB | 超了直接 `clear()`（`if (m_lineBuf.size() > 64*1024)`） |
| `MainWindow::m_events` | 64 条 | `if (m_events.size() > 64) m_events.remove(0, size-64)` |
| `/tmp/park_cloud.jpg` | **1 张 ~10KB** | 固定文件名 + `QIODevice::Truncate`，同名的 `/tmp/park_cloud_req.py`、`/tmp/park_cloud_job.json` 同理；`/tmp` 还是 tmpfs |

另外：`K210LinkWorker::feedText()` 里**不认识的 console 行走完 if/else 链就被丢掉**（没有 `else`），
所以 `K2:IMG:` 那一大串 base64 **不会进 journald**；park_ui 也没有逐帧 `qDebug`。
⇒ 磁盘不随运行时长增长。

**唯一真缺口（本轮已补）**：两个**重组表**只由"帧结束符"清空 ——

- text 通道 `m_chunks`（`K2:IMG:` 分片 → `K2:END:` 才 `clear()`）
- binary 通道 `m_binChunks`（`seq → payload`，`0x02` 帧尾才 `clear()`）

对端**半帧死掉**（拔线 / IDE interrupt / 丢一行）时，这些分片会一直躺着等一个永不到来的结束符。
而且 key 空间**不自限**：乱码的 `K2:IMG:` 行能造出任意的 `off`（`rest.left(colon).toInt()`），
binary 的 `seq` 是 **quint16（65536 宽）**。

加法（`k210_link.cpp`）：

```cpp
static const int kMaxTextChunks = 64;        static const int kMaxTextChunkBytes = 8 * 1024;
static const int kMaxBinChunks  = 256;       static const int kMaxBinChunkBytes  = 4 * 1024;
// 真帧 ≈ 15 个 900 字符分片 ⇒ 两处都是 ~4x 余量；超限即 clear() 并重同步
if (b64.size() > kMaxTextChunkBytes || m_chunks.size() >= kMaxTextChunks) m_chunks.clear();
if (payload.size() > kMaxBinChunkBytes || m_binChunks.size() >= kMaxBinChunks) m_binChunks.clear();
```

**为什么"丢掉残帧"是安全的**：两个发布端本来就校验总长 ——
`if (okLen && !b64all.isEmpty() && b64all.size() == want)` 与
`if (total == 0 || all.size() == int(total))` ⇒ 残帧**永远不可能被当成坏图发布**。

门禁（`check_static.py` 第 5b 节，含负向断言）：
四个常量必须在；`m_lineBuf.clear()` 必须在；
**`b64all.size() == want` 与 `all.size() == int(total)` 这两个校验必须在**（否则"丢弃"会退化成"发布坏图"）。

### 6. 本轮改动清单（全在工作区，未提交）

| 文件 | 改动 |
|---|---|
| `k210_fw/main.py` | `no_plate` 带出 `best`/`best_ascii`/`best_conf`；`[RECOG]` 失败行按两个来源分叉 |
| `k210_fw/tools/test_plate_decode.py` | §8 加近失暴露断言 + 三个源码 needle |
| `core1_ui/qt_gui/src/k210_link.cpp` | 两个重组表的硬上限 + 超限丢弃重同步 |
| `core1_ui/qt_gui/tools/check_static.py` | 第 5b 节门禁（含"发布端必须校验总长"的负向断言） |
| `AGENTS.md` / 本文件 | 记忆更新（含为塞进 64KB 注入上限而做的压缩） |

回归：`k210_fw/tools/` 9 个 test + `build_main.py --check` 全绿（`main.py` 幂等 122040 B）；
`check_static.py` **RESULT: PASS**。

### 7. 下一步（不变）

存 `main.py` → 冷启动（100 kHz，~3.5 min）→ **对着车牌把黄框收紧** → 看新证据行：

- `NG err=no_plate best=… conf=0.3~0.6 th=0.6` ⇒ 模型看得见 ⇒ 调 `RECOG_CONF_TH` / 继续收框
- `NG err=no_plate (model gave no usable output)` ⇒ 模型没吐东西 ⇒ 查 ROI 是否套住、
  画面朝向（厂家例程开 `set_vflip(True)`，我们默认关）
- 期望终点：`[RECOG] … OK plate=粤B…`

## K210-2026-09-16 两段式开机（让"修堆的代码"活下来）/ 目录约定 / 识别已通

### 1. 用户的四个报告，与那个收不上的死结

| 报告 | 结论 |
|---|---|
| "k210 启动要很久，离开 IDE 不清楚启动到哪了" | 见 §5（日志可见性，未修） |
| "每次启动前要刷一次 `gc_restore.py`，否则 main.py 太大读不了" | 真问题是**编译期峰值**，见下 |
| "本来就有开机自愈逻辑（`HEAP_TUNE_GC_MIN=384KB → 抬回 512KB`）" → "有但是没有，main.py 太大，k210 有时候都打不开这个文件" | **接受这个纠正**：`auto_tune_gc_heap()` 存在，但它在最需要它的那一刻**不可达** |
| "把 `gc_restore.py` 和 `main.py` 拆成几个文件放 flash、缩小单文件大小" | 拆分**不省常驻内存**，只降编译峰值 ⇒ 采用，但目的是**让启动器活下来** |

**死结的完整形状**：`main.py` ~122 KB，MicroPython 必须**先把整个文件编译成字节码**才能执行第一行；
编译期的语法树/符号表峰值远大于常驻的 ~120 KB（实测 248 KB 堆上只剩
`MemoryError: memory allocation failed, allocating 160 bytes`，而 404/464/512 KB 都能起）。
堆一旦被压小，编译器就死在**文件内部**，**连一行输出都没有**（真机现象正是"会运行，但没有任何打印"，
既无 `[MEM]` 行也无 `MemoryError:`）⇒ 写在业务里的 `auto_tune_gc_heap()` 是**死代码**。
修复逻辑必须住在"小到能在坏堆上编译出来"的文件里，而 K210 的开机执行入口**只能是 `/flash/main.py`**。

### 2. 为什么是"两段"，不是"一次修好"

`gc_heap_size(N)` **只把数字写进 SPIFFS 的 `freq.conf`**，本次运行的堆划分一个字都不动
（见 2026-09-13 源码定案）。所以启动器无法"修完接着跑"：

```
第一次上电：启动器体检 → 堆坏 → 写回 512KB → 打 POWER-CYCLE → 结束（什么都不跑）
    ↓ 用户断电重上电（零 IDE 操作）
第二次上电：堆已好 → import park_app → park_app.main() → 正常业务
```

### 3. 启动器 `k210_fw/main.py`（3813 B，硬上限 4096 B，纯 ASCII）

关键顺序（**不可调换**）：

1. `print("[BOOT] launcher: gc_heap=%d sys_free=%d gc_free=%d")` ——
   **必须在 `import park_app` 之前**：应用编译失败时，这是**唯一的证据行**。
2. 堆 < `GC_MIN=384*1024` ⇒ `gc_heap_size(512*1024)` → **读回核对** →
   打 `POWER-CYCLE`；读回不一致打 `did not stick`；取不到 `gc_heap_size` 时退回
   提示用 `k210_fw/gc_restore.py`。
3. 否则（堆正常）：`sys.path` 补 `/flash`、`/sd` → `import park_app` → `park_app.main()`。

- 只 `import gc, sys`（顶层 import 只允许这两个），**绝不内联业务代码**（否则它自己又变成大文件）。
- 末尾保留 `if __name__ == "__main__": main()`。
- 回滚路径：`park_app.py`（业务）保留了 `__main__` 守卫 + 自己的 `auto_tune_gc_heap()`，
  **存回成 `/flash/main.py` 仍能独立启动**（退回单文件方案不需要改代码）。

### 4. 目录约定（用户要求）：`k210_fw/` 根 = "会被保存到设备的文件"

判据 = **会不会在 CanMV IDE 里被执行「保存文件到设备」**。据此把非部署文件移进 `tools/`：
`sd_spi_fat.py`（41.6 KB，驱动唯一真源，被 `tools/build_main.py` 内联进业务）、
`helloworld_1.py`（厂家样例）、`readme.txt`（`LCD_PREVIEW = False` 的纸条）、
`fw_probe.py` / `sd_probe2.py`（板端探针，**用时临时存成 `/flash/main.py`**）。
同步改了这些文件的路径引用（`build_main.py` 的 `DRIVER`、`test_build_main.py`、
`test_sd_spi_fat.py`、`test_sd_vfs.py`、`test_setup_sd_vfs.py`、`test_fw_probe.py`）。

**用户随后又追问“`boot_main.py` 是不是 `main.py`？”** ⇒ 定下第二条判据：
**仓库文件名 = 设备文件名**。于是 `boot_main.py` 改名 `main.py`（它本来就是设备上的
`/flash/main.py`），原 `main.py`（业务）改名 **`park_app.py`**。救砖的 `gc_restore.py`
是唯一例外：它顶替 `main.py` 的角色，名字保持自解释。

**根目录现在只剩**：`main.py`（3.8 KB 启动器）、`park_app.py`（122923 B 业务）、
`gc_restore.py`、`README.md`、`tools/`。
（`park_app.py` 生成头里那句 `# generated by … from sd_spi_fat.py - do not …` 必须逐字节
不变，否则要重跑 `tools/build_main.py`；改名后所有宿主 test 的 `REPO/main.py` 常量、
以及注释里指业务的 “main.py” 也一并改成 `park_app.py`——**否则那个名字现在指的是启动器**。）

### 5. 用户自己改的 `_recog_crop` 限频（收进回归）

`gc.collect()` 进函数先跑；`resize` 之后 `del lp_img`；失败走 `_recog_crop_fail_n` **限频打印**
（`<=3 or %20==0`）`print("[DBG] recog_crop: %r %s (hint: tighten ROI_* or lower CONSOLE_IMG_EVERY)")`，
成功清零。仓库里的 `ROI_X/Y = 0.05/0.30`、`ROI_W/H = 0.90/0.40`、`RECOG_CONF_TH = 0.60`
**仍是未标定的默认值**——见 §7 的第一个遗留。

### 6. 本轮改动清单（全在工作区，未提交）

| 文件 | 改动 |
|---|---|
| `k210_fw/main.py` | **新建**（原名 `boot_main.py`）：两段式启动器 |
| `k210_fw/tools/test_boot_launcher.py` | **新建**：ASCII / ≤4096 B / 可 `compile()` / 顶层 import 只有 gc+sys / 修复-再跑形状 / `[BOOT] launcher:` 必须在 `import park_app` 之前 / **负向**：不得内联 `PROVINCE_ZH`/`_recog_crop`/`readblocks`/`0x1021`/`Fat32`/`lp_recog` 等业务符号；app 侧必须保留 `__main__` 守卫/`main()`/`auto_tune_gc_heap`/`("/flash", "/sd")` |
| `k210_fw/tools/test_plate_decode.py` | §8 断言改匹配限频 `print`（旧 `_dbg_once("recog_crop"` needle 过期） |
| `k210_fw/{sd_spi_fat.py,helloworld_1.py,readme.txt,fw_probe.py,sd_probe2.py}` | 移入 `tools/`（引用同步） |
| `k210_fw/main.py` ↔ `k210_fw/park_app.py` | **改名**：仓库名对齐设备名（原业务 `main.py` → `park_app.py`）；`build_main.py` 的 `MAIN` 常量与 8 个 test 的路径/注释同步 |
| `k210_fw/README.md` | 本轮先重写了目录结构 + 部署，**随后用户把整个文件删掉了并要求"别改 k210 的 readme 了"** ⇒ 现在 `k210_fw/` 没有 README，**不要再创建/修改它**；目录约定、启动链、回滚都记在 `AGENTS.md` §八 `🚀` 与本文件本节 |
| `AGENTS.md` | 新增 `🚀 两段式开机`；`⛔ K210 板端操作纪律` 修正（运行期不能新建文件，但 **IDE 上传可自定义文件名**）；`✅🎯` 与 §六.1 从"一帧都没认出车牌"改为"识别已通" |
| `docs/AGENTS_history.md` | 本节 |

回归：`k210_fw/tools/` **10 个宿主 test 全 exit=0**（新增 `test_boot_launcher`）；
`build_main.py --check` → `driver block 27166 bytes` / `park_app.py 122923 -> 122923` / `check only: nothing written`；
`check_static.py` 延续上一轮 **PASS**。

### 7. 部署（给用户的三步，与"每次刷 gc_restore"彻底告别）

1. IDE 打开 `k210_fw/park_app.py` →「保存文件到设备」→ 文件名填 **`park_app.py`**；
2. IDE 打开 `k210_fw/main.py` →「保存文件到设备」→ 文件名填 **`main.py`**；
3. **断电重上电**（冷启动）。若堆是坏的，启动器会写回 512 KB 并打 `POWER-CYCLE`，
   **再冷启动一次**即可（全程零 IDE 操作）。

### 8. 下一步（新遗留）

1. **把现场标定的 `ROI_*`/`RECOG_CONF_TH` 从板子读回仓库** ——
   任何一次重新上传 `park_app.py` 都会覆盖板上的那份，直接存会**丢掉现场标定**。
2. **打通非 IDE 场景的日志可见性**：`core1_ui/qt_gui/src/k210_link.cpp` 的 `feedText()`
   **没有 else 分支**，只认 `K2:` 行 ⇒ `[BOOT]`/`[MEM]`/`[SD]`/`[KPU]`/`[RECOG]`/`[stat]`
   **全部被 park_ui 丢弃** ⇒ "离开 IDE 看不到启动进度"与"跑久了死机看不到最后一行 `[stat]`"
   **是同一个根因**。方案 = 加 else → journald + 屏上事件条；改完须在 book 上
   `sh tools/build_arm.sh` 重编（本机无 arm 工具链，不能编译验证）。
3. **长时间运行的堆增长取证**（靠每 ~2 s 一行 `[stat] fps=… mem_free=…`）。
4. 仍未定：**"为什么以前每次都要刷 `gc_restore.py`"**。用户那次成功开机的日志里
   `[MEM] boot: … gc_heap=524288`，说明**那一次本来就没刷** ⇒ "每次"这条规律可能另有原因
   （或绑定在"改完 main.py 重新保存"这个动作上）。现在每次开机都会有一行
   `[BOOT] launcher: gc_heap=… sys_free=… gc_free=…`，正好用它来确认。

## 2026-09-16b WiFi 启动路径 + K210 日志上屏（两个用户报障）

### 1. 报障："wifi 没开，板子卡死在连接网络这一步"

**这不是玄学，是启动依赖图错了。** 三处证据（改动前）：

| 位置 | 内容 | 后果 |
|---|---|---|
| `wifi-up.service` | `Before=park-clock m4-load core0-bus park-ui`、`TimeoutStartSec=60` | 整个本地栈排在网络后面 |
| `wifi_up.sh` | DHCP 3 轮 × `udhcpc -t 5 -T 3`（+ 4s 固定 sleep）≈ **50 s** | **没有 AP 时也照跑**，全是白等 |
| `park-clock.service` | `Before=m4-load core0-bus park-ui`、`--retries 8 --delay 10`（每次 3 个 host × `TIMEOUT_S=5`）、`TimeoutStartSec=180` | **最多再压 180 s**，而且 systemd 到点会把它杀掉 |

**最容易被忽略的一点**：`park-ui.service` 的 `After=` 里**并没有** `park-clock`，但 systemd 的
依赖是**有向且可传递**的 —— `park-clock` 写着 `Before=park-ui`，park-ui 就得等它。所以
无网时"面板黑屏"= wifi-up(~50 s) + park-clock(≤180 s) ≈ **最多 4 分钟**，正是用户说的"卡死"。

**修法（5 处，全在 deploy，无需重编）**：

1. `wifi-up.service`：`Before=` 只留 `park-clock.service`（时钟是唯一真需要链路的东西）；
   `TimeoutStartSec=60 → 40`。
2. `park-clock.service`：**删掉** `Before=m4-load/core0-bus/park-ui` —— 它自己可以慢慢重试，
   但不许把延迟递给面板；`park-clock.timer`（开机 3 分钟）本来就是"链路起来了再补一次"的第二机会。
3. `park-ui.service`：`After=`/`Wants=` 里**去掉 wifi-up**（本地业务与网络无关；WIFI 芯片在没租约时
   本来就显示 `down`，云调用也已经有 `no network` 分类）。
4. `wifi_up.sh`：加**关联门**（`wpa_cli status` 轮询 `wpa_state=COMPLETED`，最多 12 s；没关联上就
   **跳过 DHCP 直接 `exit 0`** —— 没有 AP 时根本无租可续）+ DHCP 段加**墙钟截止**
   （`DEADLINE=$(date +%s)+15`，每轮前检查；`-t`/`-T` 缩小到 4/2 且 `-n` 不可全信）。
   最坏 ~32 s < unit 的 40 s。
5. `set_clock.py`：先 `ip -4 addr show $PARK_UI_WIFI` 看有没有 `inet ` —— 没有就**打一行、立即退出**，
   不再 8×10 s 空转；`ip` 找不到或命令失败时**保持旧行为**（"说不准"不能当成"没网"）。
6. `install_all.sh` 的链说明同步改：**本地栈 = board-power → m4-load → core0-bus → park-ui；
   WiFi/时钟在旁边跑**。

**门禁**（`check_static.py` 第 9 节，含负向）：本地栈三个 unit 的**依赖行**里不得出现 `wifi-up.service`；
`wifi-up`/`park-clock` 的 `Before=` 不得指向本地栈；`wifi_up.sh` 必须有 `wpa_state=`/`not associated`/`DEADLINE`；
`set_clock.py` 必须有 `has_ipv4_lease`。**只扫依赖指令、跳过注释** —— 这些 unit 现在都用注释解释规则，
朴素的子串检查会栽在自己的文档上（第一次跑就这么 FAIL 了一次）。

### 2. 报障："离开 IDE 看不到启动到哪了"（＝日志去哪儿了）

先回答定位：**K210 的启动日志一行都没走 LCD**，全是 `print()` → USB CDC（REPL console），
和 `K2:IMG:`/`K2:END:` 预览流、`K2:OK:`/`K2:NG:` 结果行**共用同一条串口**。所以：
插 PC+IDE 全都能看见；插 MP157 时这些行**物理上已经在 `/dev/ttyACM0`**，
是 `k210_link.cpp` 的 `feedText()` **只认 `K2:` 行、没有 else 分支**把它们全丢了。

**用户拍板**：`park_ui` 只读**实时**流即可，tty 打开之前那几秒不用管（省掉了 K210 侧
"再打一行开机小结"的改动）。

**修法（只改 Qt 侧，3 个文件）**：

- `k210_link.cpp`：`feedText()` 加 `else if (!line.isEmpty() && !line.startsWith("K2:"))` →
  `emit k210Log(QString::fromUtf8(line))`；行**截断到 `kMaxLogLineBytes=512`**；
  `K2:` 开头但未知的行仍然丢弃（那是协议噪声，不是日志）。
- `k210_link.h`：facade 暴露 `k210Log(const QString &)`；worker→facade 用现成的
  signal-to-signal 转发。
- `main.cpp`：`k210Log` → ① `qWarning("k210: %s")` **全量进 journald**（这就是"死机前最后一行"的
  取证面）② `k210LogIsPanelWorthy()` 过滤后 `win.pushEvent("k210 …")` 上**底栏事件条**
  （`[BOOT]/[MEM]/[SD]/SDX:/[KPU]/[CAM]/[DET]/[GC]/[ROI]` 以及含 fail/error/warn/panic 的行）；
  ③ `SDX:`/`[stat]`/`[RECOG]` 这些高频行（`k210LogIsBurst`）**限流 5 s**，两个面都不刷屏。

**为什么分两个面**：`[stat]`（每 ~5 s）与 `SDX:`（每 64 KB 读盘）是长时间运行的体检数据，
事件条只滚动 5 条、会被它们冲掉；而崩溃后要翻的是 journald，不是屏幕。

### 3. 本轮改动清单（工作区，未提交）

| 文件 | 改动 |
|---|---|
| `deploy/systemd/wifi-up.service` | `Before=` 只留 park-clock；`TimeoutStartSec=40`；注释说明为什么不在关键路径 |
| `deploy/systemd/park-clock.service` | 删掉指向本地栈的 `Before=`；注释留档（含 09-16 报障） |
| `deploy/systemd/park-ui.service` | `After=`/`Wants=` 去掉 wifi-up |
| `deploy/systemd/wifi_up.sh` | 关联门 + DHCP 墙钟截止；文件头加时间预算 |
| `deploy/systemd/set_clock.py` | `ip_tool()`/`has_ipv4_lease()` + 无租约快速退出 |
| `deploy/systemd/install_all.sh` | 启动链文案（两行）与两处注释 |
| `core1_ui/qt_gui/src/k210_link.{h,cpp}` | `k210Log` 信号 + `feedText()` else 分支 + `kMaxLogLineBytes` |
| `core1_ui/qt_gui/src/main.cpp` | `k210Log` → journald + 过滤后上事件条 + 5 s 限流常量 |
| `core1_ui/qt_gui/tools/check_static.py` | 第 5c 节（K210 日志）与第 9 节（WiFi 不在关键路径，负向断言） |
| `AGENTS.md` / 本文件 | 记忆更新（顺带把两条已归档的 K210 长条压成指针，给 64 KB 注入上限腾地方） |

**本机能验的都验了**：`check_static.py` → **RESULT: PASS**；`sh -n wifi_up.sh install_all.sh` → 0；
`python -m py_compile set_clock.py` → 0。

### 4. 板上要做的两件事（都还没验）

1. **重装 deploy 并重启**看无网启动：`sh deploy/systemd/install_all.sh`（或至少
   `systemctl daemon-reload` 后核对 `systemctl list-dependencies --after park-ui.service`）。
   期望：**面板秒起**、WIFI 芯片 `down`、`journalctl -u wifi-up` 里一行
   `not associated … skipping DHCP`、`journalctl -u park-clock` 里一行 `no IPv4 lease … skipping`。
   想量化就用 `systemd-analyze blame | head`，park-ui 的启动时刻不该再被网络单元拖住。
2. **重编 park_ui**：`sh core1_ui/qt_gui/tools/build_arm.sh`（book 上）→ scp → 重启 park-ui。
   验收：拔掉/关上 WiFi 之外，**插着 K210 时面板底栏应滚动出现 `k210 [BOOT] …`/`k210 [KPU] …`**，
   且 `journalctl -u park-ui -f | grep 'k210:'` 能看到全量（含每 5 s 的 `[stat]`）。

### 5. 真机冷启动的新证据 + 心跳去掉毫秒（2026-09-16 晚）

用户第一次用**启动器**冷启动的日志要点：

```
[BOOT] launcher: gc_heap=524288 sys_free=3477504 gc_free=433376   <- 两段式生效、堆健康
[MEM] park_app.py holds 91648 B of the 524288 B GC heap
[MEM] system heap: have 3477504, ... -> OK
SDX:[SD] read_sector(0) failed after 3 tries (baud=100000)        <- 第一条 SPI 命令就挂
[SD] inline spi driver failed: OSError('cannot read LBA0',)
[SD] /sd not usable
[BOOT] kpu_load -> False                                          <- 没模型
[WARN] cam_init failed: IDE interrupt                             <- 相机被 IDE 打断
[stat] fps=266 jpeg=0B mem_free=352352                            <- 没相机 => 空转
```

- **两段式开机真机通过**：`[BOOT] launcher:` 出现、堆 512KB 健康、没写 flash、不用断电。
- `fps=266 / jpeg=0B` **不是"炸"**：`stat_frames` 只在 `capture_frame()` 之后自增
  （`park_app.py:2471`），相机没起来 ⇒ 每轮立刻返回 ⇒ 计数飞起来；`[stat] fps` 是**帧尝试率**。
  内存一路正常（`mem_free=352352`、堆 524288）。

**新发现的缺陷（未修，等用户定）**：`park_app.py` 的 `setup_sd_vfs()` 从 `SDSPI()` **直接**
`read_sector(0)`，**没有调 `sd.init()`**（CMD0→CMD8→ACMD41→CMD9/CSD）；而 `tools/sd_probe2.py`
与 `tools/sd_spi_fat.py::setup_sd_vfs()`（`sd_spi_fat.py:806-810`）都调了。后果：
① 卡可能还在 idle state，CMD17 直接被拒；② 失败信息只有笼统的 `cannot read LBA0`，
**分不清"卡没响应/没插好"与"卡没被 init"**。以前能读，很可能是 IDE 工作流里先跑过
`sd_probe2.py`（它做 init）再跑业务——同一次上电，卡已在 transfer state。

**另一条**：`IDE interrupt` **不是硬件错**，是 CanMV IDE 打断板子（09-15 那次也是手动停的）。
排查 SD/相机失败前先确认"板子在跑的时候 IDE 没连着、没点停止、没在轮询文件系统"。

**顺手改掉一处**：驱动心跳从 `SDX:[SD] read N KB in M ms (still working)` 改成
`SDX:[SD] read N KB (still working)`——那个 ms 是**在 Python 侧围着 C 层的读**量的，量不到真实
读取时间，而且 `utime.ticks_ms()` 会回绕、计数器还跨文件累计（用户 2026-09-16："计算的读取时间不对"）。
字节数是唯一有用的信息：**"还在走" vs "卡在同一个数"**。
改法照规矩：改唯一真源 `tools/sd_spi_fat.py`（`_progress()` 去掉 `_READ_STATE` 的时间槽），
再 `python k210_fw/tools/build_main.py` 重新拼进 `park_app.py`（`--check` 幂等：
`driver block 27244` / `park_app.py 122993 -> 122993`）；10 个宿主 test 全 exit=0、`check_static.py` PASS。

## AGENTS-2026-09-26（AGENTS.md 全量快照归并：当前有效工作记忆整档并入；根目录 AGENTS.md 仍保留并继续维护）

> 归并方式：`cat AGENTS.md >> docs/AGENTS_history.md` 字节级原样并入，未改动一字节。
> 本节标题以下到文件末尾 = 2026-09-26 当天根目录 AGENTS.md 的完整内容。

---

# 项目记忆（自动注入，勿删）—— 端侧AI · 边云协同停车场

> 本文件是跨会话的工作记忆，记录决定、板级事实、当前状态与下一步；不要与 README/Task 的设计正文重复，交叉处只引用。
- **📦 历史存档 `docs/AGENTS_history.md`**：2026-09-11 本文件顶到 64KB 注入上限（尾巴被静默截断），已把**已结案/被取代**的长记录**原样**搬去该文件（K210 预览帧率迭代、M4 代码审计、RPMSG 打通全记录、C8T6 OLED 三轮排查、0x110 握手排查、can.md 审计细节、实现步骤路线图等）。**本文件只留结论与当前状态**；要完整推理链就去读存档。

## 一、项目一句话 & 文档分工
- 100ASK-MP157（STM32MP157DACx）+ 外接 K210 的停车场**端侧AI·边云协同**Demo，Linux(双A7 SMP)+FreeRTOS(M4)，M4 跑 OpenAMP/RPMSG。
- `README.md`=项目定位/分工一览/硬件清单/目录树/构建；`Task.md`=系统架构/任务拆解/数据交互/约束/KPI（内容隔离，勿重复）。
- 字段级协议（A7↔M4 帧、A7↔K210 串口、CAN 帧、共享结构体）统一归 `docs/protocols.md`（**2026-09-07 已建**：CAN §1 / K210 UART §2 / RPMSG §3 已拷入，其余通道随实现拷入；草案母本 `PhaseMd/10_协议规格总表`，改协议先改母本→同步本文件→改代码→两处变更记录）。
- `MD文档/rk3588/调试记录.md`（2026-09-25 建）= **RK3588（定昌 DC-A588）**调通记录（镜像结构/踩坑/烧录/外设体检），AGENTS 只留结论见 §九。
- `PhaseMd/`（2026-09-06 建）= **执行层任务分解，15 份 md**，按用户定稿的 8 步串行主线组织：①下位机C8T6本地 → ②M4↔下位机CAN → ③Linux↔M4 RPMSG → ④Linux本地业务 → ⑤K210↔Linux → ⑥本地业务联通 → ⑦Linux↔云端 → ⑧全业务跑通；另有第0步环境 + 支撑文档（协议总表/Qt规格/KPI用例/排障手册）+ **14_GitHub现成项目调研**（2026-09-09 建，剩余功能↔现成开源项目映射）。任务编号 `P<步>-<序号>`，每步有验收门 G0~G8；进度推进时同步勾选并更新本文件。

## 二、当前确定架构（重要，含最新变更）
- **车牌识别**：外接 K210（自带摄像头+KPU）替代 A7 本地 TFLite；接 A7-Linux UART（先 UART 后可选 SPI）。K210 固件**预览流常开**（与推理解耦），**识别 = K210 本地 KPU 主责（2026-09-10 定：触发改"持续周期识别"，无下行 UART 引出；09-11 DNK210 双模型接入，见 §7）**；低置信/失败调 DeepSeek Vision 兜底。V4L2 摄像头已删除。
- **⭐ K210 固件路线（2026-09-07 定版；09-10 识别改向见下）**：**CanMV MicroPython + CanMV IDE**（嘉楠官方；原"首选 Kendryte standalone SDK(C)"降为备选，PhaseMd/06 P5-01 已改；README/Task 同步）。K210(CanMV 板，已到手) USB 插 PC=开发/REPL（USB CDC 115200）；插 MP157=数据通道（Linux 出 /dev/ttyACM0）。**识别 = K210 本地 KPU 主责（2026-09-10 改向：无下行 UART 引出 → "持续周期识别 + console 结果行上行"，见 §7）**；低置信/失败 → DeepSeek Vision 兜底。**帧协议 2026-09-07 定版**（docs/protocols.md §2）：CRC16=XMODEM（poly 0x1021/init 0）、seq 按 type 通道计数（0x01/0x02 共用预览单调计数）、0x02 尾=4B 总长(LE)+1B 用途、0xC3=仅失败 JSON、ts=上电毫秒。
- **车辆到位检测**：默认用**板上 AP3216C**（光强ALS+接近PS，焊在 I2C1）做"遮光/接近"触发，替代外部红外/BH1750（BH1750 不必要，因板上已有光感）。
- **道闸模拟**：SG90（PWM，50Hz），由下位机控制。
- **⭐ 最新决策（本会话尾段）**：**BH1750 + SG90 交给下位机 STM32F103C8T6（FreeRTOS）控制**；C8T6 通过 **CAN** 与 MP157 通信；**MP157 M4 只接收 CAN 帧**（**接口 v2 起只收语义事件**"车到位/车离开/闸位/故障/就绪"，**不含 lux/drop% 等传感器数值**，见 §四·接口 v2 条），业务判定（车牌→开闸）在 A7-Core0。即 MP157(M4,FDCAN2→板上TJA1042) → CANH/CANL ↔ C8T6(TJA1050) → BH1750(I2C)/SG90(PWM)。CAN 用**经典 CAN 2.0 + 500kbps**（F103 无 CAN-FD），总线两端各 120Ω、共地。
- OLED 状态屏（M4 调试用）：板上 PF14/PF15 已被 AP3216C 占用且不可达，改用**Camera&Extend 口空闲 GPIO + 软件 I2C(bit‑bang)**（或挂到 C8T6 节点 I2C）。
- **⭐ 网络决策（2026-09-09）**：上位机（Modbus-TCP）与云端（DeepSeek 兜底 / 云上报 MQTT）网络链路**单用板载 WiFi（RL-UM02WBS-8723BU），不用以太网**。README 硬件清单、Task 任务1.2/通道矩阵5和7/阶段4 MQTT over WiFi/约束"网络单链路（仅 WiFi）"、PhaseMd/01 P0-02 已同步。断网时本地业务闭环照常（云兜底/云上报按各自降级策略处理）。

## 三、板级事实（从原理图 .DSN 挖出；PDF 文字被压缩无法直接提取）
- 板上**已占用/只有这些 I2C**：I2C1=AP3216C 光感/接近 + ICM-20608 加速度；触摸 I2C（TP_SCL/TP_SDA）在 LCD 排线口（相关 PE 引脚 TP_INT/TP_RST）；**I2C4/I2C6 只能 A7**，I2C1/2/3/5 可给 M4。
- **I2C2 设计者备注**："consider PH4/PH5 use I2C2"（PH4=SCL/PH5=SDA），但 .DSN 显示 I2C2_SCL/SDA 只连到核心模块引脚、**未引出到底板排针**（丝印找不到 PH4/PH5 正常）。
- **⭐ FDCAN 归属修正（2026-09-08 真机验证）**：板上 TJA1042 实际接 **FDCAN2（PB5=RX/PB6=TX, AF9）**，Linux 节点 = m_can2 / can@4400f000 / 接口 can0；早期"FDCAN1/PD0-PD1"系原理图误读，**作废**。**实测 Linux can0 ↔ C8T6 500k 经典帧全通**（query 回 0x200、1Hz 0x210、开闸 status=0x01）。**FDCAN 内核时钟实测 = 62.5MHz**（`clk_summary` 的 fdcan_k，boot 默认 mux；PLL3Q 未使能 ~24.6MHz、PLL3_P=208.9MHz）——M4 代码与 docs/protocols.md §1 按 100MHz 写的位时序均需按此修正。
- 能插杜邦线的物理接口仅：**JTAG、SWD、Camera & Extend（CAMERA_PORT）**；CAMERA_PORT 含 CSI 总线 + 扩展 GPIO。
- 板上还有：WiFi(RL-UM02WBS-8723BU)、SIM、音频 WM8960、HDMI、以太网、USB、可选4G、CAN FDCAN(TJA1042)；CSI 引脚组 CSI_D0~7/PIXCLK/MCLK/HSYNC/VSYNC。

## 四、M4 CubeMX 工程现状（m4_fw/）
- 结构：`m4_fw/{m4_fw.ioc, CA7(设备树), CM4(工程), Common/System, drivers(CMSIS+HAL)}`。CM4 工程已生成 `Core/{Src,Inc}`、`Startup/`、`STM32MP157DACX_RAM.ld`。
- **已配**：**FDCAN2 已分配给 M4**（FDCAN_FRAME_CLASSIC + Normal + PB5=RX/PB6=TX，AF9；早期记录"FDCAN1/PD0-PD1"已作废，注释残留待清）；**FreeRTOS(CMSIS_V2) 已启用**，任务 `CAN_Rx_Task`(入口 `CANRxTask`)。`main.c`：`MX_GPIO_Init(); MX_FDCAN2_Init(); ... CAN_Master_Init(); ... MX_FREERTOS_Init(); osKernelStart();`。**I2C1 已不在工程**（.ioc 外设清单只有 FDCAN2/FREERTOS/ETZPC 等 10 项，无 `i2c.c/i2c.h`，HAL I2C 模块未使能）。
- **⛔ OLED 已从 M4 工程摘除（2026-09-07）**：2026-09-07 17:04 首次 CubeIDE 构建报 `Core/Src/ssd1306.c:16 fatal error: i2c.h`——旧 `ssd1306.{c,h}`（依赖 CubeMX 生成的 `i2c.h`/`hi2c1`）随 .ioc 重生成（I2C1 被移出）而断链；且 I2C1 走 PF14/PF15 **物理上接不到排针**，板级本就走不通 → 已 `git rm` 删除 `ssd1306.{c,h}` 并清掉 `main.c` 的 include + splash 调用（其余 45 文件编译全过；git 历史可找回）。OLED 状态展示职责归 C8T6 下位机屏。
- **✅ M4 CAN 主端(网关)已落地（2026-09-07，代码就绪待烧录联调）**：新增 `CM4/Core/{Inc,Src}/can_master.{c,h}` = `CAN_Master_Init`(标准掩码过滤器收 0x200~0x2FF + 全局过滤 + Start，main USER CODE 2)、`CAN_Master_Poll`(CANRxTask 10ms 轮询 FDCAN1 RX FIFO0 收 0x200/0x210 + 3s 离线判定 + 提交待发 0x100 指令)、`CAN_Master_SendCmd/RequestCmd`、监视快照 `g_can_master_mon`(online/状态位/last_ev/last_arg/uptime_ms/dev_type/fw_ver/node_id/计数，供调试器 live watch 与后续 RPMSG；**接口 v2 起已删 `last_lux`/`last_drop`**)；`fdcan.c`：**NominalTimeSeg1=3/Seg2=1→采样点 80%**（原 2/2=60%，与从端 75% 匹配）、`TxFifoQueueElmtsNbr=8`（无它发不出；**⚠️ MP1 HAL 用 `HAL_FDCAN_AddMessageToTxFifoQ`，无 MP2 的 AddTxMessage**）；收路径**零中断轮询**（未 ActivateNotification，NVIC FDCAN1_IT0=3 已使能、无 IER 不触发）；`CanRxQueue` 移除（轮询直解析，免队列，freertos.c/.ioc 同步）；堆 3072→**8192**（FreeRTOSConfig.h + .ioc）。**调试后门/心跳灯（2026-09-07 加，09-09 已处理）**：`can_master.h` 的 `CAN_MASTER_DEBUG_AUTO_GATE_MS` 已置 **0**（去自动开闸），心跳灯改业务 LED（**LED_GREEN=PA10**，低电平亮，1Hz；C8T6 离线 3s→5Hz）——详见"设计功能升级"条。**待烧录**，联调见 `c8t6/can.md` §7.2 / PhaseMd/03 P2-06~P2-11。
- **🔑 底板 KEY 与互动方案（2026-09-07 结论）**：底板 4 键 KEY1~KEY4；`KEY1~KEY3` 默认归 **Linux(A7) input 子系统**（教材：hexdump /dev/input/event1、dmesg），`KEY4` 原理图注明=power reset；教材另注明"有一个键没配置进内核、**留给 M4 用**"。→ 本步 M4 **不直接读按键**（避免与 Linux 争用），指令由 `CAN_Master_RequestCmd()`/调试器写 `g_can_master_cmd_pending` 触发；**等 Linux+M4(RPMSG) 打通后**走主链路 `A7·按键→RPMSG→M4→CAN 0x100→C8T6`（M4 侧已留 RPMSG 触发/取状态接口）。若要用那颗"留给 M4"的键，需其 GPIO（待用户提供核心板 pdf 引脚图）。
- **✅ G3 全链路验收通过（2026-09-10 晚，重大根因修正）**：A7(RPMSG 0x11/0x12) → M4 → CAN 0x100 → C8T6 → 闸门 OPEN/CLOSE 真机闭环成功（C8T6 OLED 行3 跟随翻转）。**贯穿性的真根因 = can0 释放方式错误**：旧"release_can = can0 down + unbind 4400f000.can"会在 unbind 后让内核 clk 框架**关闭 fdcan_k 时钟（enable=0）**→ M4 的 HAL_FDCAN_Init 无时钟卡死（FDCAN2 NBTP 恒为复位值 0x06000A03、CAN 全聋），并连带 OpenAMP 引导不稳（此前"重启 flaky：无 channel+CAN 死"现象的统一解释）。**正确 recipe：m_can 保持绑定 + `ip link set can0 type can bitrate 500000` + `can0 up` 点亮时钟（fdcan_k enable=1），A7 静听不发**——实测 can0 up 后重启 M4：NBTP=0x180200(正确 500k@62.5M) + RPMSG channel 一次即成（1704s creating channel）+ C8T6 收 open 全通。**已改**：`load_m4.sh` release_can → `ensure_can_clock()`（bind+can0 up，幂等）；fdcan.c Autotune 非工程模式直接锁 62.5MHz（不信 HAL 失真测量）；main.c 把 IPCC/OpenAMP 初始化移到调度器后 Rpmsg_Task 内（防 wait_remote_ready 卡死整个系统）；`CAN_MASTER_DEBUG_AUTO_GATE_MS` 已置回 0。**更正旧结论**：AGENTS 四·审计条"must release can0( unbind)"与 c8t6/can.md 相关说法**作废**，以本条为准。遗留：C8T6 侧遮光事件(手遮→0x21)与断链 0x22 未测（C8T6 在场即可补）；重启稳定性有待用新 recipe 复测。
- **⭐ API/环境查实结论（本会话挖出，写码依据，勿重复劳动）**：
  - **CubeMX 勾法（P3-01）**：Middleware→OPENAMP 是**虚拟外设**（`VP_OPENAMP_VS_OPENAMP.Mode=OpenAmp_Activated`），勾上自动带出：IPCC 外设 + NVIC `IPCC_RX1/TX1_IRQn` + 生成 `CM4/OPENAMP/` 8 文件（openamp.{c,h}/openamp_conf.h/rsc_table.{c,h}/mbox_ipcc.{c,h}/openamp_log.{c,h}）+ main.c 自动插 `MX_IPCC_Init(); MX_OPENAMP_Init(RPMSG_REMOTE, NULL);`（USER CODE 区外，regen 稳定）+ it.c 插 IPCC_RX1/TX1_IRQHandler→`HAL_IPCC_RX/TX_IRQHandler(&hipcc)` + .cproject 自动加 `../OPENAMP` 等 include。**权威参考 = 100ASK 例程** `E:\download\100ASK-MP157\100ASK_STM32MP157_M4_Code\22_A7_M4_UserModeComm\rpmsg_user\`（CubeMX 生成、与本板内核配套；22=用户态 ttyRPMSG0，23=内核态 rpmsg_client_sample）。
  - **应用层 API（FW 1.7.0 实测核对）**：`VIRT_UART_Init(&h)`（服务名 **"rpmsg-tty"**→Linux `/dev/ttyRPMSG0`）+ `VIRT_UART_RegisterCallback(&h, VIRT_UART_RXCPLT_CB_ID, cb)` + `VIRT_UART_Transmit(&h,buf,len)`（单帧上限 496=RPMSG_BUFFER_SIZE-16，我们协议帧≤489 ✓）；任务循环轮询 `OPENAMP_check_for_message()`；**回调跑在调用者上下文、禁调 FreeRTOS API**（ST OpenAMP_FreeRTOS_echo 模式：回调 memcpy+置标志，发送在任务里）；例程 MAX_BUFFER_SIZE=512、FreeRTOS 堆=32768。
  - **vring 地址归属**：`openamp_conf.h` 定义 `LINUX_RPROC_MASTER` → VRING_RX/TX/BUFF_ADDRESS 全 = -1（**由 Linux 端分配**），M4 侧 rsc_table 不硬编码地址；dts fallback 模板里 vdev0vring0@10040000 等**不是 M4 决定的**。
  - **dts 实况**：100ASK rpmsg_user 的 kernel mx.dts `&m4_rproc` 只有 `mboxes=<&ipcc 0>,<&ipcc 1>,<&ipcc 2>`（vq0/vq1/shutdown），**无 memory-region/vring 节点**（CubeMX 明注 not managed）→ vring 保留内存要么出厂 dtb 已带、要么设备树覆盖（100ASK 注意.txt：部署需设备树覆盖）。板端实测：reserved-memory 有 vdev0vring0@0x10040000(4K)/vdev0vring1@0x10041000(4K)/vdev0buffer@0x10042000(16K)，与 M4 链接脚本 SRAM3 区间吻合。**内核实为 5.4.31（100ASK BSP，非 6.6）**：CONFIG_RPMSG_TTY=y 内置、CONFIG_RPMSG_CHAR 未开；但 NS 通道不自动绑 rpmsg_tty（见上条 ✅ 里程碑的 bind_channel 解法）。
  - **中间件源码位置（fallback 手工导入用）**：FW 包 `C:\Users\iosran\STM32Cube\Repository\STM32Cube_FW_MP1_V1.7.0\Middlewares\Third_Party\OpenAMP\{open-amp\lib, libmetal\lib, virtual_driver\virt_uart.c, mw_if\app_if\openamp_template.*, mw_if\platform_if\{mbox_ipcc,rsc_table}_template.*}`；100ASK 例程组织 = 中间件放 `Middlewares/Third_Party/OpenAMP/`、CubeMX 生成物放 `CM4/OPENAMP/`、.project 逐文件 link。
  - **⚠️ HAL 头文件 ISO-8859 编码坑**：grep 本仓库 HAL 头**必须加 `-a`**，否则静默零命中（本会话差点误判"MP1 无 GetProtocolStatus"，实有）。CMSIS 直读路径也确认过：`FDCAN_PSR_BO`=bit7。
  - 链接脚本早已就位（非本会话改）：`STM32MP157DACX_RAM.ld` 有 SRAM3_ipc_shm 0x10040000/64K + `__OPENAMP_region_*` + 空 `.resource_table` 段（regen 后 rsc_table.c 填充）。
  - A7 侧配对要点：core0_service 对通道名零假设，只要 ttyRPMSG0 出现+帧格式一致；**A7 重连后必发 0x13**（bridge 已能随时答 0x23）；A7 1s 无帧判 LINK_DOWN。
### core0_service（A7-0 Linux 侧 · 第3步 RPMSG，2026-09-07 代码就绪待板验）
- **✅ 代码已落地 `core0_service/`**（P3-05~P3-11，协议已拷入 `docs/protocols.md` §3，母本 §2 同步）：`rpmsg/rpmsg_proto.{c,h}`（CRC16=XMODEM、组帧、增量拆帧状态机：粘包/坏帧重同步/seq 按 type 丢帧统计、0x21/0x22/0x23 payload 解码）、`rpmsg/rpmsg_link.{c,h}`（通道层：open `/dev/ttyRPMSG0` O_RDWR|NOCTTY|NONBLOCK + termios raw；单 RX 线程 poll(fd+wakePipe)；双向心跳 0x7E/500ms（1B 序号）；**任何有效帧保鲜，1s 无帧→LINK_DOWN→按 reopen_ms 重开→清 RX 旧 seq→自动发 0x13 重同步**；0x23 快照 `rpmsg_link_get_m4_state`；发送线程安全、用户回调在锁外派发（回调内可再 send））、`rpmsg/rpmsg_demo.c`（联调 demo：o/c/q/s/x + 每 2s 统计）、`tools/rpmsg_cli/rpmsg_cli.c`（打桩 CLI：tx/raw/bad(坏CRC注入)/stats）、`tools/load_m4.sh`（P3-05：remoteproc start/stop/status + 等 ttyRPMSG0）、`Makefile`（CROSS_COMPILE 可交叉；`make host-check` 只做语法检查）。
- 使用备忘：业务只消费 `on_frame`/`on_link` 回调与快照（不碰 fd）；`0x21` = **接口 v2 语义事件 9B**（`code|arg u16 LE|status|node_id|tick u32 LE`），事件码与 CAN 0x200 `d[0]` 同一张表（`docs/protocols.md` §1）。**v1 的"17B CAN 原始透传"已废弃**（收到 `len=17` 会明确报 `v1 CAN passthrough? reflash M4`）。
- **✅✅ core0 业务守护（P4-01~P4-08）已真编译+自测通过（2026-09-11）**：① 新增 `tools/hostcheck/`（Linux 头文件桩，**仅 Win 语法检查**，不参与板上构建）+ 补全此前完全缺失的 Makefile 构建规则；**13/13 源文件 `-Wall -Wextra` 全过**；② 修 3 个真 bug（`ipc_shm.c` 缺 `<stdio.h>`、`core0_main.c` 缺 `<sys/mman.h>`、`rpmsg_cli.c` 两处 UB）+ 补 S8（10 轮进出计数/重复边沿不重计/出口递减/0 限幅）；③ shm 契约 v2→**v3**（新增跨进程事件字 `evt_bits_c0/c1`+`evt_seq_c0/c1`——根因**匿名 eventfd 不可跨进程**；**编译期布局断言** `sizeof==76`/偏移 48/52/56/60/68；`core1_ui/.../park_shm.h` 逐字节一致）；④ `docs/protocols.md` 拷入 §4 park_shm(v3)。**⛔ 真机 G4 仍卡在 Core1(Qt) 侧（属第6步）**：审计确认 `ipc_reader.cpp` 目前 **`O_RDONLY`+`PROT_READ` 且一个字段都不写** → 心跳/结果/远程开闸请求永远写不进 → Core0 3s 判 **Core1 失联 → 车辆到位直接跳过识别**；其结构体还缺 v2/v3 尾字段（`fault_bits`/`conf_threshold`/`req_*`）、无事件码分发、版本不符**静默无日志**、真实结果不弹卡。**本步按 spec 5.6.3 用 `core1_stub` 打桩验收即可**；板上 recipe：book 交叉编译（`make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-`）→ scp → `./core0_business --conf core0.conf` + `./core1_stub`（r/o/c/k/h/x）→ 看 `[biz]` 状态迁移与 `[slots]` 日志。**改动全在工作区未提交**。**补（2026-09-11 续）**：新建 `core0_service/G4_ACCEPTANCE.md`（L1 宿主逻辑 `make selftest` 142 项 / L2 板端离线 core0+core1_stub 十项检查 / L3 板端端到端 G4-A~G4-I 用例表，含逐条期望日志原文与 KPI 量测）+ `core0_service/sample_core0.conf`（示例配置，ASCII 白名单样例）；自测加 S13（日志级别过滤 + 文件目标 + 存储默认关为 no-op），并加跑 `-DENABLE_STORAGE=1` 版——两版各 **142 项 0 失败**；`PhaseMd/05_第4步_Linux本地业务.md` 已按 spec 重写同步（删 Modbus-TCP、P4-05 改「本地配置管理与上下位控制」、P4-01~P4-08 标 ✅代码 + G4 勾选、避坑补 park_shm v3/事件字契约与"Core1 必须 O_RDWR"）。**交叉编译三坑（2026-09-11 板上首编暴露）**：① Makefile 用 `$(CROSS)$(CC)` 但指令写 `CROSS_COMPILE=...` → 传参无效仍用宿主 gcc；已改 `CROSS_COMPILE ?=` + `CROSS ?= $(CROSS_COMPILE)`（`-n` 验证过）；② `app_config.h` 用 `size_t` 未 include → 真 arm glibc 报 `unknown type name`（**`host-check-win` 漏掉**），已补 `#include <stddef.h>`；另修 `core0_main.c` timerfd `read()` 的 `-Wunused-result`；③ 链接 `undefined reference to shm_open`（旧 glibc<2.34 在 **librt**）→ 加 `-lrt` 且**只在非 Windows 主机加**（MinGW 无 librt，否则宿主 selftest 也挂）。**教训：`host-check-win` 只是语法体检，不能替代真交叉编译**。**④ 队列唤醒缺失（真机首跑暴露，严重）**：`decode_frame/decode_link` 只 `queue_push`、**从不写 `queue_efd`**，而主循环只在 efd 可读时才 `queue_pop` ⇒ **RX 线程收到的所有帧（link UP/DOWN、0x21 车到位、0x22 节点、0x23 状态）静默积压在队列里、业务侧永远收不到**（症状=日志无 `[rpmsg] link UP`；宿主 selftest 直接调 `biz_post` 绕过队列，故未抓到）。已修：`queue_efd` 创建提前、`biz_queue_t` 加 `notify_fd`、`queue_push` 末尾 `ipc_evt_notify(notify_fd,1)` 唤醒 epoll。**⑤ `VMIN=0/VTIME=0` 导致链路每秒 UP/DOWN（真机第二次跑暴露，已修）**：`rpmsg_link.c` 的 `cfg_raw()` 设了 `VMIN=0/VTIME=0` → 内核 `n_tty_read()` 算出 `timeout=0` 命中 `if (!timeout) break;` **返回 0**，而该判断**排在 `O_NONBLOCK→-EAGAIN` 之前** ⇒ **空缓冲 read() 也返回 0**，本层当 EOF → `open→UP→"read EOF"→DOWN` 每 1s 循环（板验日志 `[link] /dev/ttyRPMSG0 read EOF -> DOWN` ×N，设备其实一直在）。**修法 = 不碰 VMIN/VTIME**（保持 cfmakeraw 的 VMIN=1/VTIME=0，配合 O_NONBLOCK 才返回 EAGAIN）。**教训：C 链路层此前从未在板上跑过（G3 验收用的是 python3 脚本），首次真跑就连爆两个 bug**。**

### C8T6 下位机工程（`c8t6/`，STM32F103C8T6，CubeIDE-F1）
- 已建 `c8t6/c8t6.ioc`。**CAN1**（bxCAN，经典 CAN）：时钟=**APB1=36MHz**（SYSCLK 72M /2），**Prescaler=9、Seg1=5、Seg2=2 → 500 kbit/s**（默认 6 是 750k，需改成 9）。引脚 **PA11=RX / PA12=TX → TJA1050**；模式 Normal。
- **FreeRTOS(CMSIS_V2) 已启用**，任务：`defaultTask`、`CAN_Rx_Task`、`BH1750_Task`、`OLED_Task`；队列：`BhDataQueue`（Item `uint16_t`，放光强值）。
- **⚠️ 堆必须开到 ≥10240**：CMSIS_V2 默认 `configTOTAL_HEAP_SIZE`=4096，4 任务+队列用 4172B → `HEAP STILL AVAILABLE=0`（红字）。在 FreeRTOS→Config parameters→`configTOTAL_HEAP_SIZE` 改 **10240**；若仍紧可删 `defaultTask`、把 OLED 任务栈降到 128 words。
- 现节点外设 = **BH1750（I2C1, 0x23/0x5C）+ “led屏”（0.96 OLED SSD1306, I2C, 0x3C）**，两者**共用 F103 的 I2C1**（地址不同不冲突）；**SG90 任务（TIM PWM）后加**。
- **✅ BH1750+OLED 业务代码已完成**：新增 `Core/{Inc,Src}/bh1750.{c,h}`（连续H分辨率 0x10，`BH1750_ReadLux` 返回 lx=raw/1.2；错误哨兵 `BH1750_ERR_VALUE=0xFFFF`）和 `Core/{Inc,Src}/ssd1306.{c,h}`（页寻址，6x8 ASCII 字体，1024B 显存，`SSD1306_WriteStringPadded` 整行覆盖防残影）。任务逻辑全在 `freertos.c` 的 USER CODE 段：BH1750_Task 200ms 读数 Put 队列（满则丢旧保最新）、`OledMutex` 保护 OLED。显示布局用页坐标宏 `OLED_PAGE_TITLE/LUX/GATE/CAN`。堆当前为 **9600**（用户已调，够 4 任务+队列）。新建 .c 属 Core/Src 源文件夹，CubeIDE 自动编译，无需手动加入工程。
- **⭐ I2C 已整体切换为软件模拟（硬件 I2C1 点不亮 OLED 的结论）**：硬件 I2C1(PB8/PB9, remap, 100k)+HAL 驱 SSD1306 黑屏；用户参考工程 `D:\Projects\Project_c8t6+oled_Check`（Keil5，江协式 bit-bang，同引脚、地址 0x78）显示正常 → 结论为 F103 硬件 I2C 路径不可靠，接线/模块无问题。已改：新增 `Core/{Inc,Src}/sw_i2c.{c,h}`（PB8=SCL/PB9=SDA GPIO 开漏、无延时、忽略 ACK，时序复刻参考工程）；`ssd1306.c` 与 `bh1750.c` 的传输层全部改为 sw_i2c（bh1750 读为 START|0x47|ACK+NACK 两字节）；`SSD1306_Init` 开头调 `SW_I2C_Init()` 把 PB8/PB9 从 I2C1 AF 配置 reclaim 回 GPIO OD，**此后 hi2c1/MX_I2C1_Init 保留但不可用**。软件 I2C 无法检测器件在位，`SSD1306_Init` 恒返回 HAL_OK（freertos.c 里的 init-fail 自挂起分支成死代码，无害）。
- **⭐ 设计功能升级（2026-09-09，方案已批，代码落地待真机验证）**：① C8T6 行1 标题 HelloWorld → **`PARK NODE`**（PhaseMd/02 P1-07 设计稿，config.h `OLED_TITLE_STR`，splash 与 OLED_Task 同源）；② **SG90 道闸执行**（P1-06/P2-02 落地）：新增手写 `c8t6/Core/{Inc,Src}/tim.{c,h}`（TIM2_CH1 PWM PA0，PSC71/ARR19999=50Hz，CCR=µs）与 `gate.{c,h}`（`Gate_Init` PWM Start+关位+上电自检 0°→90°→0°，`GATE_SELFTEST` 稳定后置 0；`Gate_Poll` 10ms 缓动 50µs/步≈0.2s 行程防抖动；**状态位 bit0/OLED 行3 改由执行到位 `Gate_IsOpen` 驱动**，不再即时改逻辑位，动作中 OLED 行3 显示 `Gate:MOVE`）；③ M4 正式化：`CAN_MASTER_DEBUG_AUTO_GATE_MS` 3000→**0**（去自动开闸后门，指令只走 RequestCmd/调试器/后续 RPMSG），defaultTask LED 由纯心跳升级**业务指示灯**（C8T6 在线→PA10 1Hz 心跳，离线 3s→5Hz 快闪）。⚠️ regen：CubeMX 重生成需在 .ioc 勾 TIM2 PWM CH1(PA0) 否则 tim.c 丢；SG90 须独立 5~6V 供电共地（PhaseMd/02 避坑）。**已本地提交（2026-09-09，未推云端，本地领先 origin 3 个）：`a45ef78` docs / `0d2f97d` feat(c8t6) / `ad68d87` feat(m4_fw)**；待双端重编烧录真机验证；舵机运行平稳后把 `GATE_SELFTEST` 置 0 再补一版提交。
- **✅ CAN 2.0 从节点功能已落地（2026-09-06，代码就绪待烧录；文档 `c8t6/can.md`）**：① 新增 `Core/{Inc,Src}/config.h`（遮光阈值/CAN ID/OLED 行宏等收拢，`OLED_LINE_*` 已从 freertos.c 迁入）；② 新增 `shade.{c,h}` = 百分比掉点遮光状态机（P1-05 落地，基线 EMA+回滞去抖，返回 ±1 边沿）；③ 新增 `can_node.{c,h}` = 从节点模块：`CAN_Node_Init`(main USER CODE 2，过滤器收 **0x1xx 段** + Start)、`CAN_Node_Poll`(CAN_Rx_Task 10ms 轮询 FIFO0：0x100 开/关闸/查询应答 + 1Hz 0x210 心跳)、`CAN_Node_SendEvent`(**接口 v2** 语义事件 0x200：`ev/arg(大端)/status/tick(大端)`，**无 lux/drop%**；闸位到位发 0x04、故障发 0x03、首次心跳成功后发 0x05)；④ **收路径采用"零中断纯轮询"**（未 ActivateNotification，NVIC 使能无 IER 无副作用）——与 `MD文档/can_standard.md` §3.8/§4 从站经验一致，原先计划的中断回调(`HAL_CAN_RxFifo0MsgPendingCallback`)作后备方案写进了 can.md §5.2；⑤ freertos.c：新增 `CanTxMutexHandle`（事件/心跳两任务 TX 串行化），BH1750_Task 每周期喂状态机+上报边沿、OLED_Task 四行全状态（Lux+drop/Gate/CAN:OK|EVT|ERR）；⑥ `can.c` 手改 `AutoRetransmission=DISABLE`（对齐 .ioc NART=ENABLE，防无 ACK 反复重传→BusOff；regen 会覆盖需核对，文件头有注）；⑦ OLED_Task 384 栈与 .ioc 同步保留，堆仍 9600（多耗 1 把互斥 ~100B，应够）。**待烧录验证**，验收清单见 can.md §7。
- **⭐ 接口 v2（2026-09-11 定版；三端重构；✅已提交 `7dd8458` + 2026-09-12 板验通过）**：**设备抽象=传感器隔离**——CAN 只传语义。① **0x200**=`ev(1)|arg(2 大端)|status(1)|tick(4 大端)`，码 `0x01 车到位/0x02 车离开/0x03 节点故障(arg 1 传感器)/0x04 闸位(执行到位才发)/0x05 就绪`；**lux/drop% 只上本节点 OLED，绝不上总线**。② **0x100**=`cmd|arg|seq|rsv`（0x03 查询→立即回 0x210；0x10 语义档位 1..5；**不回执**）。③ **0x110 只主端发**（评审揪出：初稿让从端回"指令确认"是方向错误——0x1xx 是主→从段、M4 过滤器只收 0x200~0x2FF，发出去无人收，已删）。④ **0x210**=`status|uptime_ms(大端)|dev_type|fw_ver|node_id`（=0x01/0x20/0x01，节点身份随帧）。⑤ RPMSG **0x21=9B 语义事件**（`code|arg u16 LE|status|node_id|tick u32 LE`，事件码与 CAN 同表、大端→小端；Core0 见 len=17 会报"v1 未升级"）。**验证**：host-check-win OK / selftest **158 项 0 失败**(新 S14) / check_static PASS / 三对镜像 body-identical；**M4+C8T6 无 arm 工具链不能本地编译 ⇒ 必须 CubeIDE 重编重烧（只改一端会明报）**。已同步 `protocols §1(含新 §1.0 隔离原则)`+§3、PhaseMd/10·03·02·00·04·05、can.md、rpmsg.md、G4_ACCEPTANCE、rpmsg_link_test.py(9B 解析)、README/Task(P4-07 改"轻量文件记录，不上 SQLite")。**2026-09-12 板验通过**（C8T6+M4 已重烧、core0 v2 已装）：验收栏已回填 `can.md §7`/`rpmsg.md §5`/PhaseMd 02·03·04·05；**仍未验 3 项**＝抓帧对字段与档位下发（缺 CAN 分析仪）、坏 CRC/10min soak（未编 `rpmsg_cli`）、C8T6 断电 0x22 边沿（未专项测）。
- **📋 2026-09-06 依 `MD文档/can_standard.md` 审计 c8t6**（结论全文见 `c8t6/can.md` §10 附录 A）：主条款全合规（500k/采样点75%/NART/ABOM/零中断轮询/心跳比例）；**整改 2 项** = 收帧 `DLC!=8` 丢弃 + 单轮 drain 上限 16（`CAN_RX_DRAIN_MAX`，can_node.c `CAN_Node_Poll`）；**有意差异已文档化** = 初始化位置在 main USER CODE 2（规范是任务首行+DeInit，PhaseMd/03 既定）、发送用 HAL 提交式而非 TxStatus 轮询+缓存重试、SJW=1(vs 2)；**建议项(未做)** = XOR 校验和需 M4 同步、事件帧失败重发、Start 失败 INRQ 重试。

## 五、git / 环境 / 工具备忘
- git 在 `C:\Program Files\Git\cmd\git.exe`（不在 PATH）。远程 `https://github.com/randomlyiii/Project_EdgeParking-CloudSync.git`，分支 master。
- **git 现状（2026-09-11 收工）**：本轮（第7步真机跑通后的运维/排障迭代）分 5 次提交——`6f05d92` feat(deploy) 开机自动联网 wifi-up + 校时加固 park-clock.timer / `0c226b2` feat(core1) 网络页签三动作(保存/联网/连接)+诊断页改时间+WIFI 芯片不上屏 IP / `27abcc0` fix(core1) 云端失败分类(no network/certificate)+空响应诊断 / `6995cc8` chore(privacy/docs) sample_ 约定+清空占位目录+门禁 9d~9h / `40e7812` docs 部署+验收+记忆。更早的三次第7步提交见 `339ff0d`/`2919121`/`a19de5d`，`84d3b66` = 云端 API 测试工具（P7-01）。**接口 v2 轮按功能隔离 4 提交（已 push）**：`84e2c99` docs / `7dd8458` feat(接口 v2，09-12 板验通过) / `c44f60d` docs(Linux_standard) / chore(memory)；其后用户自加 `bb77cbb`——**只改了 IDE `.settings` env-hash + `install_all.sh` 的 M4ELF 查找顺序，提交信息说的"M4 固件 bug"在 diff 里不存在，待确认**。**`git push` 可用**（SSH 免密；若失败加 `GIT_SSH_COMMAND="ssh -o BatchMode=yes"` 看报错）；远程 = `git@github.com:randomlyiii/Project_EdgeParking-CloudSync.git`（SSH），分支 master。
- **🧩 业务隔离（2026-09-11 用户要求）**：**一个功能 = 一个关注点 = 一次提交**；不同功能的代码放各自模块/文件、不混改；文档改动按功能分文件（同文件不可避免时用 `git add -p` 按 hunk 拆）；提交前先按功能分组自查 `git diff HEAD --stat`。
- **⛔ 提交纪律（2026-09-11 用户明确要求）**：**不要自动 git 提交**——改完代码只留工作区，等用户明确说"提交/commit"才提交。**提交信息临时文件写到仓库外**（本次用 `$env:TEMP\parkmsgN.txt` + `git commit -F`，纯 ASCII 路径、中文不进命令行）——曾把 `.git-commit-msg.tmp` add 进 `8ba0ebe`，只能再补 chore 删它。（K210 接入那次 `7d0bc26` 曾被 reset 撤销，其内容后已入库；`k210_fw/参考代码-车牌识别实验/` 三模型与 `core1_ui/ca.pem` 仍不入库，后者已进 `.gitignore`。）
- 框架目录（`k210_fw`、`m4_fw`、`core0_service`、`core1_ui`、`docs`、`deploy`）**均已入库**，空叶子有 `.gitkeep`；`.gitignore` 已补 `cloud.conf*/cloud.env/key.txt` 与 CA 本地副本规则（用户亦改过）。**`.ioc` 重生成、板端构建产物、`/etc/park/*`、`core1_ui/ca.pem` 一律不入库。**
- **🔐 样例/隐私文件约定（2026-09-11 用户要求"扫描隐私配置 + 建 sample_"）**：仓库只放 **`sample_<原名>`** 模板（纯 ASCII + 占位值，入库），真文件一律 gitignore——`/etc/park/cloud.conf`(`deploy/sample_cloud.conf`，API key)、`/etc/wpa_supplicant.conf`(`deploy/sample_wpa_supplicant.conf`，**WiFi PSK**)、`/etc/park-ui.env`(`deploy/sample_park-ui.env`)、`/opt/core0/core0.conf`(`core0_service/sample_core0.conf`)、`云端API调用测试参考/key.txt`↔`sample_key.txt`（原有，命名由此而来）。**旧 `.example` 命名已全部改名**（`install_all.sh`/README/G4/G7/代码注释同步）。`check_static.py` 第 8 节门禁：sample 必须存在+纯 ASCII+无 `sk-` 真 key/非占位 `psk=`/`192.168.*`、env 样例里每个 `PARK_*` 必须真被源码读、`.gitignore` 必须覆盖所有真文件名（**顺带发现 `PARK_UI_WIFI` 只写在文档里、main.cpp 从未 setInterface，已补这行**；负向测试：往 sample 填真 PSK 会 FAIL）。
- 参考资料：`E:\download\100ASK-MP157\100ask-mp157原理图\01_Base_board(底板)\`（原理图 pdf + `.DSN` + `.brd`）。**`.DSN` 可用 grep 搜明文网表/备注；两个 PDF 文字被压缩、且本环境无 PDF 渲染/转换工具**（`pwsh` 读 E: 二进制被沙箱挡、curl schannel 拉不下来 poppler）。
- 清理脚本 `cubekill.bat`（`c8t6/` 与 `m4_fw/` 各一份，内容相同）：通用 CubeMX/CubeIDE 产物清理。用法 `cubekill.bat [目标目录]`；递归识别构建目录（`objects.list`/`CMakeCache.txt` 标记，或 Debug/Release/build 且真有 `*.o/*.mk`）；另清 `*.bak/*.tmp`。不碰 `.ioc/.project/.cproject/.settings/Core/Drivers/Middlewares/*.ld`。m4_fw 构建产物在 `CM4/Debug/`。
- 本环境无 arm-none-eabi 工具链，**无法本地编译验证** STM32 工程；构建/烧录在用户 STM32CubeIDE 完成。
- **⛔ Linux 端(板端粘贴/运行)代码必须纯 ASCII——注释、print、报错字符串一律英文，禁止中文**（用户 2026-09-08 定规）。其 SSH 终端粘贴中文必坏（UTF-8 字节被截/转码），python3 报 `Non-UTF-8 code starting with '\xe8'` / `invalid character in identifier`；给板子的 heredoc/单行脚本即使注释也不能含中文。仓库 .md/.c 说明文档不受此限。另：板端 root 无 sudo；`/tmp` 重启即清，一次性测试用 `python3 - <<'PYEOF'` 即贴即跑最稳。

## 六、下一步建议（优先级）
1. **K210 线（2026-09-16）**：**端到端全程通 + 用户真机确认能正常识别车牌** ⇒ 识别这条线的卡点已消（见 §八 `🚀`/`🧪`）。**剩余三件见 §八 `✅🎯`**（ROI 标定回仓库／日志可见性／长时间堆取证）。**SD 提速：已放弃**（200k 也挂，见 §八 🔌）。**裸 `MemoryError:`（无 `[MEM]`）= GC 堆被压坏** → `k210_fw/gc_restore.py` 救。**改代码一律走宿主回归**（`k210_fw/tools/` **10** 个 test + `build_main.py --check`），别再往板上堆探针文件。
2. **✅ 第2/3步已通（M4↔C8T6 + Linux↔M4 RPMSG）**：0x100/0x110/0x200/0x210 全闭环；G3 全链路（A7 RPMSG→M4→CAN→C8T6 闸门）2026-09-10 验收通过。**⚠️ 早期"can0 down + unbind `4400f000.can`"的释放流程已作废**，正确 recipe 见 §四·G3 条。**遗留**：① C8T6 在手时补测手遮→0x21 与断链 0x22；② 正式版 M4 重编（宏已置 0，板端仍是验证版）；（可选）读 `g_fdcan_meas_hz` 固化时钟记录。
3. C8T6 侧 CAN 已全闭环（心跳/查询/遮光事件/0x110 确认）；**SG90 已挂载（09-09 设计功能）**；BH1750 物理验收 + 遮光阈值现场微调见 `c8t6/can.md` §7。
4. **git / 收工状态（2026-09-11 末）**：板端重启后功能测试全通过 → 已提交并推送（见五·git 现状）。**下次开工**：① `park_ui` 有新改动仍需 `sh tools/build_arm.sh` 重编再 scp；② 欠着的 P6-05/06/07 埋点/五场景；③ `core0_service` 改动需 book 交叉编译后 `install_all.sh`。

## 七、Qt GUI 2026-09-09 状态（当前收工点）
- **✅ Qt GUI 已用板端匹配版本编译成功**：`core1_ui/qt_gui/bin/park_ui` 已在 PC Linux 上生成 ARM 32-bit ELF；不能使用 OpenSTLinux SDK 的 Qt 5.14.1 编译，否则板端 Qt 5.12.8 启动时报 `QtPrivate::argToQString ... version Qt_5`。
- **✅ 正确编译工具链**：使用 `/home/book/100ask_stm32mp157_pro-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin/qmake`（Qt 5.12.8）和同目录 `arm-buildroot-linux-gnueabihf-g++`；Makefile 曾残留 `/home/book/stm32mp157/ST-Buildroot/output/...` 旧绝对路径，已临时替换为当前 SDK 路径后链接成功。当前 SDK 的 qmake 需要 `local_features/force_asserts.prf` 空文件绕过缺失 feature，且环境变量/qt.conf 处理不当会混入 OpenSTLinux 编译器和 sysroot。
- **✅ 部署路径已确认**：PC Linux 编译机执行 `scp /home/book/core1_ui/qt_gui/bin/park_ui root@192.168.189.65:/root/park_ui`；板端再 `mkdir -p /opt/park_ui && cp /root/park_ui /opt/park_ui/park_ui && chmod +x /opt/park_ui/park_ui`。此前 `/home/book/park_ui` 不存在导致板端继续运行旧二进制，已修正。
- **板端 Qt/LCD 事实**：Qt 运行库为 5.12.8；插件目录是 `/usr/lib/qt/plugins/platforms`，不是 `/usr/lib/qt5/plugins/platforms`；目录中有 `libqlinuxfb.so`，`/dev/fb0` 存在，驱动名 `stmdrmfb`，分辨率 `1024x600`、16bpp。推荐环境：`QT_QPA_PLATFORM='linuxfb:fb=/dev/fb0'`、`QT_QPA_PLATFORM_PLUGIN_PATH=/usr/lib/qt/plugins/platforms`、`LD_LIBRARY_PATH=/usr/lib`。
- **✅ LCD 已由 park_ui 独占常显（2026-09-10 板验，deploy systemd 自启直达）**：开机 → park-ui.service → park_ui linuxfb 全屏常显，重启验证通过。屏霸 = **myir.service**（"myir hmi v2.0"，单元 `/usr/lib/systemd/system/myir.service`，ExecStart=`/bin/sh /usr/bin/start.sh start` → mxapp2 `-platform eglfs` 独占 DRM；**grep "mxapp" 搜不到它——单元文件只写 start.sh**）；禁用法 `systemctl disable --now myir.service`（已固化进 `deploy/systemd/install_park_ui.sh`）；park-ui 的 mxapp2 ExecStartPre `pkill -9 -f mxapp2`（勿带全路径、需 -9）**在板端是空操作——没有 `pgrep`/`pkill`**（已改为 `/proc` 扫描；**⚠️ 但禁 myir 会连带关掉 USB Host 供电，见文末 board-power 条**）。**1/4 屏（09-11 定案，旧"缺 showFullScreen"说法作废）**= linuxfb 无 WM 时 `showFullScreen()` 不改窗口几何，窗口停在 sizeHint（实测 `PAINTED bbox x=[0..383] y=[0..301]`＝布局算式吻合；与 DPI/驱动无关，驱动 1024x600/16bpp/line_length2048 正常）⇒ 已改 `mainwindow.cpp` linuxfb 分支显式 `setGeometry(screen()->geometry())` 并打印窗口尺寸。**K210 方向**：定案见下文「方向最终定案」；防伪标记 `strings /opt/park_ui/park_ui | grep K210-ORIENT` 保留。**识别 = 持续周期识别 + console 结果行上行**（无 UART 引出 ⇒ 放弃"下行 0xC1 触发"）：`RECOG_PERIOD_MS=3000`，Qt 端解析 `K2:OK:`/`K2:NG:` → recogResult/recogFailed → 弹卡。**DNK210 车牌模型**（厂家三件套在 `k210_fw/参考代码-车牌识别实验/`；**部署 = SD `/sd/KPU/` 下三文件**）要 **Lite 版 CanMV 固件** + DNK210 KPU 扩展 API（`init_yolo2`/`regionlayer_yolo2`/`lp_recog_load_weight_data`/`lp_recog`/`pix_to_ai`），输出 **UTF-8 中文车牌**（PROVINCE_ZH 把 pinyin 标签映射成 粤/京…，合 protocols §2 口径）。**模型已能加载且每帧推理 ⇒ 见 `🧪`。**识别主责口径已更正：K210 本地 KPU 主责、云端 DeepSeek 兜底（README/Task 待同步）。**下一主体（第4步）**：Core0 业务守护——读 /park_shm 车牌(core1 写) + M4 车到位(RPMSG 0x21) → 判定 → RPMSG 0x11 开闸。
- **✅ deploy 层已扩成「全链路启动」（2026-09-11）**：新增 `deploy/systemd/m4-load.service`（oneshot：跑 `load_m4.sh start`，内含 ensure_can_clock + driver_override/bind；`SuccessExitStatus=0 1 2` 容忍 M4 flaky）+ `core0-bus.service`（`/opt/core0/core0_business -c /opt/core0/core0.conf`、`Restart=always`、缺 ttyRPMSG0 也能起）+ 更新 `park-ui.service`（`After/Wants=m4-load,core0-bus`）；**启动链（09-16 起）= board-power → m4-load → core0-bus → park-ui，WiFi/时钟不在这条链上**（见 §八 `🌐`）。新增 `install_all.sh`（布局 `/opt/core0/{core0_business,core0.conf,tools/load_m4.sh,tools/rpmsg_link_test.py}` + `/opt/park_ui/park_ui` + `/lib/firmware/m4_fw.elf`；**不覆盖已存在的 core0.conf**；开关 `INSTALL_M4/CORE0/UI=0`、`CORE0_SRC/UI_SRC/M4_FW=`），旧 `install_park_ui.sh` 改为薄壳（UI-only 兼容）；脚本 `sh -n` 过、纯 ASCII。**⛔ 前置缺口：core0_business 还没在 book 交叉编译**（`cd ~/core0_service && make CROSS_COMPILE=arm-buildroot-linux-gnueabihf- core0_business`），否则 install_all 会跳过 core0 unit。
- **📦 K210 细节已归档**（`docs/AGENTS_history.md` §K210-2026-09-11）：**方向定案 = 只在固件 `capture_frame()` 里软件 `img.replace()` 翻一次**（`CAM_SW_HMIRROR=True/CAM_SW_VFLIP=False`），Qt 侧不翻，运行时用 `/etc/park-ui.env` 的 `PARK_UI_K210_FLIP=none|hmirror|flip180|vflip` 改；`/flash/main.py` 必须用 CanMV IDE「保存到设备」才驻板（点“运行”不写盘）；厂家资料包/固件/KPU 三模型在 `E:\download\k210`（与我们手上的逐字节相同）。
- **⭐ 策略定案：固定 ROI 直识别（2026-09-15，已实现）**：闸机前是**固定机位**，车牌出现区域基本不变 ⇒ **不加载 YOLOv2 检测模型**，直接对固定框跑识别模型。收益：省 `lp_detect.kmodel` 460456B + `init_yolo2` ~68KB 系统堆（正是 `jpeg` OOM 那一侧的根因）、每周期少一次全图推理、**根除 `det=0`**。实现：`ROI_MODE=1`（默认）+ **相对坐标** `ROI_X/Y/W/H`（0~1，QVGA/QQVGA 通用、越界自动钳位）+ `ROI_EXTEND=0`（人工框**不再外扩**——识别模型吃的是"车牌刚好占满 208x64"的图，外扩只会让字更小）；`kpu_load_detect()` 在 ROI_MODE 直接返回；预览上 **ROI 框画黄色**（检测框绿色）供现场调框。**三条约束**：① ROI 必须**紧到车牌基本占满它**（太松 ⇒ 字太小 ⇒ 认不出；先大后小收紧）；② 抓拍点/停车位要稳定，车距变化大就回 `ROI_MODE=0` 双模型；③ 模型搜索在 ROI_MODE 下**只要求 recog+weight**（原先 `min()` 要三件齐全 ⇒ 被省掉的 det 反成硬依赖，已修）。回归 → `test_plate_decode.py` §7。
- **🧪 固定 ROI 两次真机跑（2026-09-15）**：**首跑** = 搜索修复生效（`recog=697512 weight=1498500`）+ 预览流修好，但每轮 `[DBG] recog_box: TypeError("unsupported types for __mod__: '', 'tuple'")`；**根因不在我们**（AST 扫过：`%` 左侧全是字面量格式串），已按厂家例程补**每轮 `del rimg`**。**第二跑**：100kHz 下 recog 65s / weight 140s 全程通、识别每帧在跑（`ms=9~15`），**但 40 帧全 `NG err=no_plate`**。**两条关键读数**：① **`det=1` 是结构值**——ROI_MODE 下 `lps` 写死一个框，`det` 恒为 1；**没有 `[DET]` 行**才证明检测模型没加载。② crop/run/out 全成功 ⇒ 遇到 `no_plate` 必须分清 `best is None`（模型没吐东西）与 `best[0] < RECOG_CONF_TH`（认出来了但分不够，对策相反）⇒ 证据行 `best=… conf=… th=…`。
- **🔬 "识别第一轮就早死"的定位装置（2026-09-15）**：框循环 `stage` 逐段赋值（extend→crop→run→out→decode），**两次 C 调用各自 `except`**（tag `recog_run`/`recog_out` + crop 尺寸），外层 `recog_box: stage=…`；三条都带 `_mem2()` = `free=<GC堆> sysfree=<系统堆>`（绝不抛）⇒ 日志能分开"C 层自己抛"与"堆被压坏后对象成垃圾"。回归 → `test_plate_decode.py` §8。**＋09-16 晚**：板子上比 IDE 更容易 `recog_crop MemoryError`（`free=354784` 却要不到 55KB）⇒ 失败行带 `stage=/want=/probe_cut/probe_recog`（`_gc_probe` 试分配：分开 GC 堆碎／系统堆碎）；`[RECOG]` 加 `cropfail=N`（>0 = 模型没拿到图）；切图刚好 208x64 跳过 resize。→ §11。**＋09-16 深夜**：`probe_cut=0`+`probe_recog=1`+`free=378K` = **碎片**（GC 不压缩、collect 无效；**不是泄漏**）⇒ 内存正解 = **GC 堆 768K**；`[CFG] build=/app=B/roi=/conf_th=` 指纹（板上 ROI (15,71,288,95) vs 仓库 (16,72,288,96) ⇒ **板上可以是另一份文件**）。→ §12。**09-16 深夜二（含一次我犯的错，已改回）**：失败点曾移到 `stage=pix_to_ai` ⇒ ① 启动器 `GC_SET`→**768K** ② 每次大分配前 `_gc_collect()` ③ **⛔ 我加的 `CROP_MAX_PIXELS` 居中缩框是错的：09-15 能认的版本送的是「整个 ROI」(0.05/0.30/0.90/0.40 → 288×96，宽高比 3.03≈车牌 3.14，车牌基本占满)，居中裁一刀正好切掉车牌两端 ⇒ 取图成功也认不出。**已改回默认 0（关）**，`_cap_box` 只作手动挡。**教训：诊断留下的"顺手优化"改的是模型输入，先问"这会不会改算法输入"。**
- **✅✅ SD 卡读写（2026-09-13 结论仍有效，完整记录已搬去 history §SD-2026-09-13）**：卡/卡座/线材都好，是**驱动时序**问题；正道 = **裸 SPI**（`machine.SPI(1, …sck=27,mosi=28,miso=26,cs0=29)`），卡座在 SPI、固件找 SDIO，所以固件不挂它；`uos.mount(BlockDev)` 实测能成、**无 `uos.register_vfs`**；应答预算**一律毫秒制**（`TOKEN_MS=2000`/`R1_MS=250`，按循环次数算会在提速后悄悄缩水 10 倍 = "卡死"的根源）。**提速见上面 `🔌` 条**。
- **✅ P6-04 Core1 业务写端（2026-09-11 代码 + 板验都通过）**：`core1_ui/qt_gui/src/ipc_writer.{h,cpp}`（规格 `.codeartsdoer/specs/core1_business/spec.md`）= `O_RDWR`+`PROT_WRITE` 挂 `/park_shm`、magic/version 自检、**1s `hb_core1`**、结果回写、低置信/失败 → `cloud_pending=1`、`req_gate_*` 脉冲、200ms 消费 `evt_c0`；触发源 = 底栏**触摸「开闸/关闸」按钮**（板端无键盘，触摸即鼠标）+ `O`/`C` 热键 + 直写 shm。静态门禁 `check_static.py` **PASS**（纯 ASCII + **负向断言“绝不写 Core0 归属字段”**）。**板验**：双向动作正常、闸位跟随无闪变、LCD 上 M4↔C8T6 互动正常。**完整描述 → history §P6-04-2026-09-11**。
- **🐞 远程关闸失效（已修）**：根因 = `biz` 的 `gate_state` 是**观测态**、只由 M4 的 0x23 更新，而 **M4 命令后不主动上报** ⇒ 关闸命中幂等分支、`0x12` 根本没发。修法：① 命令**发送成功后以目标态当工作态**（只在 `rc==0` 时置，失败不撒谎）；② **延迟 0x13 重同步**（`GATE_SETTLE_MS=1600`，**必须 > C8T6 1Hz 心跳 + SG90 0.2s 行程**，否则读回的是上一拍）；③ **周期 0x13**（`M4_RESYNC_PERIOD_MS=2000`，M4 只在被问时才答 0x23）；④ 收到上报即清 `gate_query_at`。**教训：宿主 S7 没抓到，是因为它在开关闸之间注入了自发 0x23 —— 真机没有；已补回归**。用例见 `G4_ACCEPTANCE.md` L3 G4-B2；原文 → history §关闸-2026-09-11。
- **🐞 闸位“闪变”（已修）**：周期/边沿的 0x13 可能**早于命令**发出、带回的是命令前的位置，把刚乐观置上的状态盖掉 ⇒ 新增 **`GATE_REPORT_GUARD_MS=1500` 保护窗**：距上次成功命令 1.5s 内的 0x23 **只采纳 node/can_err、不采纳闸位**（settle 查询 1600ms > 1.5s 正常采纳；`last_cmd_ms` 只在真发命令时更新）。原文 → history §关闸-2026-09-11。
- **🟢 2026-09-11 收工**：core0 入 `/opt/core0`、**VMIN 修复生效**（`[rpmsg] link UP` 稳定）、**LCD 1/4 屏修好**、**闸门链复测通过**（shm 偏移 52/53 → core0 → RPMSG → CAN → 舵机 ✅）；**板端新事实**：跑 Qt 要 `< /dev/null`（否则被 SIGTTIN 停住）、`/etc/profile` 的 `QT_*` 对 systemd 无效、**没有 `pgrep`/`timeout`**。原文 → history §收工-2026-09-11。
- **⚡ USB Host 供电 = GPIO 82/139（2026-09-11 真机定案，重要）**：现象 = K210 预览突然消失、`lsusb` 只剩三个 `1d6b:` root hub、**连 U 盘也不认**（K210 本身没事：PC 上 CanMV IDE 能连）。根因 = **`/usr/bin/start.sh`（`myir.service` 启的）除了 mxapp2，还会把 GPIO 82(PF2) / 139(PI11) 拉高给板载 USB HUB/主机供电**；我们 `systemctl disable --now myir.service` 之后这两脚再没人管，而 **GPIO 输出是保持态** ⇒ 完美解释"硬件没变、重启前一直好好的、重启后突然不行"。**修法**：新增 `deploy/systemd/board-power.service`（oneshot，`After=sysinit.target`、`Before=m4-load/core0-bus/park-ui`，ExecStart 就是那两条 `export/out/value=1` 循环），`install_all.sh` 的 **[7/9]** 步安装并 enable，`park-ui.service` 的 `After/Wants=` 里加上它。**验证**：手动 `echo 1 > value` 后 `lsusb` 立刻多出 `0424:2514`(板载 USB HUB) + `1a86:55d4`(CH9102 = K210 那路串口，走 `cdc_acm` → `/dev/ttyACM0`) + `0bda:8152`(USB 网卡)，park_ui 2s 内自动重连、LCD 预览恢复。`/sys/kernel/debug/gpio` 里这两脚的 consumer 名就叫 **`sysfs`**（无 DT label = 厂家脚本手工 export）。**教训：禁用厂商 HMI/服务单元时，必须查它附带的外设初始化（本次是 USB 供电），不能只看"屏幕归属"。** **✅ 已跨重启验证（2026-09-11）**：`board-power.service` loaded/enabled/**active (exited)**，重启后不用手敲 GPIO，`lsusb` 自动出现 `0424:2514`+`1a86:55d4`，**`/dev/ttyACM0`（c 166,0 = CDC-ACM）就位 ⇒ CH9102 走 `cdc_acm`，不需要 ch341，也不需要 `PARK_UI_TTY` 覆盖**。

## 八、第7步 云端兜底 + LCD 运维面（2026-09-11 真机跑通；✅ 已提交 `339ff0d`/`2919121`/`a19de5d`）
- **⛔ K210 板端操作纪律（2026-09-13，必须遵守）**：串口终端**不能执行命令**（REPL 粘贴一律无效）⇒ 一切验证 = **存 .py 到设备 + 冷启动**；诊断只靠串口打印（**运行期**不能新建文件，但 IDE 上传可自定义文件名）。**绝不用 IDE 的「运行/Run」**：那是软重启，**不归还**上次 KPU 占的内存（实测 `sys_free` 2.5MB→144KB）⇒ 数据全废；main.py 现在会自己认出（`sys_free < 1.8MB`）并跳过模型、打印 `UNPLUG AND REPLUG`。
- **🚀 两段式开机（2026-09-16，已实现）**：`main.py` → `/flash/main.py`（**3.9KB 启动器**）、`park_app.py` → `/flash/park_app.py`（**~122KB 业务**）。**为什么**：122KB 要**先编译**才能跑第一行，编译峰值 ≫ 常驻 ⇒ 堆一被压小，编译器死在**文件里面**、**零输出**（真机："运行但零打印"）⇒ 业务里的 `auto_tune_gc_heap()` 恰在最需要时是死代码，修复只能住在"小到能在坏堆上编译出来"的文件里。启动器：先打一行堆底账（**应用编译失败时这是唯一证据**）→ 堆 <384KB 就写回 512KB + 喊 `POWER-CYCLE`（**写入下次上电才生效** ⇒ 正常是"冷启动两次、零 IDE 操作"）→ 否则先 `sd_probe2.wait()` 挂卡、再 `import park_app` 调 `park_app.main()`。**回滚**：业务保留 `__main__` 守卫，存成 `main.py` 仍能独立启动。**目录约定**：`k210_fw/` 根**只放会被保存到设备的文件**，且**仓库文件名 = 设备文件名**（`main.py`/`park_app.py`/`sd_probe2.py`；救砖的 `gc_restore.py` 是例外，它顶替 `main.py`）；其余全进 `tools/`（`sd_spi_fat.py`/`fw_probe.py` 已移入），业务由 `tools/build_main.py` 生成、**内联驱动块别手改**。⚠️ **`k210_fw/README.md` 已删，不要再创建/改它**。回归 **10 个 test**（新增 `test_boot_launcher.py`：启动器 >4096 B 或内联了业务代码即 FAIL）。
- **📊 GC 堆 / 系统堆的真相（2026-09-13 ⭐ 由 CanMV 1.0.7 **源码**定案，**推翻我此前"两块内存不互相让"的错误结论**）**：`gc_heap_size(N)` 只做两件事——把数字**写进 SPIFFS 的 `freq.conf`** 然后返回；**本次运行的内存划分一个字都不动**（`components/micropython/port/src/Maix/Maix_utils.c`）⇒ 我们看到的 `deferred` 是**设计如此**，**必须重启一次才生效**；开机时 `gc_heap = malloc(config->gc_heap_size)`（`maixpy_main.c`）——**和模型同一个池子** ⇒ **交易是 1:1 真实的**（`malloc` 失败会回退 128KB 并打印 "too large"，所以"过大"能自愈、"过小"会把 main.py 编译死）。默认值 **0xBB9B0 = 768432（750KB）**，但本板**固件默认值是 512KB**。**⭐ 2026-09-14 池子总量实测**：makerobo **FULL** 镜像（v1.0.4）`sys_free 2514944 + gc 475136 = 2990080`；官方 **Lite** 镜像（v1.0.5-4）`sys_free 3784704 + gc 524288 = 4308992` ⇒ **换固件白赚 1.27MB**（K210 无外部 DRAM，固件正文小 520KB 就多 520KB SRAM 给堆）。**main.py 常驻 ~120KB（GC 堆 23%，不占系统堆；"拆多文件"零收益）。**
- **🚑 压小 GC 堆是真旋钮，但下限"未验证"**：压到 253952 后开机只剩 `MemoryError: memory allocation failed, allocating 160 bytes`（**编译期峰值**远大于常驻 113KB）。已知：**404/464/512KB 能起，248KB 起不来**。⇒ 整定函数现在**只读体检**、正常堆一个字节不写（`HEAP_TUNE_GC_MIN=384KB` 只是保守估计）。**救砖 = `k210_fw/gc_restore.py`**（2.8KB、无条件写 512KB）当 main.py 存上去 → 冷启动 → 再存回 main.py（实测救回）。**要再往下压，必须先在板上量出真实编译下限再加余量，不许试值。**
- **🔌 SD：挂载 100kHz；**提速两次都挂 ⇒ `BAUD_TRY=0` 是唯一出厂值****（09-16 实测 200kHz：kmodel 读得动 32.9s ✓，但**第二个文件 `lp_weight.bin` 一 open 就永不返回**——无 `weight_data_size`、无 `SDX` 心跳，与 400k **同一签名** ⇒ 不是"200 行"，是**这块固件的 SPI 驱动经不起"改波特率+再开第二个文件"**。代价：冷启动 ~214s）。原理与三次提速实测（100k/200k/400k = 65154/32959/16753ms）、400k 的 2 成 2 死取证 ⇒ **全文在 history §SD-2026-09-15**。**只留操作要点**：① 挂载永远 100kHz，只动挂载之后那一段；② **100kHz 从不挂但每次开机 ~3.5 min**；③ 变体顺序 `init(full)`→`init(baudrate)`→**`deinit()+new SPI()`（唯一能成的那个，它先释放旧对象）**→`new SPI()`；每个变体前打 `[SD] try <变体> -> <baud>`，**挂死时最后一行就是元凶**；④ 应答预算**一律毫秒制**（`TOKEN_MS=2000`/`R1_MS=250`，按循环次数算会在提速后悄悄缩水 10 倍）；失败扇区重试 3 次；⑤ **"静止 30 秒"不是卡死**：`load_kmodel`/`lp_recog_load_weight_data` 返回前**一个字都不打**（先把整个文件读进来）。判据 = 驱动心跳 `SDX:[SD] read N KB`（**每 64KB** 一行）+ 固件自己打的 `weight_data_size: <n>`（**连它都没有 = 卡在 C 层"打开文件"**，有它没心跳 = 卡在长读里）；⑥ 大段读完后**不再 `stat`**（09-13 它把板子挂死）；⑦ **09-16 起心跳只打 KB、不打 ms**（那个毫秒是 Python 侧围着 C 层的读量的，量不到真实读时间，且 `ticks_ms` 会回绕、计数还跨文件累计）；⑧ **09-16 起没卡就不往下走**：`setup_sd_vfs()` 补上了原先漏掉的 `sd.init()`（CMD0/CMD8/ACMD41/CSD，失败会报是哪一步），并由 `sd_wait_until_mounted()` **有界兜底**（**09-16 晚：`SD_WAIT_MS=2000`×`SD_WAIT_MAX=3`，无限等归启动器 `sd_probe2.wait()`——那时什么都还没起来、重试免费**）；每轮重试前 `_sd_release()` 先 deinit 旧 SPI（同一外设开第二个 SPI 会挂板 = 变体 3 的老教训）。另加排查开关 `SENSOR_RUN`（0=不碰 `sensor.run`/`skip_frames`）。
- **🖥️ 相机/LCD 账（换 Lite 固件后从"三者不可兼得"变成"**全都要也装得下**"）**：模型真实字节数 **det 460456 + recog 697512 + weight 1498500 = 2656468**。**ROI_MODE=1 之后 det 不再加载 ⇒ 模型只占 2196012**，再加 QVGA 相机 ~389KB 与 LCD 面板 155648B 还余 ~1.3MB（旧 FULL 固件的 2.99MB 池子装不下 = 当年 ENOMEM/权重失败的根因，细节 → history）。**`lcd.deinit()` 再重配相机 = 硬故障**（`EPC 0x8006d722`）⇒ LCD 开关在进函数前定死，之后只降相机尺寸（QVGA→QQVGA 兜底）。**默认仍是 `LCD_PREVIEW=False`（屏黑、结论走串口/Qt）**；现在内存够了，想看板载屏就在 main.py 里置 1（厂家例程就是 LCD+QVGA+双模型跑的）。
- **🔁 重试必须封顶**：`KPU_MAX_TRIES=3`、**内存类失败 `permanent=True` 立即放弃**（每 10s 重试会**每次漏一个 KPU 实例**，十几分钟把 GC 堆耗到 6112B），同一条原因只打一次。
- **💾 图片存储上限（2026-09-15 审计；结论 + 细节 → history §K210-2026-09-15b）**：**K210 全文不写文件**（`/flash` 连新建都不行、`/sd` 只读模型）⇒ 预览是**同步 `print()`** 推出去的、**没有队列**，失效模式是**反过来的 fail-stop**（Linux 不读 ⇒ `print()` 阻塞 ⇒ 主循环与识别一起停，reader 回来自愈）。**Qt 侧原本就有界**（`m_latest` 只留最新 1 帧、`m_lineBuf` >64KB 即清、事件条 ≤64 行、`/tmp/park_cloud.jpg` 固定名 + Truncate）；**唯一真缺口已补**：两个重组表加硬上限（64×8KB / 256×4KB），超限丢弃重同步——发布端本来就校验总长 ⇒ 残帧永远不会被当坏图发布。
- **✅🎯 当前进度（2026-09-16）**：**识别已通**（用户真机确认能出车牌）⇒ 六关 ①换 Lite 固件（池子 2.99→**4.31MB**）②固定 ROI（省掉检测模型）③`det=0` 消失 ④SD 退回 100kHz ⑤预览流恢复 ⑥端到端跑通 **全部闭环**；另有 **两段式开机**（见 `🚀`）。**下一步**：① 标定值靠开机 `[CFG]` 行对账（`build=`/`roi=`/`conf_th=`）② 长时间运行堆增长取证。**板端动作**：存 `park_app.py`（+`main.py`）后冷启动。回归 = `k210_fw/tools/` **10** 个 test + `build_main.py --check` 全绿。
- **🖼️ 预览链路的两条硬规矩（09-14 / 09-16）**：① `jpeg_from()` **不许静默吞错**（失败必打 `[DBG] jpeg_fail:`，且**不许**退回裸 RGB565）；② `console_send_jpeg()` **整段 try/except + 限频 `_preview_drop`** —— 真机一帧涨到 **14280 字符**（16 行×25ms）后串口**直接断连、且没有任何 Python 报错**：预览只是给人看的，绝不许掀掉主循环把识别一起带走。配套旋钮（09-16 大帧后调）：`CONSOLE_IMG_EVERY=10`／`JPEG_QUALITY=40`／`GC_PERIOD_MS=1000`（预览每帧造几十 KB 临时垃圾）。硬件 JPEG（`set_pixformat(JPEG)`）新固件值得重测。门禁 = `test_plate_decode.py` §10。
- **📦 K210 两项细节已归档**（原文 → history §det0-2026-09-14 / §相机方向-2026-09-14）：① **`det=0` 读法**（仅 `ROI_MODE=0` 用）：`[DET] n=… top=…` + 把阈值临时降到 0.05，可区分"没候选／低于阈值／检测没跑"；② **相机方向顺序坑**：`set_framesize` 会覆盖传感器的 `set_vflip/hmirror` ⇒ 必须按厂家顺序（framesize → pixformat → vflip），已改，开机自述 `[CAM] 320x240 RGB565 sensor_vflip=… sw_hmirror=…`。
- **需求**（LCD 改云端阈值/要求云检测/换 API+模型名/WiFi 自助）：**细节已归档** → `docs/AGENTS_history.md` §第7步-2026-09-11（5 个新模块 + `G7_ACCEPTANCE.md` + `check_static.py` 门禁）。
**只留操作要点**：① **云代码只在 `core1_ui/qt_gui/src/`**（Core0 禁云，门禁负向断言）；② `/etc/park/cloud.conf`（600，仓库外，模板 `deploy/sample_cloud.conf`，**每个 provider 一把 key**）；③ **`transport=auto|qt|python`** —— Qt 5.12.8+OpenSSL 1.1.1 在 TLS 1.3 上会卡死，实测 `python` 通道 ~1s 拿到 200；④ CA 必须自备 `/etc/park/ca.pem`，且**无 RTC（开机 2020）⇒ 先校时否则证书"尚未生效"**；⑤ `timeout_ms` 上限 ~6000（Core0 只把 3s 延到 6s）；⑥ 本机 IP/网关**不上屏不落盘**（门禁断言）。
- **🐞 布局修复第二轮**：`QSizePolicy::Ignored` 会让状态芯片**全部消失**——`Ignored` 在 Qt 里是"贪婪"语义；正确配方 = **`Preferred` + `setMinimumWidth(0)`**（门禁已断言）。
- **🔧 book 交叉编译**：一切固化在 `core1_ui/qt_gui/tools/build_arm.sh`（补 SDK 软链 → `rm Makefile .qmake.stash` → 显式 `QMAKE_CC/CXX` → `sed --sysroot` → `touch` → `make -j4` → 打印 `strings bin/park_ui | grep -c "python transport"` 防伪）。配方与坑见 `qt_gui/README.md` §3。
- **✅✅✅ 第7步云端真机端到端识别成功（2026-09-11）**：`transport=python` + DeepSeek key **实测识别出车牌** ⇒ P7-01~P7-07 全链路真机验证；**每个 provider 一把 key** 已落地（`CloudSettings::keys` + `cloud_provider_for_base()` + `cloud_settings_set_key/adopt_key()`；`api_key=` 生效 + `key_<provider>=` 各家备份，PRESET/改 URL 自动 adopt，旧配置首次保存自动归属）。细节见 `cloud_settings.h` 注释与 `G7_ACCEPTANCE.md`。
- **🔒 本机地址不上屏/不落盘（2026-09-11 用户要求）**：底栏 WIFI 芯片**只显示 `SSID`**（无租约=`WIFI:down`、租约刚到手但 SSID 未探到=`WIFI:online`），设置页「网络」页签只显示 `iface/state/ssid/signal`；`WifiStatus` 的 `ip`/`gw` 字段与 `parseRoute()`（/proc/net/route）**已删除**，只留布尔 `hasIp`；journal/状态行也不回显地址（`connected to <ssid>`）。静态门禁新增断言：wifi_manager.h 不得有 `QString ip|gw`、不得解析 `/proc/net/route`、全 GUI 源码不得出现 `st.ip/s.ip/st->gw/ip=%/gw=%` 或 `192.168.` 字面量。理由：屏挂在门口，暴露本机地址 = 递刀子。
- **🌐 开机自动联网 + 云失败误报（2026-09-11 用户紧急报障，已修）**：现象①「每次开机得手敲三条厂商命令」②「云端检测失败、屏上显示 python transport」。根因：**拉起 wlan0 原本是厂商桌面 `myir.service` 干的活**，禁用 myir 后没人接手（`park_ui` 只在设置页按 CONNECT 时才动 wlan0），`board-power.service` 只补了 USB 供电 ⇒ 上电无网 → Qt 5s 超时 → 回退 python → DNS 失败 → 被报成 `python transport failed`（**两个症状同一根因**）。修法：新增 `deploy/systemd/wifi_up.sh`（纯 ASCII：`ip link set up` → `/proc` 扫描杀旧 `wpa_supplicant` → `-B -D nl80211 -i wlan0 -c conf` → `udhcpc -n -q -t 5 -T 3` **最多 3 轮** → 只打印租约有无/SSID，**不打印密码与 IP**，恒 `exit 0`）+ `wifi-up.service`（oneshot、`After=board-power`、`EnvironmentFile=-/etc/park-ui.env`，支持 `PARK_UI_WIFI`/`PARK_WPA_CONF`）；`install_all.sh` 重编号 **/9**（新 `[4/9]` 步）；**启动链 09-16 已改，见下条**；设置页诊断页列 6 个 unit。云侧诊断同时修：`CloudClient` 新增 `looksLikeNetworkDown()/cloudFailReason()`（DNS/无路由 ⇒ reason = **`no network`**、含 timeout ⇒ `timeout`，不再一律报 `python transport failed`），`main.cpp` 失败行改为 `cloud: FAILED <reason> (<detail>) [wifi:lease=yes|no ssid=…]`，无租约时事件行追加 **`- no WiFi lease: systemctl restart wifi-up`**。`check_static.py` 第 9 节门禁：wifi_up.sh 存在+纯 ASCII+厂商三步+无 procps 工具+不提 psk、unit 顺序与安装、`no network` 分类与 `wifi:lease=` 提示，并禁止日志再出现 `wifi:ip=`。
- **🌐 WiFi 移出启动关键路径（2026-09-16，修用户报障"wifi 没开就卡死在连接网络"）**：旧链 `wifi-up→park-clock→m4-load→core0-bus→park-ui` 把**本地栈排在网络后面**，且 `wifi_up.sh` 的 3 轮 DHCP（≈50s）在**没有 AP** 时也照跑 ⇒ 黑屏数分钟。三条修法：① `wifi-up`/`park-clock` 的 `Before=` 只剩彼此，`park-ui`/`m4-load`/`core0-bus` **不得依赖 wifi-up**；② 脚本加**关联门**（`wpa_state=COMPLETED` 最多等 12s，否则跳过 DHCP）+ **15s 墙钟 DHCP 截止**，unit `TimeoutStartSec=40`；③ `set_clock.py` 无租约立即退出。门禁 = `check_static.py` 第 9 节（负向：本地栈不得出现在网络单元的 `Before=` 里）。
- **📺 K210 日志上 LCD（2026-09-16，用户要求）**：日志**本来就在 `/dev/ttyACM0` 上**——是 `k210_link.cpp` 的 `feedText()` 只认 `K2:` 行，把 `[BOOT]`/`[MEM]`/`[SD]`/`[KPU]`/`[RECOG]`/`[stat]` 全丢了（"离开 IDE 看不到启动进度"与"死机前最后一行看不到"是同一根因）。现：`feedText()` 加 else → `emit k210Log` → `main.cpp` `qWarning("k210: …")` 进 journald + **筛选后** `pushEvent` 上底栏事件条（`[BOOT]/[MEM]/[SD]/SDX:/[KPU]/[CAM]/[DET]/[GC]/[ROI]` 及含 fail/error/warn 的行）；高频行（`SDX:`/`[stat]`/`[RECOG]`）**限流 5s**，`[RECOG]` **09-16 晚起也上事件条**；只读实时流（开机最初几秒不等）。**改完须 book 上 `sh tools/build_arm.sh` 重编**；门禁 = §5c。
- **📍 云代码位置澄清（2026-09-11 用户问"core0_service/cloud 下没有代码"）**：**唯一云代码在 `core1_ui/qt_gui/src/`**——`cloud_client.{h,cpp}`（Qt Network + python3 回退传输、超时/重试/容错解析/错误分类）、`cloud_settings.{h,cpp}`（`/etc/park/cloud.conf` + 每厂商 key），装配在 `main.cpp`，UI 在 `settingspage.*`/`mainwindow.*`，模板 `deploy/sample_cloud.conf`，测试工具 `云端API调用测试参考/cloud_api_test.py`。**已 `git rm` 两个空占位目录** `core0_service/cloud/.gitkeep` 与 `core1_ui/cloud_api/.gitkeep`（Core0 禁云、原独立 cloud_api 方案作废，只剩 `.gitkeep` 会误导人去找代码）；`README.md` 目录树按实际结构重写，`Task.md`/本地 spec 里 `cloud_api` 字样改为"云端模块（park_ui 内的 cloud_*）"，`check_static.py` 第 9c 节起负向断言（这两个目录再出现即 FAIL）。
- **📦 09-11 晚三项已归档**（LCD `联网` 按钮 / 时钟加固 / 云错误诊断 / 诊断页改时间 / 网络页签 `保存`）原文在 `docs/AGENTS_history.md` §第7步-2026-09-11。**要点**：`联网`=只动链路不写文件；`保存`=只写 `/etc/wpa_supplicant.conf`（`.bak` 只生成一次）；`连接`=写+应用+20s 无租约回滚；诊断页 `同步`/`改时间`（严格正则后 argv 调 `/bin/date -u -s`）；云失败分类 `no network`/`certificate` + `body…: 片段`；门禁 9d~9h。

## 九、RK3588（定昌 DC-A588）板级事实（2026-09-25 调通；详录 `MD文档/rk3588/调试记录.md`）
- **板子**：定昌 **DC-A588**（=飞牛 NAS 同款底板），RK3588 八核 / 8G RAM / **64G eMMC** / 双千兆网口（eth0/eth1）。原厂固件 = GB-RK3588 Ubuntu 24.04 GNOME（5.10.198 vendor 内核）。USB 口分工：**仅 Type-C 口**（otg 控制器 dr_mode=otg）支持设备模式跑 adb；**USB-A 口全部 host-only**（dtb 焊死），且其中一口兼 **MASKROM 救砖口**——A对A 线接上电即进 maskrom，**勿当调试口**。
- **在板固件** = 原厂 + 仅两处修复：① `/etc/init.d/.usb_config` 带前导空格致 `case` 永不命中 → 改精确 `usb_adb_en`（上电自启 adbd）② `/etc/rc.local` `echo host`→`peripheral`（锁死 OTG 设备模式，防开机末段顶掉 gadget）。烧后已配：root 密码 **rk3588**、netplan `192.168.10.2/24` 静态 + `dhcp4` 双配置（持久化）。镜像文件 `D:\BaiduNetdiskDownload\GB-RK3588-gnome-minimal-adb-on.img`。
- **调试通道**（两条均真机验证）：① `adb shell` = A对C 线插板子 Type-C 口，**直接 root**；② `ssh root@192.168.10.2` = 网线直连 PC/笔记本（PC 侧设 `192.168.10.1/24`，**网关/DNS 留空**），插路由器则自动 DHCP 拿第二地址。
- **⭐ 烧录（绕开摘要校验）**：SoCToolKit v2.96"升级固件"模式对**改过字节的固件一律报"固件摘要检查失败"**（无厂商私钥不可重签）；正解 = MarkToolbox 自带经典工具 `C:\ProgramData\MarkToolbox\bin\upgrade_tool.exe`——板子进 MASKROM 后 `upgrade_tool.exe UF <img>`，**不校验摘要，14GB 约 6.5 分钟写完自动重启**。注意：镜像须 **512 字节对齐**（零填充补齐）；进 MASKROM = 救砖 USB-A 口 + A对A 线接 PC 上电，或 MASKROM 键。镜像结构：RKFW 容器 + 内嵌 RKAF 包（`0x77226`，文件表每项 0x70B）+ **整个文件即 ext4**（起点 `0x43B4226`=70992422，非 512 对齐）；离线改文件用 `debugfs`（loop 挂载 /mnt/d 会被 9P 强制只读）。
- **外设体检（全正常，43°C）**：GPU Mali 1GHz / **NPU RKNPU v0.9.8 1GHz（设备节点 = `/dev/dri/card1`+`renderD129`，新版驱动走 DRM，无 `/dev/rknpu` 属正常）** / WiFi 博通 bcmdhd / 蓝牙 hci0 / USB / HDMI×2 / 音频×4（HDMI×2+HDMI-in+ES8388）/ RTC / CAN / PWM。缺 **librknnrt**（跑 NPU 推理前需装 RKNN Runtime）。
- **踩坑速记**：`chpasswd` 设 <8 位密码被 pwquality 拒 → 用 `usermod -p '<sha512哈希>' root`；最小改动版固件**未**含实验阶段注入的密码/密钥（原厂 root 密码未知，烧后必改）；A对A 线掉 maskrom 是救砖口设计行为不是故障。

---

## AGENTS-2026-09-26b（AGENTS.md 第二轮精简索引；原文不重复搬运）

根目录 AGENTS.md 当日从 ~130 行（近注入上限）瘦身约 2/3。以下搬出块**原样保留在本文件 774 行起 `AGENTS-2026-09-26 全量快照`的对应小节内**（快照 §一~§八 与瘦身前逐字节一致），故此处只列索引：

- **§二 旧 K210 识别口径**（"识别=K210 本地 KPU 主责 09-10 定 / 09-11 DNK210 双模型接入 / 帧协议 09-07 定版细节"两条）→ 已被 **2026-09-21 改向：K210 纯发图、识别主责在上位机**（commit `81f1c4a`，旧固件入 `k210_fw/tmp_kpu_old/`）取代；现行结论见根目录 AGENTS.md §二。
- **§四 搬出**：M4「OLED 摘除（09-07）」「CAN 主端落地（09-07 待烧录版，已被 G3 验收条取代）」「底板 KEY 方案」；core0「代码已落地 P3-05~P3-11 文件清单」「业务守护 09-11 长块（交叉编译三坑/队列唤醒/VMIN=0 两连爆/142 项 selftest）」；C8T6「BH1750+OLED 业务完成」「设计功能升级（09-09）」「CAN 2.0 从节点落地（09-06）」「接口 v2 定版过程条（09-11/09-12）」「审计附录 A（09-06）」。现行结论（500k 位时序/sw I2C/SG90/接口 v2 帧格式/未验 3 项/regen 注意）已压缩保留在根目录 §四。
- **§五 搬出**：「git 现状（2026-09-11 收工）」提交清单长块。纪律条款（不自动提交/一功能一提交/sample_ 约定/ASCII 规）全部仍在根目录 §五。
- **§六 搬出**：09-16 版下一步（K210 识别剩余三件等）——已被 09-21 改向 + RK3588 线取代；现行下一步见根目录 §六（2026-09-26 更新）。
- **§七 搬出（最大头）**：K210 识别时代全记录——「📦 K210 细节已归档」「⭐ 固定 ROI 直识别（09-15）」「🧪 两次真机跑」「🔬 早死定位装置（09-15/16）」「✅✅ SD 卡读写」「📊 GC 堆/系统堆真相（09-13）」「🚑 压小 GC 堆」「🔌 SD 提速全过程」「🖥️ 相机/LCD 账」「🔁 重试封顶」「💾 图片存储审计」「✅🎯 09-16 进度」「🖼️ 预览硬规矩」（结论=jpeg_from 不吞错/console_send_jpeg try+限频，已压缩保留根目录 §八末条）「📦 det=0/相机方向」「🚀 两段式开机（09-16，现已被 09-21 单 main.py 部署取代）」；Qt 旧叙事「🐞 远程关闸/闸位闪变」（机制值已压缩保留根目录 §七末条）「🐞 布局修复第二轮」「🟢 09-11 收工」。仍在用的硬事实（book qmake 配方/部署路径/板端 Qt 事实/1-4 屏结论/启动链/board-power/P6-04/K210 日志上屏）全部保留在根目录 §七。
- **§八 搬出**：「⛔ K210 板端操作纪律」长块（结论=保存到设备+冷启动、勿用 IDE「运行」，已压缩保留根目录 §五末条）；「🐞 布局修复第二轮」（同 §七 条）；「✅✅✅ 第7步端到端成功」长块（结论保留根目录 §八）；「📦 09-11 晚三项已归档」指针。

**⚠️ 快照缺口提醒**：774 行快照的 **§九 是旧版**（调试通道只有 adb/ssh 两条），**缺** 当日后补的四块——③ Tailscale 主远程通道、网络拓扑定案（笔记本常驻 ICS）、NPU 全栈已通（librknnrt 坑 + LPRNet 2.43ms/帧）、启动介质定案（eMMC）。这四块**目前以根目录 AGENTS.md §九 为唯一存档**；下次精简若搬动 §九，必须先补快照。


## 代码审计 2026-09-27（core0_service 业务逻辑 + C8T6 下位机，只读审查双报告）

> 审查方式：双 explore 代理全量精读。**以后再审这两块：先读本节结论 → 只核对"代码是否已改/新增路径"**，勿再全量翻代码。C8T6 部分与 `c8t6/can.md` §10 互补（§10 是 09-06 历史审计，本节是增量）。

### A. core0_service 业务逻辑（agent-0）

**高严重度**
1. **白名单热加载重复追加**（`business/app_config.c:100,151-174`，`WL_MAX_ENTRIES 64`）：热加载以当前内存为 base 再追加 → 每次热加载白名单翻倍，21 次后表满、新条目永久被拒、删除不生效。修法：解析 `[whitelist]` 前 `wl_init` 归零（一行）；selftest 补"两次 load wl_count 不变"。改动小。
2. **COOL_DOWN 期间新车到位永久漏检**（`business.c:250-263,580-581`）：A 车冷却中 B 车到达被丢弃，冷却回 IDLE 后无补触发 → B 永远不被识别（复现：被拒车倒走、下辆 4s 内进场）。修法：冷却到期/presence=1 时补走识别分支。改动小。

**中严重度**
3. **shm plate/confidence/result_source 双向复用竞态**（`core0_main.c:155-173` vs `ipc_writer.cpp:227-263`）：core0 任何 publish 会覆盖待消费的 core1 结果（≤200ms 窗口）→ 开错闸方向。最小修法：`plat_publish` 在 `result_valid` 时保留三字段；彻底修法 shm v4 分字段。改动小/中。
4. **自动开闸忽略 `gate_cmd` 返回值**（`business.c:341-344`）：rc=-1 时闸没发出去却进 COOL_DOWN，车被吞。改动小。
5. **识别窗口不随车离开取消**（`business.c:318-351`）：车走了迟到结果照开闸（给空气开 4s）。待确认 spec。改动小。
6. **presence 无自愈**（`business.c:432-436` + `core0_main.c:236`）：C8T6 重启后 presence 卡 1 → 识别死锁到 core0 重启；0x22 解码失败还映射成 online=1。修法：0x22 离线清 presence/pass_pending，解码失败按离线。改动小。
7. **result_valid 清除与新写入并发**（`ipc_shm.c:79-99`）：先拷后清会抹掉刚到的新结果。修法：用 evt_seq_c1 做 handshake/CAS。改动中。
8. **置信门槛归属与文档口径矛盾（待拍板）**：实现在 core1（ipc_writer 读 shm conf_threshold），AGENTS 说"判定在 core0"；云回写又走 core1 accept_conf=0.50 不过 core0 阈值。改动小（文档或 core0 补判）。

**低严重度**：fault bit3 用累计计数清零语义矛盾（待确认 M4）；HB_CHECK==HB_LOST==3s → core1 失联最坏 ~6s；双重 0x13；`write_all_locked` 持锁 200ms 拖闸令 KPI；事件队列满丢 0x01；热加载 mtime 秒级 + log_file 不重建；seq 缺口统计回绕失真；`enter_deny` 记上一辆车的牌；**无自动关闸**（开闸后闸保持开，过车不自动关——待确认需求，若要在 on_car_leave 补 gate_cmd(0)）。

**已确认自洽**：超时收敛/cloud 只延长一次/绝对上限；关闸乐观态+SETTLE 1600+GUARD 1500 三参数自洽；计数方向/限幅/防重正确；配置双缓冲原子切换；RPMSG 拆帧/重连节流无风暴。

### B. C8T6 下位机（agent-1）

**中严重度**
- **P1 闸位"到位"判定不对称**（`gate.c:92-95` + `can_node.c:176-188,60`）：`Gate_IsOpen()=cur>=OPEN_CCR`，开方向到位才置 1 ✓，**关方向第一步即置 0** → 0x04(arg=0) 提前 ~190ms 撒谎（A7 有 SETTLE 兜底不炸，但协议语义不符 can.md §4.5）。修法：边沿+bit0 统一 `IsOpen && !IsMoving`（或加 `Gate_IsSettled()`）。改动小。
- **P2 事件发送失败丢边沿且无重试挂点**（`can_node.c:181-188`、`freertos.c:280-287`）：latch 先更新致重试不可能。修法：1 槽重发缓存 `s_evt_retry{valid,ev,arg}`，Poll 每拍补发直到成功（上限防风暴；0x01/0x02 可由 0x210 bit1 兜底、0x04 由 bit0 兜底，1 槽够）。改动中。
- **P3 shade 基线对灯光突变无防护**（`shade.c:76-95` + `config.h:30-32`）：变暗 EMA/16 慢衰减 → 灯光阶跃降 80% 时误发 0x01（~3s）+ ~8.5s 后误发 0x02 ≈ 10s 假 PRESENCE。修法组合：变暗方向前 K 拍快 EMA(/4) / 越阈时强制 5 拍基线学习宽限 / CONFIRM_N 提 5。改动中。
- **P4 基线"只上不下"ratchet**（`shade.c:79-82`）：上跳即时赋值 → 车灯/亮斑 1 拍抬基线 → 灯后误报"车到位"，~8.5s 才回落。修法：上行也限幅（每拍 +max_step=基线 5~10%）或快 EMA(/4)。改动小。
- **P5 低照度 drop% 信噪比崩溃（待现场数据）**：夜间 lux<10 时 ±1lx 噪声=10~100% drop；修法：基线 <SHADE_LUX_MIN 时冻结状态机 + 状态位约定，或百分比+绝对差双条件。改动小-中。
- **P6 HAL_CAN_Start 失败永久失联**（`can_node.c:135-163`）：can.md #14 重申；修法：Poll 里 10s 周期性 Stop+重配+Start 重试，成功补发 0x05。改动小-中。

**小/微小**：P7 `GATE_SELFTEST=1` 常驻（交付版置 0）；P8 软件 I2C 实测 ~70kHz（注释假设 300kHz），OLED 整页关中断 ~13ms → 采样周期漂移/关中断窗口偏大（待示波器实测，调 Delay 或分块关中断）；P9 任务栈高水位未标定（联调用 uxTaskGetStackHighWaterMark）；P10 bit2=1 时 bit0/bit1 无效的跨节点约定要钉进 protocols §4.5（待确认 M4/core0 是否已按此）；P11 OLED 无 PRESENCE 显式字样；P12 0x05 失败不重发 + 越界档位无计数（随 P2 一并）；P13 无 IWDG/远程复位（demo 可接受，记录为限制）。

**已确认无需改**：0x100 解析健壮、TX 互斥无优先级反转、NART/ABOM/零中断轮询、故障哨兵隔离、drop-oldest 队列、tick 减法回绕安全、行4 优先级、灵敏度档位 0x10 全链路落地。

**双端共同待办（按本次审查）**：core0 白名单归零重载 + 冷却补检 + publish 保留 result 字段；C8T6 P1 settled 语义 + P2 重发槽 + P3/P4 基线防护。均"小-中"改动量，可各成独立提交。

## AGENTS-2026-09-28（AGENTS.md 全量快照归并第 2 轮；根目录 AGENTS.md 仍保留并继续维护）

> 归并方式：`cat AGENTS.md >> docs/AGENTS_history.md` 字节级原样并入，未改动一字节（本节头部除外）。
> 本节标题以下到文件末尾 = 2026-09-28 归档当天根目录 AGENTS.md 的完整内容（即 2026-09-27 13:22 第二轮精简后的现行版，~29KB / 91 行，涵盖 §一~§九）。
> 与上一轮的关系：上文 `AGENTS-2026-09-26` 快照（本文件 774 行起）= 09-26b 精简**前**的全量版；本节 = 精简**后**的现行版。09-26b 索引警告的"快照 §九 缺四块（Tailscale / 网络拓扑定案 / NPU 全栈 / 启动介质）"缺口**已由本节补齐**（现行 §九 四块齐全，且含 09-26 装机部署三坑、双端逻辑审计存档指针、笔记本链路根因修复等后补内容）。

---

# 项目记忆（自动注入，勿删）—— 端侧AI · 边云协同停车场

> 本文件是跨会话的工作记忆，记录决定、板级事实、当前状态与下一步；不要与 README/Task 的设计正文重复，交叉处只引用。
> **📦 历史归档 `docs/AGENTS_history.md`**：2026-09-11 起多轮搬移（K210 识别时代全记录 / C8T6 / 第7步细节 / **2026-09-26 全量快照** + 第二轮精简索引 §AGENTS-2026-09-26b）。**本文件只留结论与当前状态**；要完整推理链去读存档。被取代的旧口径以存档为准，勿凭记忆复述。

## 一、项目一句话 & 文档分工
- 100ASK-MP157（STM32MP157DACx，双 A7 SMP + M4 FreeRTOS/OpenAMP）+ 外接 K210 + **RK3588（定昌 DC-A588，2026-09-25 入场）**的停车场**端侧AI·边云协同** Demo。
- `README.md`=定位/分工/硬件/目录/构建；`Task.md`=架构/拆解/数据交互/约束/KPI（内容隔离，勿重复）。**两处都有 09-21 后未纠偏的过时描述（见 §六-5）**。
- 字段级协议统一归 `docs/protocols.md`（改协议先改母本 `PhaseMd/10_协议规格总表` → 同步 protocols → 改代码 → 两处变更记录）。
- `MD文档/rk3588/调试记录.md` = RK3588 调通详录；`PhaseMd/` = 15 份执行层任务分解（P<步>-<序号>，验收门 G0~G8）。
- **📄 新增 .md 归口 `docs/`（2026-09-27 定）**：新写文档（设计/记录/调试/规范）一律放 `docs/`；`MD文档/`、`PhaseMd/` 属存量布局不强制搬迁，但新增不再进这两处；根入口文件（README.md / Task.md / AGENTS.md）除外。

## 二、当前确定架构
- **⭐ 车牌识别（2026-09-21 改向，取代"K210 本地 KPU 主责"旧口径）**：**K210 = 纯图像采集上行**——`other/k210_fw/main.py`（BUILD=2026-09-21-imgonly）只干 相机+JPEG+上行，不加载 SD/KPU/识别；旧识别固件归档在 `other/k210_fw/tmp_kpu_old/`。**识别主责 = RK3588 NPU**（LPRNet，§九）。**⭐ 识别引擎已切 HyperLPR3（2026-09-26 晚，9/9 板端回归全对）**：`edge_hub --engine hyperlpr3`（systemd 默认；CPU onnxruntime 68.8ms/帧）——RPNet v3 识别宽 160，治好了 LPRNet 的省份字混淆（京→津）和 <200px 掉尾字，还输出绿牌类型 ptype。LPRNet 线保留为 `--engine lprnet` 回退（+`--det-model yolov8s.rknn` = 两段式）。`--votes 2` 连续帧投票（`plate_vote.py`）两个引擎共用。检测器 A/B 定论：`Stara-AI/CarPlateDetection` yolov8n 宣传 mAP 0.994 但对大目标车牌零检出（训练分布窄），**弃用**；yolov8s rknn 留作回退。HyperLPR3 离线装法（板子无外网时）：台式机 pip download + 模型 zip scp 上板。**⭐ 回传通道已拍板（2026-09-26，方案 2）**：K210 物理上插 **RK3588**，`rk3588_service/edge_hub.py` 把 K210 行流 + 注入的 `K2:OK/NG` 识别结果经 **以太网 TCP :8089** 中继给 park_ui（`k210_link` mode=tcp，协议 `docs/protocols.md` §6）；备选方案 1（K210 留 MP157、图像转发 RK3588）记录在 §6.5 未实现。代码+宿主单测 32 项+板上识别实测全绿（川A88888 conf 0.986 / NPU 2.5ms）。**📦 2026-09-27 迁移定案**：`k210_fw/` → `other/k210_fw/`（K210 下线归档，commit `1ac911c`，19 文件 rename 历史保留；`main.py` 的 CAM_CROP 改进留在工作区归 RK3588 线提交；`tools/readme.txt` 按 *.txt 规则退库）。
- **车辆到位/道闸**：C8T6（BH1750 遮光 + SG90 + OLED）⇄ **CAN 500k** ⇄ M4(FDCAN2) ⇄ **RPMSG** ⇄ A7-Core0 业务判定。接口 v2 语义事件：CAN 帧表 = `docs/protocols.md` §1，RPMSG **0x21 = 9B**（`code|arg u16 LE|status|node_id|tick u32 LE`）。
- **⭐ 相机源（2026-09-27 改，V4L2 取代 K210 主线）**：杂牌 UVC 摄像头（icSpring 32e6:9221）直插 RK3588，走 `edge_hub --source /dev/video4`（`/dev/ttyACM0` = K210 备选源，一键切换）；相机 640x480@30 MJPG，中继 `--relay-fps 15` → LCD 实测 14.8fps（park_ui 60ms 拉帧 + 状态栏 **FPS 芯片**）；识别仍 3s 周期。K210 已归档 `other/k210_fw/`（含 `CAM_CROP` 固件级取景框裁剪——小图编码快+车牌占满，备选路线）。**认节点用 `v4l2-ctl --list-devices`**：RK 上几十个 video 节点多为 rkisp/hdmi 内建设备，`video52` 是 HDMI-IN 骗点；opencv 在错节点 open 成功但 read 全 0，别只 `ls | tail`。
- **⭐ 云端 enabled 总开关（2026-09-27）**：`/etc/park/cloud.conf` `enabled=`（0=全禁）+ LCD 设置页"云端使能"勾选（即改即生效、持久），**低置信与识别失败两条路径都门控**——失败路径无阈值可挡，想零 cloud 请求只能靠它或"断网演练"（后者重启复位）。**真正的采纳/兜底门槛 = Core0 `conf_threshold`**（shm，core0.conf 热加载）；`trigger_conf`/`accept_conf` **未接线**（只解析+显示，别再调）。WiFi 掉线与云代码无因果：enabled=0 零流量后仍掉 → 路由器/云端侧（MP157 调试入口 = COM4 串口，WiFi 不稳不靠它）。
- **网络决策**：MP157 云端链路单用板载 WiFi；**MP157↔RK3588 = 板载网口直连：MP157 eth0 ↔ RK3588 eth0**（192.168.10.1↔10.2，主机名 `rk3588`，`rk3588-eth.sh` 幂等配置；唯一网线从笔记本让位过来）。当日踩坑：RTL8152 USB 网卡载波抖动退役；**RK 的 NetworkManager 抢 eth0 致静态 IP 时有时无** → 删 NM 配置 + `conf.d/99-eth0-unmanaged.conf` 钉 unmanaged（详见 `MD文档/rk3588/调试记录.md` §九）。**RK 调试 = MP157 跳板** `ssh -J root@<MP157> root@rk3588`，已免密（MP157 root 密钥注入 RK）；Tailscale 因 RK 断外网休眠，接回笔记本线即复活。断网本地闭环照常。

## 三、板级事实（MP157，从原理图 .DSN 挖出）
- I2C：I2C1=AP3216C+ICM-20608；触摸 I2C 在 LCD 排线口；**I2C4/I2C6 只能 A7**；I2C2(PH4/PH5) 未引出排针。
- **FDCAN = FDCAN2（PB5=RX/PB6=TX, AF9）**，Linux 节点 can0；**内核时钟实测 62.5MHz**（M4 位时序/文档均按此；早期"FDCAN1/PD0-PD1"误读作废）。
- 能插杜邦线的物理口仅 JTAG/SWD/Camera&Extend；板上另有 WiFi/SIM/WM8960/HDMI/以太网/USB/CAN(TJA1042)。
- **MP157 USB Host 供电 = GPIO 82(PF2)/139(PI11)**：禁 myir.service 后无人拉高 ⇒ `board-power.service` 负责（§七）；K210（CH9102 `1a86:55d4`）走 `cdc_acm` → `/dev/ttyACM0`。

## 四、M4 / C8T6 / core0 现状（结论版）
### M4（m4_fw/）
- 工程：.ioc 只配 FDCAN2 + FreeRTOS(CMSIS_V2, 堆 32768) + OPENAMP(rpmsg_bridge)；CAN_Rx_Task 10ms 轮询 + Rpmsg_Task；**I2C1/OLED 已从工程摘除**（过程 → history）。
- **✅ G3 全链路验收通过（2026-09-10）**：A7 0x11/0x12 → M4 → CAN 0x100 → C8T6 闸门真机闭环。**现行 can0 recipe（旧"down+unbind"作废）**：m_can 保持绑定 + `ip link set can0 type can bitrate 500000` + `can0 up` 点亮 fdcan_k 时钟，A7 静听不发；`load_m4.sh ensure_can_clock()` 已固化。**遗留**：C8T6 手遮→0x21、断电→0x22 未补测；**正式版 M4 未重烧**（后门宏源码已置 0，板端仍是验证版）。
- 按键：M4 不读底板 KEY（避免与 Linux input 争用），指令一律 RPMSG→M4→CAN。
- 写码依据速查（细节 → history 快照）：HAL 用 `HAL_FDCAN_AddMessageToTxFifoQ`（MP1 无 AddTxMessage）；CubeMX OPENAMP 勾法与 100ASK 例程在 `E:\download\100ASK-MP157\...22_A7_M4_UserModeComm\rpmsg_user\`；vring 由 Linux 分配；内核 5.4.31 双怪癖（NS 通道须先 `driver_override` 再 bind）；**HAL 头 ISO-8859 编码，grep 必须加 `-a`**。
### C8T6（c8t6/）
- CAN1 500k = APB1 36MHz / Prescaler 9 / Seg1 5 / Seg2 2；PA11/PA12 → TJA1050；**NART + ABOM 开**；FreeRTOS 堆 9600（4 任务+队列够）。
- **I2C 全软件模拟**（硬件 I2C1 不可靠已实锤）：`sw_i2c` PB8=SCL/PB9=SDA；SSD1306(0x3C)+BH1750(0x23) 共总线，`OledMutex` = 总线互斥；OLED_Task 栈 384。
- SG90 = TIM2_CH1(PA0) 50Hz PWM，`Gate_Poll` 10ms 缓动到位才报闸位；接口 v2 事件/心跳/查询全落地，**09-12 板验通过**；**未验 3 项** = 抓帧对字段、坏 CRC/10min soak、断电 0x22（不阻塞）。
- .ioc regen 必核对：TIM2 PWM、NART、堆 9600、OLED 栈 384（都会被回退）。
### core0_service（A7-0）
- **✅ 业务守护 P4-01~P4-08 全落地**：RPMSG 链路层（ttyRPMSG0 raw + 心跳 + 断链重开自动发 0x13 重同步）+ 业务（白名单/进出计数/开关闸判定）+ park_shm v3 双向 + 轻量文件记录（默认关）。selftest 142 项（两版）全过；板端验收用 `G4_ACCEPTANCE.md` + `core1_stub` 打桩；配置模板 `sample_core0.conf`。
- 消费方式：只用 `on_frame`/`on_link` 回调 + 0x23 快照（不碰 fd）；**v1 的 17B CAN 透传已废**（收到 len=17 会报 "v1 CAN passthrough? reflash M4"）。
- 工具：`load_m4.sh`（remoteproc + can0 时钟）、`rpmsg_cli`（tx/raw/bad/stats）；**改动后须 book 交叉编译** `make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-` → `install_all.sh`。

## 五、git / 环境 / 工具备忘（纪律仍然有效）
- git 在 `C:\Program Files\Git\cmd\git.exe`（不在 PATH）；远程 SSH `git@github.com:randomlyiii/Project_EdgeParking-CloudSync.git` master；push 可用（失败加 `GIT_SSH_COMMAND="ssh -o BatchMode=yes"`）。**AGENTS.md 已被 .gitignore（纯本地）**。
- **⛔ 不要自动 git 提交**：改完只留工作区，等用户明确说"提交/commit"；提交信息临时文件写仓库外（`$env:TEMP\parkmsgN.txt` + `git commit -F`，中文不进命令行）。
- **🧩 一功能 = 一提交**：先按功能分组自查 `git diff HEAD --stat`；同文件多功能用 `git add -p` 拆 hunk；**docs 改动独立成 `docs(...)` 提交**。细则全文 = `MD文档/Git_Update_standard.md`（2026-09-27 建）。
- **🔐 sample_ 约定**：真文件（`/etc/park/cloud.conf`、`wpa_supplicant.conf`、`park-ui.env`、`core0.conf`、`key.txt`）一律 gitignore，仓库只放纯 ASCII `sample_*` 模板；`check_static.py` 第 8 节门禁（含负向测试）。
- 环境：本机无 arm-none-eabi ⇒ STM32 工程在 CubeIDE 编；`cubekill.bat` 清构建产物；参考资料 `E:\download\100ASK-MP157\100ask-mp157原理图\01_Base_board(底板)\`（.DSN 可 grep 明文，PDF 无法提取）；**⛔ 板端粘贴代码纯 ASCII、禁中文**（SSH 粘贴中文必坏）；板端 root 无 sudo、无 pgrep/timeout，`/tmp` 重启即清。
- **K210 部署纪律**：CanMV IDE「保存到设备」才驻板；验证一律存 .py + 冷启动；**勿用 IDE「运行」**（软重启不还内存）。

## 六、下一步建议（2026-09-26 更新）
0. **今天收尾（2026-09-26 装机日）**：全链路已通——RK eth0 直连 MP157（网口插 eth0！eth1 是笔记本 ICS 口）、edge-hub 相机源 /dev/video4 + hyperlpr3（unit 加 `Environment=HOME=/root` 修复识别静默失效，仓库模板同步提交 `265bd81`）、LCD 有图、no_plate 3s 轮询正常。**遗留动作 = 举牌实测弹卡**（打印川A88888 入镜 → 预期 hold→OK 两周期 ~6s）。排查通道：台式机 COM4（MobaXterm 需让口）→ MP157 → `ssh root@rk3588` 免密跳板。
1. **✅ HyperLPR3 → RKNN 已转换+对拍（2026-09-27，细节 = `参考资料/rk3588模型/hyperlpr3/RESULTS.md`）**：y5fu_320x/640x + rpv3 三个 FP16 rknn 已生成并持久化（板上 `/root/models/`，PC 同版备份），**NPU vs CPU 引擎 3/3 全对**（含 14% 缩小/8° 旋转），320 档 det+rec 合计 **~18ms**（CPU 69-131ms）、640 档 ~46ms。**纠正两个预估**：①检测**不是 yolov5 anchor**——`y5fu_*_sim` 是单融合头 (1,N,15) **anchor-free**（"sim"=sigmoid 已焊死，**解码全用 raw logits**：obj>0.25 → 角点仿射校正 → NMS），生产参考 `hyperlpr3.inference.multitask_detect.post_precessing`；②字符表 **78 token 非 65**（rpv3 输出 (1,T,78)，blank=0）。rknn 输入 NHWC uint8（det mean0/std255、rec 127.5，BGR→RGB 自做，rec 右补 127）。⚠️ `hyperlpr3.inference.*` 的 `@cost` 装饰器板上 import 即炸，只能 import `common.*`+`multitask_detect` 函数。conf 口径待对齐（rknn softmax 均值 0.03 vs CPU 0.999，字串全对）。**✅ 验收已过（2026-09-27，COM4 通道实操）**：8 图集（原图/14%/50%/25%/模糊/变暗/±旋转）320 档 **8/8 与 CPU 全对**（det 11-17ms+rec 6-12ms）；640 档 7/8（高斯模糊图少认一个 8，320 反而对）——**集成取 320 档**。**剩**：`--engine hyperlpr3rknn` 接入 edge_hub（双引擎灰度，9 回归+现场实测过才切默认；集成点：角点校正/双层牌 40% 切开/cls 颜色可选/conf 对齐）——动 `edge_hub.py` 前先协调另一在途会话（其 MotionWatch/engine 改动未提交）。
2. **✅ 识别帧差门控已落地（2026-09-27，方案 B：相机连续帧运动检测）**：`motion_watch.py` MotionWatch——CameraThread 用现成 BGR 帧降采样 64x48 灰度 MAD（threshold 2.0）喂 watch，RecogThread 查询 `motion_recent()`（hold 9s 无运动才睡）；**hold 窗口一个参数替代状态机**（=滞回+即时唤醒+冷启动 9s 宽限，a 方案的 arm/probe/首帧特判全删）；**投票 pending 非空时禁止休眠**（`PlateVoter.has_pending()`，防车停稳后投票卡 1/N 弹卡永不触发）；睡着每 30s 打 `recog: idle`，唤醒打 `recog: wake (motion)`；CLI `--motion-threshold 2.0 / --motion-hold 9`（0=关），unit 未改（默认开）。不动相机帧率（LCD 预览常显；发热主体是相机链路，此优化价值=省识别空转）。宿主单测 68 项全绿（含"静态冻结/运动唤醒/投票钉住"三个集成用例）。
1. **RK3588 侧（代码+部署已落地，剩物理连线）**：`rk3588_service/` 全套——edge_hub（K210 reader + LPRNet 周期识别 + TCP :8089 行中继）+ lpr_server（HTTP 测试台 :8088）+ lpr_decode（字符表 2026-09-26 实车标定）+ 宿主单测 32 项 + `tests/itest_edge_hub.py` 板上端到端（pty 假 K210）PASS + systemd `edge-hub`/`lpr-server` 已 enable；模型 `/root/models/lprnet.rknn`，板上实测 川A88888 conf 0.9864 / NPU 2.5ms / HTTP 8.5ms，Tailscale 外部链路 curl 亦通。**剩**：K210 插到 RK3588 后看 `journalctl -u edge-hub -f` 出 `recog: OK`；注意 **brltty 抢 ttyACM0**（GNOME 桌面常见，`systemctl mask brltty`）。
2. ~~拍板：RK3588 识别结果怎么回 MP157~~ **已拍板（2026-09-26）= 方案 2 以太网 TCP 行中继**（edge_hub 复用 K2: 行协议注入 K2:OK/NG，park_ui `k210_link` 加 mode=tcp 已落地）；云兜底归属不变（Core1 收 `recogFailed` 后走既有 DeepSeek 链）。
3. **MP157 收尾（小活）**：M4 正式版重编重烧；park_ui 若按拍板改 K210 消费逻辑，book `build_arm.sh` 重编 + scp。
4. **验收欠账**：现场验收剩：C8T6 审计四修（P1 settled 语义/P2 重发槽/P3+P4 基线防护，**代码已提交 `bc8f9a5`/`064782e`/`1cbb228` 但固件未烧**——CubeIDE 重编烧录后补实测）+ C8T6 三项老补测（抓帧对字段/坏CRC soak/断电 0x22）；P6-05/06/07 埋点随验收补；**other/k210_fw/tools 回归套件已坏（09-22 单文件重写后未同步）**：`build_main.py --check` 找已归档的 `park_app.py`、多个 test 引用 `sd_probe2.py` → discover 8 errors；**改 other/k210_fw 前先把测试套件按 imgonly 单文件现状重写**。**⭐ core0 审计三修已提交并上板（2026-09-27，`7f203f9`/`c7d129f`/`19e2479`，宿主 selftest 164 项全绿）**：白名单热加载文件权威化、冷却期到达补识别（arrive_deferred）、publish 保待消费结果三字段。板上 `core0.conf` 现值：recog_timeout=8000ms / cloud_timeout=12000ms / 白名单含京AD06088（排障期调宽，功能正常保留）。**排障陷阱记**：遮光/闸令全走 CAN——**CAN 断连的现象 = "识别成功（LCD 弹卡正常）但不开闸"**，先查 can0/M4 再怀疑业务。
5. **文档纠偏（2026-09-26 大头已做）**：README 已重写（RK3588/C8T6 入列、K210 纯发图、core1_ui=qmake、开机链补 board-power 与 rk3588-eth）；Task.md 识别相关段落已同步；protocols.md §6 + 母本 §7 已建。**剩**：`MD文档/rk3588/调试记录.md` 补 librknnrt 版本错配与 `init_runtime()` 无 target 两条（见 §九更正）。

## 七、Qt GUI / 部署（当前有效事实）
- **编译**：只能 book SDK 的 qmake（Qt 5.12.8，`/home/book/100ask_stm32mp157_pro-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin/qmake` + 同目录 g++）；OpenSTLinux Qt 5.14.1 编出的板端跑不了（`version Qt_5` 报错）。配方固化在 `core1_ui/qt_gui/tools/build_arm.sh`，坑见 `qt_gui/README.md` §3。
- **部署**：`scp bin/park_ui root@192.168.189.65:` → 板端 `/opt/park_ui/park_ui`；前台跑要 `< /dev/null`（否则 SIGTTIN 停住）。
- **板端 Qt**：5.12.8；插件目录 `/usr/lib/qt/plugins/platforms`（有 libqlinuxfb.so）；`QT_QPA_PLATFORM='linuxfb:fb=/dev/fb0'`；`/etc/profile` 的 QT_* 对 systemd 无效。
- **LCD**：park_ui linuxfb 独占常显（park-ui.service）；**1/4 屏根因** = linuxfb 无 WM 时 showFullScreen 不改几何 ⇒ `mainwindow.cpp` linuxfb 分支显式 `setGeometry(screen()->geometry())`（勿回退）。
- **启动链**：`board-power → m4-load → core0-bus → rk3588-eth → park-ui`（rk3588-eth = 静态 192.168.10.1/24 + hosts `rk3588`→.2，幂等脚本，IFACE=eth0 板载口）；WiFi/时钟不在关键链。park_ui 的 K210 上行默认 **tcp 模式**（`PARK_UI_K210_TCP`，默认 `rk3588:8089`；显式置空回退本地串口）。安装器 `deploy/systemd/install_all.sh`（**10 步**；装 /opt/core0 + /opt/park_ui + /lib/firmware/m4_fw.elf + rk3588-eth；**不覆盖已有 core0.conf**）。**park_ui 新二进制已部署（2026-09-27）**：状态栏 FPS 芯片 + 60ms 拉帧（15fps）+ 云端 enabled 门控。
- **⚡ board-power.service**：开机拉高 GPIO 82/139 给 USB HUB 供电（禁 myir 的代价），已跨重启验证；没它 K210/U 盘全消失。
- **✅ P6-04 已板验**：`ipc_writer` O_RDWR 挂 /park_shm、1s 心跳、结果回写、低置信→cloud_pending、req_gate_* 脉冲、底栏触摸开/关闸 + O/C 热键。
- **📺 K210 日志上 LCD（09-16）**：k210_link `feedText()` 全量转发 → journald + 事件条筛选（[BOOT]/[MEM]/[SD]/SDX:/fail/error 等，高频限流 5s）。K210 不再发结果行后，此通道只看诊断。
- **弹卡/结果链路现状**：原 `K2:OK/NG:` 结果行已随 09-21 固件移除 ⇒ **Qt 弹卡链路待 §六-2 拍板后重接**。
- 已修机制在代码里（**勿回退**）：关闸乐观态 + `GATE_SETTLE_MS=1600` 延迟重同步 + 周期 0x13、`GATE_REPORT_GUARD_MS=1500` 防闪变；云失败分类 no network/certificate；wifi_up.sh 关联门 + 15s DHCP 截止；布局用 `Preferred + setMinimumWidth(0)`（**勿用 QSizePolicy::Ignored**，芯片会消失）。

## 八、第7步 云端（操作要点仍然有效）
- **云代码唯一在 `core1_ui/qt_gui/src/`**（cloud_client/cloud_settings，装配 main.cpp；**Core0 禁云**；`core0_service/cloud`、`core1_ui/cloud_api` 占位目录已删，`check_static.py` 第 9c 节负向断言）。
- `/etc/park/cloud.conf`（600，模板 `deploy/sample_cloud.conf`，**每个 provider 一把 key**）；CA 自备 `/etc/park/ca.pem`；**无 RTC ⇒ 先校时否则证书"尚未生效"**；`transport=auto|qt|python`（Qt+OpenSSL1.1.1 在 TLS1.3 卡死，python 通道 ~1s）；`timeout_ms` 上限 ~6000。
- **✅ 端到端已真机验证（09-11）**：transport=python + DeepSeek 实测识别出车牌。实现细节 → history §第7步-2026-09-11。
- **🔒 本机 IP/网关不上屏不落盘**（门禁：wifi_manager 无 QString ip/gw、不解析 /proc/net/route、全 GUI 源码无 `192.168.` 字面量）；WiFi 芯片只显示 SSID。
- **WiFi 运维**：wifi-up.service 开机自连（厂商三步固化在 `deploy/systemd/wifi_up.sh`，无 AP 时关联门跳过 DHCP）；设置页「网络」三动作：联网=只动链路、保存=只写 wpa_supplicant.conf、连接=写+应用+20s 无租约回滚；无租约时事件行提示 `systemctl restart wifi-up`。
- **K210 端两条硬规矩（纯发图固件仍在用）**：`jpeg_from()` 失败必打原因、**不许**退回裸 RGB565；`console_send_jpeg()` 整段 try/except + 限频 `_preview_drop`（预览失败绝不许掀掉主循环）。

## 九、RK3588（定昌 DC-A588）板级事实（2026-09-25 调通；详录 `MD文档/rk3588/调试记录.md`）
- **板子**：定昌 **DC-A588**（=飞牛 NAS 同款底板），RK3588 八核 / 8G RAM / **64G eMMC** / 双千兆网口（eth0/eth1）。原厂固件 = GB-RK3588 Ubuntu 24.04 GNOME（5.10.198 vendor 内核）。USB 口分工：**仅 Type-C 口**（otg 控制器 dr_mode=otg）支持设备模式跑 adb；**USB-A 口全部 host-only**（dtb 焊死），且其中一口兼 **MASKROM 救砖口**——A对A 线接上电即进 maskrom，**勿当调试口**。
- **在板固件** = 原厂 + 仅两处修复：① `/etc/init.d/.usb_config` 带前导空格致 `case` 永不命中 → 改精确 `usb_adb_en`（上电自启 adbd）② `/etc/rc.local` `echo host`→`peripheral`（锁死 OTG 设备模式，防开机末段顶掉 gadget）。烧后已配：root 密码 **rk3588**、netplan `192.168.10.2/24` 静态 + `dhcp4` 双配置（持久化）。镜像文件 `D:\BaiduNetdiskDownload\GB-RK3588-gnome-minimal-adb-on.img`。
- **调试通道**（均真机验证）：① `adb shell` = A对C 线插板子 Type-C 口，**直接 root**；② `ssh root@192.168.10.2` = 网线直连 PC/笔记本（PC 侧设 `192.168.10.1/24`，**网关/DNS 留空**），插路由器则自动 DHCP 拿第二地址；③ **⭐ Tailscale（主远程通道）** = `ssh -i ~/.ssh/id_ed25519 root@100.72.143.125` 免密直达（authorized_keys 仅台式机+笔记本两把密钥），跨任意网络、与物理拓扑解耦。
- **⭐ 网络拓扑定案（2026-09-26）**：台式机 ──Tailscale── 板子(dcztl/100.72.143.125) ──网线── **笔记本=常驻 ICS 网关**（以太网侧 192.168.137.1）──WiFi── 互联网。板子 netplan 双地址（192.168.10.2/24 静态 + dhcp4）⇒ 直连 PC / 接 ICS / 插路由器三场景自适应免改配。**运维纪律：笔记本以太网设"自动获得 IP"且永远不要手动设 IP**（ICS 会强制 137.1，手动 10.1/137.1 都会打架断网——09-26 事故根因）；WLAN 属性→共享→允许→选以太网。笔记本合盖/关机 = 板子断外网（Tailscale 掉线，本地系统照跑）。台式机 Tailscale 卡 "NoState/Tailscale is starting" = 服务卡死或丢登录态 → 重启服务（管理员 `net stop tailscale && net start tailscale`）大概率无效 ⇒ **别自己折腾，直接告诉用户去托盘重新 Login（同一 GitHub 账号）**（用户 2026-09-27 明确：没开就说一声，他自己点）。TF 卡那套（未完成）：见踩坑速记。
- **⭐ 烧录（绕开摘要校验）**：SoCToolKit v2.96"升级固件"模式对**改过字节的固件一律报"固件摘要检查失败"**（无厂商私钥不可重签）；正解 = MarkToolbox 自带经典工具 `C:\ProgramData\MarkToolbox\bin\upgrade_tool.exe`——板子进 MASKROM 后 `upgrade_tool.exe UF <img>`，**不校验摘要，14GB 约 6.5 分钟写完自动重启**。注意：镜像须 **512 字节对齐**（零填充补齐）；进 MASKROM = 救砖 USB-A 口 + A对A 线接 PC 上电，或 MASKROM 键。镜像结构：RKFW 容器 + 内嵌 RKAF 包（`0x77226`，文件表每项 0x70B）+ **整个文件即 ext4**（起点 `0x43B4226`=70992422，非 512 对齐）；离线改文件用 `debugfs`（loop 挂载 /mnt/d 会被 9P 强制只读）。**🔒 安全加固（2026-09-26）**：`sshd_config.d/99-hardening.conf` = `PasswordAuthentication no`（仅密钥：台式机/笔记本/MP157 三把）；NM 调试热点 profile（dcztl/12345678）已删；edge_hub/lpr_server 只绑 192.168.10.2（详见调试记录 §九"安全加固"）。
- **外设体检（全正常，43°C）**：GPU Mali 1GHz / **NPU RKNPU v0.9.8 1GHz（设备节点 = `/dev/dri/card1`+`renderD129`，新版驱动走 DRM，无 `/dev/rknpu` 属正常）** / WiFi 博通 bcmdhd / 蓝牙 hci0 / USB / HDMI×2 / 音频×4（HDMI×2+HDMI-in+ES8388）/ RTC / CAN / PWM。**⭐ NPU 全栈已通且识别实测正确（2026-09-26）**：板载 `pip3 install rknn-toolkit-lite2 rknn-toolkit2 onnx`（lite2/toolkit2 均 2.3.2）；**坑 1：PyPI 的 lite2 wheel 不含 librknnrt.so**（导入不报、get_sdk_version 返 None 才露馅），从 hf-mirror 下载 v2.1.0 `librknnrt.so` 放 `/usr/lib/` + ldconfig（与 driver 0.9.8 配对，版本 WARN 可忽略）。**坑 2（旧口径更正）：板载推理 `init_runtime()` 必须不带 target**——`target='rk3588'` 会走 adb/代理模式报 "Unsupported run platform: Linux aarch64"。**坑 3：输入必须 NHWC uint8**（transpose 成 NCHW 或预归一化 float 都解错）。转换键名是 `mean_values/std_values`（`input_mean/std` 已废弃）。**LPRNet 实测**：`lprnet.onnx`（D:\迅雷下载）转 rknn3588 后 NPU **2.5ms/帧**；实车牌裁图验证 **川A88888 conf 0.9864**（字符表 67+blank=67 已标定进 `rk3588_service/lpr_decode.py`）；模型已就位 `/root/models/lprnet.rknn`，服务代码 `/opt/rk3588_service/`。⚠️ 板子 rockchip adbd 的 `adb push` 坏（报成功不落盘）且 shell 命令 ≤4KB——**传文件一律走 Tailscale scp**（§九 调试通道）；板子会偶发冷启动掉 MASKROM（重上电即愈，连续失败就 UF 重刷）。
- **踩坑速记**：`chpasswd` 设 <8 位密码被 pwquality 拒 → 用 `usermod -p '<sha512哈希>' root`；最小改动版固件**未**含实验阶段注入的密码/密钥（原厂 root 密码未知，烧后必改）；A对A 线掉 maskrom 是救砖口设计行为不是故障。
- **⭐ 装机部署三坑（2026-09-26 实装踩平）**：⓪ **网线只有一根**：笔记本与 MP157 二选一插 RK（eth1=笔记本 / eth0=MP157），换调试通道=改插物理线，不是"两根都插"；① RK 网线必须插 **eth0**（eth1 是笔记本 ICS 口，插错现象 = MP157 侧 CARRIER=1 但 ARP 零应答，因为 eth1 配的是 137.99 不应 10.2 的 ARP）；② edge-hub 相机源 = `--source /dev/video4`（UVC 主相机，K210 ttyACM0 仅归档场景；板上 unit 已改，仓库模板同步）；③ systemd 服务**没有 HOME 环境变量** → hyperlpr3 的 `os.environ['HOME']` 抛 KeyError → recognizer=None → 表现为"有图无识别 + `NoneType has no attribute plate` 每周期刷错误"；unit 必须 `Environment=HOME=/root`。排查通道：台式机 COM4（CH9102，**复用 `$TEMP/serial_cmd.py`**，MobaXterm 需让口）→ MP157 → `ssh root@rk3588`（别名 `ssh rk`）免密跳板。
- **⭐ 双端逻辑审计存档（2026-09-27，历史优先复用）**：core0_service + C8T6 全量审查结论在 `docs/AGENTS_history.md` §代码审计-2026-09-27——**再审这两块先读存档只核对增量**。"如果只改三件事"已全部实施提交（core0 三件 + C8T6 四件中的 P1/P2/P3/P4；**剩余未做**：core0 开闸返回值/presence 自愈/窗口取消/result CAS、C8T6 P5 低照度/P6 CAN重试/P8 I2C 实测——见存档清单，按需点单）。
- **⭐ 笔记本链路"长时间断电连不上"根因与修复（2026-09-26 实锤）**：eth1（板载 RTL8111，EEPROM MAC F4:F4:DE:18:0D:F5 稳定；dmesg 里 permaddr 46:40:b0 是驱动早期随机占位，勿误判）被**双管理**——netplan/networkd 与 NetworkManager（nm-generated"有线连接 1"，存 /run 每次重启重新生成 → DHCP 身份每次不同）各跑一个 DHCP 客户端（实锤双租约 .202/.204、/var/lib/NetworkManager 几十份 internal-eth1.lease）+ Windows ICS 对长断电后"陌生"客户端 DISCOVER 响应极慢 + eth1 无静态兜底 ⇒ 连不上。**修复（收益最高的 b 方案，已生效）**：`99-eth-dbg.yaml` eth1 加静态兜底 `192.168.137.99/24` + 默认路由 via 137.1 metric 200 + DNS 137.1（DHCP 在时 metric 100 优先，ICS 不理时兜底通；ICS 对整个 137/24 NAT，静态客户端合法）。旧配置备份 `/root/netplan.yaml.bak`。**未做**：NM 钉 unmanaged（a 方案，用户裁决 b 收益更高）；笔记本侧 c 方案不适用（笔记本正常开机不自动共享）。改 netplan 用"guard 脚本 75s ping 不通网关自动回滚"保 Tailscale 通道。
- **启动介质定案（2026-09-26）**：日常 **eMMC 启动**（速度/可靠性/掉电/寿命全胜 TF 卡，且 maskrom 救砖已验证 ⇒ TF"拔卡重刷"的优势被抵消）；TF 槽留空或只放数据卡，**勿长期插可启动 TF 卡**（可能改变实际启动介质）；eMMC 整卡备份法（maskrom + `upgrade_tool.exe rd` 分段读回）见 `MD文档/rk3588/调试记录.md` §七。

## AGENTS-2026-09-28b（第三轮快照：初步验收通过后的更新版）

> 归并方式：`cat AGENTS.md >> docs/AGENTS_history.md` 字节级原样并入，未改动一字节（本节头部除外）。
> 本节 = 2026-09-28 傍晚"初步验收通过"后的根目录 AGENTS.md（在 09-28 首轮快照基础上新增：presence 持续认+新鲜结果采纳 **已上板验收**（3617923）、motion-hold 60s 已生效（a7e5b6c）、hyperlpr3rknn 接入完成、LCD 27fps 用户实测口径、README 纠偏提交（91f6f21）、固件存档目录建册、git 现状领先 4）。
> 当天快照关系：774 行 = 09-26 精简前版；964 行起 = 09-28 首轮（精简后现行版）；本节 = 验收后终版。

---

# 项目记忆（自动注入，勿删）—— 端侧AI · 边云协同停车场

> 本文件是跨会话的工作记忆，记录决定、板级事实、当前状态与下一步；不要与 README/Task 的设计正文重复，交叉处只引用。
> **📦 历史归档 `docs/AGENTS_history.md`**：2026-09-11 起多轮搬移（K210 识别时代全记录 / C8T6 / 第7步细节 / **2026-09-26 全量快照** + 第二轮精简索引 §AGENTS-2026-09-26b）。**本文件只留结论与当前状态**；要完整推理链去读存档。被取代的旧口径以存档为准，勿凭记忆复述。

## 一、项目一句话 & 文档分工
- 100ASK-MP157（STM32MP157DACx，双 A7 SMP + M4 FreeRTOS/OpenAMP）+ 外接 K210 + **RK3588（定昌 DC-A588，2026-09-25 入场）**的停车场**端侧AI·边云协同** Demo。
- `README.md`=定位/分工/硬件/目录/构建；`Task.md`=架构/拆解/数据交互/约束/KPI（内容隔离，勿重复）。**两处都有 09-21 后未纠偏的过时描述（见 §六-5）**。
- 字段级协议统一归 `docs/protocols.md`（改协议先改母本 `PhaseMd/10_协议规格总表` → 同步 protocols → 改代码 → 两处变更记录）。
- `MD文档/rk3588/调试记录.md` = RK3588 调通详录；`PhaseMd/` = 15 份执行层任务分解（P<步>-<序号>，验收门 G0~G8）。
- **📄 新增 .md 归口 `docs/`（2026-09-27 定）**：新写文档（设计/记录/调试/规范）一律放 `docs/`；`MD文档/`、`PhaseMd/` 属存量布局不强制搬迁，但新增不再进这两处；根入口文件（README.md / Task.md / AGENTS.md）除外。

## 二、当前确定架构
- **⭐ 车牌识别（2026-09-21 改向，取代"K210 本地 KPU 主责"旧口径）**：**K210 = 纯图像采集上行**——`other/k210_fw/main.py`（BUILD=2026-09-21-imgonly）只干 相机+JPEG+上行，不加载 SD/KPU/识别；旧识别固件归档在 `other/k210_fw/tmp_kpu_old/`。**识别主责 = RK3588 NPU**（LPRNet，§九）。**⭐ 识别引擎已切 HyperLPR3（2026-09-26 晚，9/9 板端回归全对）**：`edge_hub --engine hyperlpr3`（systemd 默认；CPU onnxruntime 68.8ms/帧）——RPNet v3 识别宽 160，治好了 LPRNet 的省份字混淆（京→津）和 <200px 掉尾字，还输出绿牌类型 ptype。LPRNet 线保留为 `--engine lprnet` 回退（+`--det-model yolov8s.rknn` = 两段式）。`--votes 2` 连续帧投票（`plate_vote.py`）两个引擎共用。检测器 A/B 定论：`Stara-AI/CarPlateDetection` yolov8n 宣传 mAP 0.994 但对大目标车牌零检出（训练分布窄），**弃用**；yolov8s rknn 留作回退。HyperLPR3 离线装法（板子无外网时）：台式机 pip download + 模型 zip scp 上板。**⭐ 回传通道已拍板（2026-09-26，方案 2）**：K210 物理上插 **RK3588**，`rk3588_service/edge_hub.py` 把 K210 行流 + 注入的 `K2:OK/NG` 识别结果经 **以太网 TCP :8089** 中继给 park_ui（`k210_link` mode=tcp，协议 `docs/protocols.md` §6）；备选方案 1（K210 留 MP157、图像转发 RK3588）记录在 §6.5 未实现。代码+宿主单测 32 项+板上识别实测全绿（川A88888 conf 0.986 / NPU 2.5ms）。**📦 2026-09-27 迁移定案**：`k210_fw/` → `other/k210_fw/`（K210 下线归档，commit `1ac911c`，19 文件 rename 历史保留；`main.py` 的 CAM_CROP 改进留在工作区归 RK3588 线提交；`tools/readme.txt` 按 *.txt 规则退库）。
- **车辆到位/道闸**：C8T6（BH1750 遮光 + SG90 + OLED）⇄ **CAN 500k** ⇄ M4(FDCAN2) ⇄ **RPMSG** ⇄ A7-Core0 业务判定。接口 v2 语义事件：CAN 帧表 = `docs/protocols.md` §1，RPMSG **0x21 = 9B**（`code|arg u16 LE|status|node_id|tick u32 LE`）。
- **⭐ 相机源（2026-09-27 改，V4L2 取代 K210 主线）**：杂牌 UVC 摄像头（icSpring 32e6:9221）直插 RK3588，走 `edge_hub --source /dev/video4`（`/dev/ttyACM0` = K210 备选源，一键切换）；相机 640x480@30 MJPG，中继 30fps 行流 → **LCD 实测显示最高 27fps**（状态栏 FPS 芯片；2026-09-28 用户实测口径，旧"relay-fps 15 → 14.8fps"作废）；识别仍 3s 周期。K210 已归档 `other/k210_fw/`（含 `CAM_CROP` 固件级取景框裁剪——小图编码快+车牌占满，备选路线）。**认节点用 `v4l2-ctl --list-devices`**：RK 上几十个 video 节点多为 rkisp/hdmi 内建设备，`video52` 是 HDMI-IN 骗点；opencv 在错节点 open 成功但 read 全 0，别只 `ls | tail`。
- **⭐ 云端 enabled 总开关（2026-09-27）**：`/etc/park/cloud.conf` `enabled=`（0=全禁）+ LCD 设置页"云端使能"勾选（即改即生效、持久），**低置信与识别失败两条路径都门控**——失败路径无阈值可挡，想零 cloud 请求只能靠它或"断网演练"（后者重启复位）。**真正的采纳/兜底门槛 = Core0 `conf_threshold`**（shm，core0.conf 热加载）；`trigger_conf`/`accept_conf` **未接线**（只解析+显示，别再调）。WiFi 掉线与云代码无因果：enabled=0 零流量后仍掉 → 路由器/云端侧（MP157 调试入口 = COM4 串口，WiFi 不稳不靠它）。
- **网络决策**：MP157 云端链路单用板载 WiFi；**MP157↔RK3588 = 板载网口直连：MP157 eth0 ↔ RK3588 eth0**（192.168.10.1↔10.2，主机名 `rk3588`，`rk3588-eth.sh` 幂等配置；唯一网线从笔记本让位过来）。当日踩坑：RTL8152 USB 网卡载波抖动退役；**RK 的 NetworkManager 抢 eth0 致静态 IP 时有时无** → 删 NM 配置 + `conf.d/99-eth0-unmanaged.conf` 钉 unmanaged（详见 `MD文档/rk3588/调试记录.md` §九）。**RK 调试 = MP157 跳板** `ssh -J root@<MP157> root@rk3588`，已免密（MP157 root 密钥注入 RK）；Tailscale 因 RK 断外网休眠，接回笔记本线即复活。断网本地闭环照常。

## 三、板级事实（MP157，从原理图 .DSN 挖出）
- I2C：I2C1=AP3216C+ICM-20608；触摸 I2C 在 LCD 排线口；**I2C4/I2C6 只能 A7**；I2C2(PH4/PH5) 未引出排针。
- **FDCAN = FDCAN2（PB5=RX/PB6=TX, AF9）**，Linux 节点 can0；**内核时钟实测 62.5MHz**（M4 位时序/文档均按此；早期"FDCAN1/PD0-PD1"误读作废）。
- 能插杜邦线的物理口仅 JTAG/SWD/Camera&Extend；板上另有 WiFi/SIM/WM8960/HDMI/以太网/USB/CAN(TJA1042)。
- **MP157 USB Host 供电 = GPIO 82(PF2)/139(PI11)**：禁 myir.service 后无人拉高 ⇒ `board-power.service` 负责（§七）；K210（CH9102 `1a86:55d4`）走 `cdc_acm` → `/dev/ttyACM0`。

## 四、M4 / C8T6 / core0 现状（结论版）
### M4（m4_fw/）
- 工程：.ioc 只配 FDCAN2 + FreeRTOS(CMSIS_V2, 堆 32768) + OPENAMP(rpmsg_bridge)；CAN_Rx_Task 10ms 轮询 + Rpmsg_Task；**I2C1/OLED 已从工程摘除**（过程 → history）。
- **✅ G3 全链路验收通过（2026-09-10）**：A7 0x11/0x12 → M4 → CAN 0x100 → C8T6 闸门真机闭环。**现行 can0 recipe（旧"down+unbind"作废）**：m_can 保持绑定 + `ip link set can0 type can bitrate 500000` + `can0 up` 点亮 fdcan_k 时钟，A7 静听不发；`load_m4.sh ensure_can_clock()` 已固化。**遗留**：C8T6 手遮→0x21、断电→0x22 未补测；**正式版 M4 未重烧**（后门宏源码已置 0，板端仍是验证版）。
- 按键：M4 不读底板 KEY（避免与 Linux input 争用），指令一律 RPMSG→M4→CAN。
- 写码依据速查（细节 → history 快照）：HAL 用 `HAL_FDCAN_AddMessageToTxFifoQ`（MP1 无 AddTxMessage）；CubeMX OPENAMP 勾法与 100ASK 例程在 `E:\download\100ASK-MP157\...22_A7_M4_UserModeComm\rpmsg_user\`；vring 由 Linux 分配；内核 5.4.31 双怪癖（NS 通道须先 `driver_override` 再 bind）；**HAL 头 ISO-8859 编码，grep 必须加 `-a`**。
### C8T6（c8t6/）
- CAN1 500k = APB1 36MHz / Prescaler 9 / Seg1 5 / Seg2 2；PA11/PA12 → TJA1050；**NART + ABOM 开**；FreeRTOS 堆 9600（4 任务+队列够）。
- **I2C 全软件模拟**（硬件 I2C1 不可靠已实锤）：`sw_i2c` PB8=SCL/PB9=SDA；SSD1306(0x3C)+BH1750(0x23) 共总线，`OledMutex` = 总线互斥；OLED_Task 栈 384。
- SG90 = TIM2_CH1(PA0) 50Hz PWM，`Gate_Poll` 10ms 缓动到位才报闸位；接口 v2 事件/心跳/查询全落地，**09-12 板验通过**；**未验 3 项** = 抓帧对字段、坏 CRC/10min soak、断电 0x22（不阻塞）。
- .ioc regen 必核对：TIM2 PWM、NART、堆 9600、OLED 栈 384（都会被回退）。
### core0_service（A7-0）
- **✅ 业务守护 P4-01~P4-08 全落地**：RPMSG 链路层（ttyRPMSG0 raw + 心跳 + 断链重开自动发 0x13 重同步）+ 业务（白名单/进出计数/开关闸判定）+ park_shm v3 双向 + 轻量文件记录（默认关）。selftest 142 项（两版）全过；板端验收用 `G4_ACCEPTANCE.md` + `core1_stub` 打桩；配置模板 `sample_core0.conf`。
- 消费方式：只用 `on_frame`/`on_link` 回调 + 0x23 快照（不碰 fd）；**v1 的 17B CAN 透传已废**（收到 len=17 会报 "v1 CAN passthrough? reflash M4"）。
- 工具：`load_m4.sh`（remoteproc + can0 时钟）、`rpmsg_cli`（tx/raw/bad/stats）；**改动后须 book 交叉编译** `make CROSS_COMPILE=arm-buildroot-linux-gnueabihf-` → `install_all.sh`。
- **⭐ 2026-09-28 "presence 持续认 + 新鲜结果即开闸"（`3617923`，selftest 186 项全绿；**已 book 编译部署 + 初步验收通过**）**：治用户报障"长时间遮光掉识别中、识别出来也不开闸"。① 超时 deny→cooldown→IDLE 后 `presence=1` 且本周期未出结果 → 自动重新 `start_recognition`（每 ~12s 一轮，直到认出或车离开）；② 结果采纳门由"必须 RECOGNIZING"放宽为 **"presence=1"**（Core1 实时推送 <1s，到达即新鲜 = 用户"3s 内"口径的字面实现；presence=0 照旧丢弃，保留审计 #5 防护）；③ 新增 **(presence, plate) 判定缓存**——同牌只判一次，防边缘侧 1/s 重发刷循环 deny/刷 cooldown；deferred 补识别清缓存（新车新周期）、车离开清 deferred。**根因**：C8T6 只发边沿，旧逻辑 IDLE+presence=1 无新 0x01 就死等，迟到结果被状态门丢弃。字面 3s 时间戳（shm v4 + Core1 盖章）分析过、暂不推荐。

## 五、git / 环境 / 工具备忘（纪律仍然有效）
- git 在 `C:\Program Files\Git\cmd\git.exe`（不在 PATH）；远程 SSH `git@github.com:randomlyiii/Project_EdgeParking-CloudSync.git` master；push 可用（失败加 `GIT_SSH_COMMAND="ssh -o BatchMode=yes"`）。**AGENTS.md 已被 .gitignore（纯本地）**。
- **git 现状（2026-09-28）**：早前 21+ 提交已由其他会话 push；现本地领先 origin **4**（`3617923` fix(core0) presence 持续认 / `a7e5b6c` fix(rk3588) motion-hold 60s / `91f6f21` docs README 纠偏 / `c06d727` docs AGENTS 归档补录）待 push。
- **⛔ 不要自动 git 提交**：改完只留工作区，等用户明确说"提交/commit"；提交信息临时文件写仓库外（`$env:TEMP\parkmsgN.txt` + `git commit -F`，中文不进命令行）。
- **🧩 一功能 = 一提交**：先按功能分组自查 `git diff HEAD --stat`；同文件多功能用 `git add -p` 拆 hunk；**docs 改动独立成 `docs(...)` 提交**。细则全文 = `MD文档/Git_Update_standard.md`（2026-09-27 建）。
- **🔐 sample_ 约定**：真文件（`/etc/park/cloud.conf`、`wpa_supplicant.conf`、`park-ui.env`、`core0.conf`、`key.txt`）一律 gitignore，仓库只放纯 ASCII `sample_*` 模板；`check_static.py` 第 8 节门禁（含负向测试）。
- 环境：本机无 arm-none-eabi ⇒ STM32 工程在 CubeIDE 编；`cubekill.bat` 清构建产物；参考资料 `E:\download\100ASK-MP157\100ask-mp157原理图\01_Base_board(底板)\`（.DSN 可 grep 明文，PDF 无法提取）；**⛔ 板端粘贴代码纯 ASCII、禁中文**（SSH 粘贴中文必坏）；板端 root 无 sudo、无 pgrep/timeout，`/tmp` 重启即清。
- **K210 部署纪律**：CanMV IDE「保存到设备」才驻板；验证一律存 .py + 冷启动；**勿用 IDE「运行」**（软重启不还内存）。

## 六、下一步建议（2026-09-26 更新）
0. **今天收尾（2026-09-26 装机日）**：全链路已通——RK eth0 直连 MP157（网口插 eth0！eth1 是笔记本 ICS 口）、edge-hub 相机源 /dev/video4 + hyperlpr3（unit 加 `Environment=HOME=/root` 修复识别静默失效，仓库模板同步提交 `265bd81`）、LCD 有图、no_plate 3s 轮询正常。**遗留动作 = 举牌实测弹卡**（打印川A88888 入镜 → 预期 hold→OK 两周期 ~6s）。排查通道：台式机 COM4（MobaXterm 需让口）→ MP157 → `ssh root@rk3588` 免密跳板。
1. **✅ HyperLPR3 → RKNN 已转换+对拍（2026-09-27，细节 = `参考资料/rk3588模型/hyperlpr3/RESULTS.md`）**：y5fu_320x/640x + rpv3 三个 FP16 rknn 已生成并持久化（板上 `/root/models/`，PC 同版备份），**NPU vs CPU 引擎 3/3 全对**（含 14% 缩小/8° 旋转），320 档 det+rec 合计 **~18ms**（CPU 69-131ms）、640 档 ~46ms。**纠正两个预估**：①检测**不是 yolov5 anchor**——`y5fu_*_sim` 是单融合头 (1,N,15) **anchor-free**（"sim"=sigmoid 已焊死，**解码全用 raw logits**：obj>0.25 → 角点仿射校正 → NMS），生产参考 `hyperlpr3.inference.multitask_detect.post_precessing`；②字符表 **78 token 非 65**（rpv3 输出 (1,T,78)，blank=0）。rknn 输入 NHWC uint8（det mean0/std255、rec 127.5，BGR→RGB 自做，rec 右补 127）。⚠️ `hyperlpr3.inference.*` 的 `@cost` 装饰器板上 import 即炸，只能 import `common.*`+`multitask_detect` 函数。conf 口径待对齐（rknn softmax 均值 0.03 vs CPU 0.999，字串全对）。**✅ 验收已过（2026-09-27，COM4 通道实操）**：8 图集（原图/14%/50%/25%/模糊/变暗/±旋转）320 档 **8/8 与 CPU 全对**（det 11-17ms+rec 6-12ms）；640 档 7/8（高斯模糊图少认一个 8，320 反而对）——**集成取 320 档**。**✅ 已接入 edge_hub 并切默认（2026-09-28，unit `--engine hyperlpr3rknn` 在跑）**：`hyperlpr3_rknn.py`（anchor-free raw-logits 解码 + 78-token CTC，conf=逐字原始最大均值**绝不可再 softmax**）；板上实测 det+rec ~18ms、验收识别正确。
2. **✅ 识别帧差门控已落地（2026-09-27，方案 B：相机连续帧运动检测）**：`motion_watch.py` MotionWatch——CameraThread 用现成 BGR 帧降采样 64x48 灰度 MAD（threshold 2.0）喂 watch，RecogThread 查询 `motion_recent()`（hold 9s 无运动才睡）；**hold 窗口一个参数替代状态机**（=滞回+即时唤醒+冷启动 9s 宽限，a 方案的 arm/probe/首帧特判全删）；**投票 pending 非空时禁止休眠**（`PlateVoter.has_pending()`，防车停稳后投票卡 1/N 弹卡永不触发）；睡着每 30s 打 `recog: idle`，唤醒打 `recog: wake (motion)`；CLI `--motion-threshold 2.0 / --motion-hold 9`（0=关）。**⭐ 2026-09-28：hold 9s→60s（`a7e5b6c`，仓库模板 + 板上 unit 均已改、restart 生效）**——9s 睡死会让 core0 重识别也等不到结果（跨板耦合死锁）；demo 下识别空转成本 ≈ NPU 0.25%，休眠收益本就虚。不动相机帧率。宿主单测 68 项全绿（含"静态冻结/运动唤醒/投票钉住"三个集成用例）。
1. **RK3588 侧（代码+部署已落地，剩物理连线）**：`rk3588_service/` 全套——edge_hub（K210 reader + LPRNet 周期识别 + TCP :8089 行中继）+ lpr_server（HTTP 测试台 :8088）+ lpr_decode（字符表 2026-09-26 实车标定）+ 宿主单测 32 项 + `tests/itest_edge_hub.py` 板上端到端（pty 假 K210）PASS + systemd `edge-hub`/`lpr-server` 已 enable；模型 `/root/models/lprnet.rknn`，板上实测 川A88888 conf 0.9864 / NPU 2.5ms / HTTP 8.5ms，Tailscale 外部链路 curl 亦通。**剩**：K210 插到 RK3588 后看 `journalctl -u edge-hub -f` 出 `recog: OK`；注意 **brltty 抢 ttyACM0**（GNOME 桌面常见，`systemctl mask brltty`）。
2. ~~拍板：RK3588 识别结果怎么回 MP157~~ **已拍板（2026-09-26）= 方案 2 以太网 TCP 行中继**（edge_hub 复用 K2: 行协议注入 K2:OK/NG，park_ui `k210_link` 加 mode=tcp 已落地）；云兜底归属不变（Core1 收 `recogFailed` 后走既有 DeepSeek 链）。
3. **MP157 收尾（小活）**：M4 正式版重编重烧；park_ui 若按拍板改 K210 消费逻辑，book `build_arm.sh` 重编 + scp。
4. **验收欠账**：现场验收剩：C8T6 审计四修（P1 settled 语义/P2 重发槽/P3+P4 基线防护，**代码已提交 `bc8f9a5`/`064782e`/`1cbb228` 但固件未烧**——CubeIDE 重编烧录后补实测）+ C8T6 三项老补测（抓帧对字段/坏CRC soak/断电 0x22）；P6-05/06/07 埋点随验收补；**other/k210_fw/tools 回归套件已坏（09-22 单文件重写后未同步）**：`build_main.py --check` 找已归档的 `park_app.py`、多个 test 引用 `sd_probe2.py` → discover 8 errors；**改 other/k210_fw 前先把测试套件按 imgonly 单文件现状重写**。**⭐ core0 审计三修已提交并上板（2026-09-27，`7f203f9`/`c7d129f`/`19e2479`，宿主 selftest 164 项全绿）**：白名单热加载文件权威化、冷却期到达补识别（arrive_deferred）、publish 保待消费结果三字段。板上 `core0.conf` 现值：recog_timeout=8000ms / cloud_timeout=12000ms / 白名单含京AD06088（排障期调宽，功能正常保留）。**⭐ 2026-09-28 presence 持续认+新鲜结果采纳已提交并上板初步验收通过（`3617923`）**：新二进制由 book 编译 scp 上板（连同 park_ui 397KB / core0_business 161KB / core1_stub 已回存 `stm32mp157-已编译固件/`），长时间遮光场景回归通过——掉识别中后自动恢复、结果出即开闸。**排障陷阱记**：遮光/闸令全走 CAN——**CAN 断连的现象 = "识别成功（LCD 弹卡正常）但不开闸"**，先查 can0/M4 再怀疑业务。
5. **文档纠偏（2026-09-26 大头已做）**：README 已重写（RK3588/C8T6 入列、K210 纯发图、core1_ui=qmake、开机链补 board-power 与 rk3588-eth）；Task.md 识别相关段落已同步；protocols.md §6 + 母本 §7 已建。**剩**：`MD文档/rk3588/调试记录.md` 补 librknnrt 版本错配与 `init_runtime()` 无 target 两条（见 §九更正）。

## 七、Qt GUI / 部署（当前有效事实）
- **编译**：只能 book SDK 的 qmake（Qt 5.12.8，`/home/book/100ask_stm32mp157_pro-sdk/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin/qmake` + 同目录 g++）；OpenSTLinux Qt 5.14.1 编出的板端跑不了（`version Qt_5` 报错）。配方固化在 `core1_ui/qt_gui/tools/build_arm.sh`，坑见 `qt_gui/README.md` §3。
- **部署**：`scp bin/park_ui root@192.168.189.65:` → 板端 `/opt/park_ui/park_ui`；前台跑要 `< /dev/null`（否则 SIGTTIN 停住）。
- **板端 Qt**：5.12.8；插件目录 `/usr/lib/qt/plugins/platforms`（有 libqlinuxfb.so）；`QT_QPA_PLATFORM='linuxfb:fb=/dev/fb0'`；`/etc/profile` 的 QT_* 对 systemd 无效。
- **LCD**：park_ui linuxfb 独占常显（park-ui.service）；**1/4 屏根因** = linuxfb 无 WM 时 showFullScreen 不改几何 ⇒ `mainwindow.cpp` linuxfb 分支显式 `setGeometry(screen()->geometry())`（勿回退）。
- **启动链**：`board-power → m4-load → core0-bus → rk3588-eth → park-ui`（rk3588-eth = 静态 192.168.10.1/24 + hosts `rk3588`→.2，幂等脚本，IFACE=eth0 板载口）；WiFi/时钟不在关键链。park_ui 的 K210 上行默认 **tcp 模式**（`PARK_UI_K210_TCP`，默认 `rk3588:8089`；显式置空回退本地串口）。安装器 `deploy/systemd/install_all.sh`（**10 步**；装 /opt/core0 + /opt/park_ui + /lib/firmware/m4_fw.elf + rk3588-eth；**不覆盖已有 core0.conf**）。**park_ui 新二进制已部署（2026-09-27）**：状态栏 FPS 芯片 + 云端 enabled 门控；LCD 上屏实测最高 27fps（2026-09-28 口径，旧"60ms 拉帧 15fps"作废）。
- **⚡ board-power.service**：开机拉高 GPIO 82/139 给 USB HUB 供电（禁 myir 的代价），已跨重启验证；没它 K210/U 盘全消失。
- **✅ P6-04 已板验**：`ipc_writer` O_RDWR 挂 /park_shm、1s 心跳、结果回写、低置信→cloud_pending、req_gate_* 脉冲、底栏触摸开/关闸 + O/C 热键。
- **📺 K210 日志上 LCD（09-16）**：k210_link `feedText()` 全量转发 → journald + 事件条筛选（[BOOT]/[MEM]/[SD]/SDX:/fail/error 等，高频限流 5s）。K210 不再发结果行后，此通道只看诊断。
- **弹卡/结果链路现状**：**已重接（2026-09-26 起）**——edge_hub 经 TCP :8089 中继注入 `K2:OK/NG:` 行，cam_link tcp 模式解析 → `recogResult/recogFailed` → 弹卡 + shm 回写 core0。旧"待拍板重接"口径作废。
- 已修机制在代码里（**勿回退**）：关闸乐观态 + `GATE_SETTLE_MS=1600` 延迟重同步 + 周期 0x13、`GATE_REPORT_GUARD_MS=1500` 防闪变；云失败分类 no network/certificate；wifi_up.sh 关联门 + 15s DHCP 截止；布局用 `Preferred + setMinimumWidth(0)`（**勿用 QSizePolicy::Ignored**，芯片会消失）。

## 八、第7步 云端（操作要点仍然有效）
- **云代码唯一在 `core1_ui/qt_gui/src/`**（cloud_client/cloud_settings，装配 main.cpp；**Core0 禁云**；`core0_service/cloud`、`core1_ui/cloud_api` 占位目录已删，`check_static.py` 第 9c 节负向断言）。
- `/etc/park/cloud.conf`（600，模板 `deploy/sample_cloud.conf`，**每个 provider 一把 key**）；CA 自备 `/etc/park/ca.pem`；**无 RTC ⇒ 先校时否则证书"尚未生效"**；`transport=auto|qt|python`（Qt+OpenSSL1.1.1 在 TLS1.3 卡死，python 通道 ~1s）；`timeout_ms` 上限 ~6000。
- **✅ 端到端已真机验证（09-11）**：transport=python + DeepSeek 实测识别出车牌。实现细节 → history §第7步-2026-09-11。
- **🔒 本机 IP/网关不上屏不落盘**（门禁：wifi_manager 无 QString ip/gw、不解析 /proc/net/route、全 GUI 源码无 `192.168.` 字面量）；WiFi 芯片只显示 SSID。
- **WiFi 运维**：wifi-up.service 开机自连（厂商三步固化在 `deploy/systemd/wifi_up.sh`，无 AP 时关联门跳过 DHCP）；设置页「网络」三动作：联网=只动链路、保存=只写 wpa_supplicant.conf、连接=写+应用+20s 无租约回滚；无租约时事件行提示 `systemctl restart wifi-up`。
- **K210 端两条硬规矩（纯发图固件仍在用）**：`jpeg_from()` 失败必打原因、**不许**退回裸 RGB565；`console_send_jpeg()` 整段 try/except + 限频 `_preview_drop`（预览失败绝不许掀掉主循环）。

## 九、RK3588（定昌 DC-A588）板级事实（2026-09-25 调通；详录 `MD文档/rk3588/调试记录.md`）
- **板子**：定昌 **DC-A588**（=飞牛 NAS 同款底板），RK3588 八核 / 8G RAM / **64G eMMC** / 双千兆网口（eth0/eth1）。原厂固件 = GB-RK3588 Ubuntu 24.04 GNOME（5.10.198 vendor 内核）。USB 口分工：**仅 Type-C 口**（otg 控制器 dr_mode=otg）支持设备模式跑 adb；**USB-A 口全部 host-only**（dtb 焊死），且其中一口兼 **MASKROM 救砖口**——A对A 线接上电即进 maskrom，**勿当调试口**。
- **在板固件** = 原厂 + 仅两处修复：① `/etc/init.d/.usb_config` 带前导空格致 `case` 永不命中 → 改精确 `usb_adb_en`（上电自启 adbd）② `/etc/rc.local` `echo host`→`peripheral`（锁死 OTG 设备模式，防开机末段顶掉 gadget）。烧后已配：root 密码 **rk3588**、netplan `192.168.10.2/24` 静态 + `dhcp4` 双配置（持久化）。镜像文件 `D:\BaiduNetdiskDownload\GB-RK3588-gnome-minimal-adb-on.img`。
- **调试通道**（均真机验证）：① `adb shell` = A对C 线插板子 Type-C 口，**直接 root**；② `ssh root@192.168.10.2` = 网线直连 PC/笔记本（PC 侧设 `192.168.10.1/24`，**网关/DNS 留空**），插路由器则自动 DHCP 拿第二地址；③ **⭐ Tailscale（主远程通道）** = `ssh -i ~/.ssh/id_ed25519 root@100.72.143.125` 免密直达（authorized_keys 仅台式机+笔记本两把密钥），跨任意网络、与物理拓扑解耦。
- **⭐ 网络拓扑定案（2026-09-26）**：台式机 ──Tailscale── 板子(dcztl/100.72.143.125) ──网线── **笔记本=常驻 ICS 网关**（以太网侧 192.168.137.1）──WiFi── 互联网。板子 netplan 双地址（192.168.10.2/24 静态 + dhcp4）⇒ 直连 PC / 接 ICS / 插路由器三场景自适应免改配。**运维纪律：笔记本以太网设"自动获得 IP"且永远不要手动设 IP**（ICS 会强制 137.1，手动 10.1/137.1 都会打架断网——09-26 事故根因）；WLAN 属性→共享→允许→选以太网。笔记本合盖/关机 = 板子断外网（Tailscale 掉线，本地系统照跑）。台式机 Tailscale 卡 "NoState/Tailscale is starting" = 服务卡死或丢登录态 → 重启服务（管理员 `net stop tailscale && net start tailscale`）大概率无效 ⇒ **别自己折腾，直接告诉用户去托盘重新 Login（同一 GitHub 账号）**（用户 2026-09-27 明确：没开就说一声，他自己点）。TF 卡那套（未完成）：见踩坑速记。
- **⭐ 烧录（绕开摘要校验）**：SoCToolKit v2.96"升级固件"模式对**改过字节的固件一律报"固件摘要检查失败"**（无厂商私钥不可重签）；正解 = MarkToolbox 自带经典工具 `C:\ProgramData\MarkToolbox\bin\upgrade_tool.exe`——板子进 MASKROM 后 `upgrade_tool.exe UF <img>`，**不校验摘要，14GB 约 6.5 分钟写完自动重启**。注意：镜像须 **512 字节对齐**（零填充补齐）；进 MASKROM = 救砖 USB-A 口 + A对A 线接 PC 上电，或 MASKROM 键。镜像结构：RKFW 容器 + 内嵌 RKAF 包（`0x77226`，文件表每项 0x70B）+ **整个文件即 ext4**（起点 `0x43B4226`=70992422，非 512 对齐）；离线改文件用 `debugfs`（loop 挂载 /mnt/d 会被 9P 强制只读）。**🔒 安全加固（2026-09-26）**：`sshd_config.d/99-hardening.conf` = `PasswordAuthentication no`（仅密钥：台式机/笔记本/MP157 三把）；NM 调试热点 profile（dcztl/12345678）已删；edge_hub/lpr_server 只绑 192.168.10.2（详见调试记录 §九"安全加固"）。
- **外设体检（全正常，43°C）**：GPU Mali 1GHz / **NPU RKNPU v0.9.8 1GHz（设备节点 = `/dev/dri/card1`+`renderD129`，新版驱动走 DRM，无 `/dev/rknpu` 属正常）** / WiFi 博通 bcmdhd / 蓝牙 hci0 / USB / HDMI×2 / 音频×4（HDMI×2+HDMI-in+ES8388）/ RTC / CAN / PWM。**⭐ NPU 全栈已通且识别实测正确（2026-09-26）**：板载 `pip3 install rknn-toolkit-lite2 rknn-toolkit2 onnx`（lite2/toolkit2 均 2.3.2）；**坑 1：PyPI 的 lite2 wheel 不含 librknnrt.so**（导入不报、get_sdk_version 返 None 才露馅），从 hf-mirror 下载 v2.1.0 `librknnrt.so` 放 `/usr/lib/` + ldconfig（与 driver 0.9.8 配对，版本 WARN 可忽略）。**坑 2（旧口径更正）：板载推理 `init_runtime()` 必须不带 target**——`target='rk3588'` 会走 adb/代理模式报 "Unsupported run platform: Linux aarch64"。**坑 3：输入必须 NHWC uint8**（transpose 成 NCHW 或预归一化 float 都解错）。转换键名是 `mean_values/std_values`（`input_mean/std` 已废弃）。**LPRNet 实测**：`lprnet.onnx`（D:\迅雷下载）转 rknn3588 后 NPU **2.5ms/帧**；实车牌裁图验证 **川A88888 conf 0.9864**（字符表 67+blank=67 已标定进 `rk3588_service/lpr_decode.py`）；模型已就位 `/root/models/lprnet.rknn`，服务代码 `/opt/rk3588_service/`。⚠️ 板子 rockchip adbd 的 `adb push` 坏（报成功不落盘）且 shell 命令 ≤4KB——**传文件一律走 Tailscale scp**（§九 调试通道）；板子会偶发冷启动掉 MASKROM（重上电即愈，连续失败就 UF 重刷）。
- **踩坑速记**：`chpasswd` 设 <8 位密码被 pwquality 拒 → 用 `usermod -p '<sha512哈希>' root`；最小改动版固件**未**含实验阶段注入的密码/密钥（原厂 root 密码未知，烧后必改）；A对A 线掉 maskrom 是救砖口设计行为不是故障。
- **⭐ 装机部署三坑（2026-09-26 实装踩平）**：⓪ **网线只有一根**：笔记本与 MP157 二选一插 RK（eth1=笔记本 / eth0=MP157），换调试通道=改插物理线，不是"两根都插"；① RK 网线必须插 **eth0**（eth1 是笔记本 ICS 口，插错现象 = MP157 侧 CARRIER=1 但 ARP 零应答，因为 eth1 配的是 137.99 不应 10.2 的 ARP）；② edge-hub 相机源 = `--source /dev/video4`（UVC 主相机，K210 ttyACM0 仅归档场景；板上 unit 已改，仓库模板同步）；③ systemd 服务**没有 HOME 环境变量** → hyperlpr3 的 `os.environ['HOME']` 抛 KeyError → recognizer=None → 表现为"有图无识别 + `NoneType has no attribute plate` 每周期刷错误"；unit 必须 `Environment=HOME=/root`。排查通道：台式机 COM4（CH9102，**复用 `$TEMP/serial_cmd.py`**，MobaXterm 需让口）→ MP157 → `ssh root@rk3588`（别名 `ssh rk`）免密跳板。
- **⭐ 双端逻辑审计存档（2026-09-27，历史优先复用）**：core0_service + C8T6 全量审查结论在 `docs/AGENTS_history.md` §代码审计-2026-09-27——**再审这两块先读存档只核对增量**。"如果只改三件事"已全部实施提交（core0 三件 + C8T6 四件中的 P1/P2/P3/P4；**剩余未做**：core0 开闸返回值/presence 自愈/窗口取消/result CAS、C8T6 P5 低照度/P6 CAN重试/P8 I2C 实测——见存档清单，按需点单）。
- **⭐ 笔记本链路"长时间断电连不上"根因与修复（2026-09-26 实锤）**：eth1（板载 RTL8111，EEPROM MAC F4:F4:DE:18:0D:F5 稳定；dmesg 里 permaddr 46:40:b0 是驱动早期随机占位，勿误判）被**双管理**——netplan/networkd 与 NetworkManager（nm-generated"有线连接 1"，存 /run 每次重启重新生成 → DHCP 身份每次不同）各跑一个 DHCP 客户端（实锤双租约 .202/.204、/var/lib/NetworkManager 几十份 internal-eth1.lease）+ Windows ICS 对长断电后"陌生"客户端 DISCOVER 响应极慢 + eth1 无静态兜底 ⇒ 连不上。**修复（收益最高的 b 方案，已生效）**：`99-eth-dbg.yaml` eth1 加静态兜底 `192.168.137.99/24` + 默认路由 via 137.1 metric 200 + DNS 137.1（DHCP 在时 metric 100 优先，ICS 不理时兜底通；ICS 对整个 137/24 NAT，静态客户端合法）。旧配置备份 `/root/netplan.yaml.bak`。**未做**：NM 钉 unmanaged（a 方案，用户裁决 b 收益更高）；笔记本侧 c 方案不适用（笔记本正常开机不自动共享）。改 netplan 用"guard 脚本 75s ping 不通网关自动回滚"保 Tailscale 通道。
- **启动介质定案（2026-09-26）**：日常 **eMMC 启动**（速度/可靠性/掉电/寿命全胜 TF 卡，且 maskrom 救砖已验证 ⇒ TF"拔卡重刷"的优势被抵消）；TF 槽留空或只放数据卡，**勿长期插可启动 TF 卡**（可能改变实际启动介质）；eMMC 整卡备份法（maskrom + `upgrade_tool.exe rd` 分段读回）见 `MD文档/rk3588/调试记录.md` §七。
