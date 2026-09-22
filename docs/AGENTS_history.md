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
