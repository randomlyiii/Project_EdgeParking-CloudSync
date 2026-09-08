# A7↔M4 RPMSG 通信规范 — STM32MP157 (Linux A7 ↔ FreeRTOS M4 OpenAMP)

> 适用范围: 第 3 步 Linux(Cortex-A7) ↔ M4(FreeRTOS+OpenAMP) 用户态 rpmsg 链路
> 最后更新: 2026-09-10（创建：RPMSG 板端打通实测沉淀；两次全通板验 + 5.4 BSP 双怪癖 + 重启 flaky 已知问题）
> 关联文档: `docs/protocols.md` §3（帧协议权威定义）、`m4_fw/rpmsg.md`（CubeMX 配置/bringup 步骤）、`PhaseMd/04`（第3步任务分解）

---

## 1. 架构与链路

```
Linux A7 (master)  <--virtio rpmsg / IPCC mbox-->  M4 (remote, RPMSG_REMOTE)
  core0_service/rpmsg/*         |                m4_fw/CM4/Core/rpmsg_bridge.{c,h}
  /dev/ttyRPMSG0  (rpmsg-tty)    |                VIRT_UART("rpmsg-tty") + Rpmsg_Task
                                |                     |(OpenAMP)
                             CAN 0x100/0x110/0x200/0x210 <-> C8T6
```

| 项 | 值/位置 |
|----|---------|
| 通道服务名 | `rpmsg-tty` → Linux `/dev/ttyRPMSG0`（内核 CONFIG_RPMSG_TTY） |
| M4 角色 | `RPMSG_REMOTE`（openamp_conf.h 定 `LINUX_RPROC_MASTER`，vring 地址由 Linux 分配） |
| A7 侧代码 | `core0_service/rpmsg/{rpmsg_types.h,rpmsg_proto.{c,h},rpmsg_link.{c,h},rpmsg_demo.c}` + `tools/{load_m4.sh,rpmsg_cli/}` |
| M4 侧代码 | `m4_fw/CM4/Core/{Inc,Src}/rpmsg_types.h/rpmsg_proto.{c,h}/rpmsg_bridge.{c,h}`（协议副本=A7 侧拷贝，单一事实源在 A7）+ `CM4/OPENAMP/`（CubeMX 生成） |
| 共享内存 | M4 ld: SRAM3 `0x10040000` 64K（`__OPENAMP_region_*`）；板载 DT reserved-memory: vdev0vring0@0x10040000(4K) / vdev0vring1@0x10041000(4K) / vdev0buffer@0x10042000(16K) |
| 帧协议 | `AA 55 | type | seq(2 LE) | len(2 LE) | payload | CRC16-XMODEM(2 LE, type..payload)`，详见 `docs/protocols.md` §3 |

线程模型铁律（M4 侧）：**OpenAMP 非线程安全，全部 OpenAMP 调用收敛在 Rpmsg_Task 单任务**；VIRT_UART RX 回调跑在调用者上下文，只允许 memcpy+置标志，禁调 FreeRTOS API。

---

## 2. M4 工程配置（CubeMX）要点

Middleware→OPENAMP 为虚拟外设（`VP_OPENAMP_VS_OPENAMP.Mode=OpenAmp_Activated`），勾上自动带出 IPCC + NVIC `IPCC_RX1/TX1` + `CM4/OPENAMP/` 8 文件 + main.c 插 `MX_IPCC_Init(); MX_OPENAMP_Init(RPMSG_REMOTE, NULL);`。详细步骤/fallback 见 `m4_fw/rpmsg.md`。

> ⚠️ regen 安全：freertos.c 里 Rpmsg_Task 的句柄/属性必须放 **USER CODE 区**（放生成区会被 regen 擦除 → 编译报 undeclared）；rpmsg_bridge.c 需显式 `#include "virt_uart.h"`（生成 openamp.h 不保证带出）。

---

## 3. Linux 端 Bringup 标准流程（实测定版 2026-09-10）

### 3.1 加载

```sh
echo m4_fw.elf > /sys/class/remoteproc/remoteproc0/firmware   # 重启后必须重设(默认名 rproc-m4-fw 会 -2 失败)
echo start > /sys/class/remoteproc/remoteproc0/state
sleep 3
dmesg | grep -i channel     # 期望: creating channel rpmsg-tty addr 0x400
```

### 3.2 5.4 BSP 双怪癖 → 必须手动绑驱动（**先 override 再 bind**）

实测内核 = **5.4.31**（100ASK BSP，非 6.6）。两个坑：
1. 远端 NS 建出的通道**不会自动绑** rpmsg_tty 驱动（`creating channel` 有，/dev/ttyRPMSG0 不出现）；
2. **直接 `echo <dev> > .../bind` 报 `No such device`**（5.4 rpmsg 总线 id 匹配 bug）——必须先写 driver_override。

```sh
d=/sys/bus/rpmsg/devices/virtio0.rpmsg-tty.-1.1024            # 名字以实际为准
echo rpmsg_tty > $d/driver_override
echo ${d##*/} > /sys/bus/rpmsg/drivers/rpmsg_tty/bind
ls -l /dev/ttyRPMSG0                                          # crw-rw---- root dialout 5,3
```

`core0_service/tools/load_m4.sh` 的 `bind_channel()` 已内置（先 override 后 bind，幂等，M4 重启后自动补绑）。

### 3.3 M4 侧端点创建策略（补丁，防首 NS 被吞）

rpmsg_bridge.c：`VIRT_UART_Init` 不在一进任务就做，改为**任务启动 1s 后首试、失败每 2s 自动重试**；结果可见于 `g_rpmsg_bridge_mon.vuart_ready / vuart_init_rc(0=OK/1=ERR/0xFF=未试) / vuart_init_attempts`。实测旧版"开机即发"常丢首个 NS（mbox IRQ 恒 1、无 channel）。

### 3.4 python3 免编译验收（板端无 make/gcc）

板端 100ASK 镜像**没有 gcc/make**，core0_service 的 C demo 无法在板上编译；验收用纯 stdlib python3（见下方 §5 或会话记录）。正式 A7 服务需交叉编译（100ASK SDK/工具链待定）。

---

## 4. 板验记录与已知问题

### 4.1 已验证（两次全通）
- 内核 dyn debug 完整收到 NS 帧（"rpmsg-tty"+addr 0x400 逐字节正确）→ `creating channel` → `Received 1 messages`；
- python3 验收：`0x13` 查询 → 即时 `0x23`（gate/online/can_err），0x7E 心跳 ~9 个/6s，**CRC 0 错**，双向通。

### 4.2 ⚠️ M4 重启 flaky（未根治，实测成功 2/9）

| 现象 | 失败特征 = mbox IRQ 78 恒 1（GIC-0 133, 4c001000.mailbox），M4 首 kick 后内核无后续处理；dyn debug 下连 NS 帧都没有 |
|------|------|
| 已证伪 | 缓冲地址假说（成功/失败 kernel `buffers: dma` 都是 0xd8042000 DDR）；冷/软重启差异；固件版本差异 |
| 粗略规律 | 静默失败会话**静坐几分钟后** stop/start 更易成（118→786 隔 668s ✓、37→350 隔 311s ✓）；活跃会话停止后立刻重启基本必败（786→2005 ✗）；失败会话快速重启也败（87~160s 间隔 ✗） |
| 开发建议 | M4 起来后勿频繁重启；重启失败 → stop → 等 2~5 分钟 → 再 start，或直接重启系统；换固件同理 |

### 4.3 dyn debug 排障法（重启即失效，需每次重开）

```sh
mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo 'file drivers/rpmsg/virtio_rpmsg_bus.c +p' > /sys/kernel/debug/dynamic_debug/control
echo 'file drivers/remoteproc/remoteproc_virtio.c +p' > /sys/kernel/debug/dynamic_debug/control
# stop/start 后 dmesg 可见: vring0/1 地址 qsz、buffers dma、kicking、NS 帧 hexdump、Received N messages
```

### 4.4 已排除清单（勿重复排查）
旧固件（能注册 virtio0 必含 OPENAMP rsc table）；D-Cache（工程无 cache/MPU 使能）；main.c 流程（=ST 模板）；mbox_ipcc stop-ack（`shutdown without ack` 正常，无 shutdown 通道回调）；RPMSG_REMOTE 下 shpool base=-1 无害（缓冲由主端预填）；DT carveout 缺失（节点齐全）；IPCC 寄存器残留（读值多为保留位全 1，无法直接判读）。

### 4.5 遗留
① C8T6 端到端（0x11/0x12→CAN 0x100→SG90、手遮→0x21、断链 0x22）；② A7 正式服务交叉编译方案；③ 用户现场反馈"共地线疑似松动"——C8T6/CAN 联调前先查接线。

---

## 5. 验收清单（G3 状态：传输层 ✅ / 全链路 ⏳）

- [x] ttyRPMSG0 出现，0x13→0x23、0x7E 心跳、CRC 0 错（python3 免编译脚本，两次全通）
- [ ] 0x11/0x12 开/关闸经 M4 → CAN 0x100 → C8T6 → SG90（需 C8T6 上总线）
- [ ] 手遮 BH1750 → 0x200 → M4 → A7 收 0x21（17B: id+dlc+data+tick）
- [ ] C8T6 断链 → M4 发 0x22 边沿、恢复再 0x22
- [ ] A7 侧断链/重连：rpmsg_link 1s 判 LINK_DOWN → 重开 → 自动 0x13 重同步

验收脚本（板端 python3，ASCII）：

```python
# 0x13 查询 + 统计 0x23/0x7E（帧格式见 docs/protocols.md §3）
import os, select, termios, tty, time
def crc16(d):
    c=0
    for b in d:
        c^=b<<8
        for _ in range(8):
            c=((c<<1)^0x1021)&0xFFFF if c&0x8000 else (c<<1)&0xFFFF
    return c
def build(t,seq):
    h=bytes([0xAA,0x55,t,seq&0xFF,(seq>>8)&0xFF,0,0]); cc=crc16(h[2:]); return h+bytes([cc&0xFF,(cc>>8)&0xFF])
fd=os.open('/dev/ttyRPMSG0',os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK); tty.setraw(fd)
buf=bytearray(); hb=0; st=0; t0=time.time()
while time.time()-t0<6:
    r,_,_=select.select([fd],[],[],0.2)
    if fd in r:
        buf+=os.read(fd,512)
        while True:
            i=buf.find(b'\xaa\x55')
            if i<0: buf=bytearray(); break
            if i>0: del buf[:i]
            if len(buf)<7: break
            pl=buf[5]|(buf[6]<<8); tot=7+pl+2
            if len(buf)<tot: break
            if crc16(bytes(buf[2:tot-2]))==(buf[tot-2]|(buf[tot-1]<<8)):
                if buf[2]==0x7E: hb+=1
                if buf[2]==0x23: st+=1
                del buf[:tot]
            else: del buf[:1]
print('OK: hb=%d m4state=%d'%(hb,st))
os.close(fd)
```

---

## 6. 参考
- 权威帧协议: `docs/protocols.md` §3；母本 `PhaseMd/10_协议规格总表`
- CubeMX bringup 详细步骤: `m4_fw/rpmsg.md`
- 100ASK 官方例程（与本板内核配套）: `E:\download\100ASK-MP157\100ASK_STM32MP157_M4_Code\22_A7_M4_UserModeComm\rpmsg_user\`（M4 侧裸机，无 FreeRTOS；22=用户态 ttyRPMSG0）
- ST 官方 FreeRTOS+OpenAMP 例程: `C:\Users\iosran\STM32Cube\Repository\STM32Cube_FW_MP1_V1.7.0\Projects\STM32MP157C-DK2\Applications\OpenAMP\OpenAMP_FreeRTOS_echo\`（端点在 osKernelStart 前创建，与我们的任务内延时重试等效）
