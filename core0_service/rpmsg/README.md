# core0_service/rpmsg — Linux(Core0) ↔ M4 RPMSG 通讯模块（第3步）

> 对端：M4（OpenAMP/RPMSG 从端，FreeRTOS，工程在 `m4_fw/`）。协议权威定义：`docs/protocols.md` §3（母本 `PhaseMd/10` §2）。本模块把 A7 侧任务 P3-06~P3-11 落成代码，**待板端编译联调**；M4 侧网关 `m4_fw/CM4/Core/{Inc,Src}/rpmsg_bridge.{c,h}`（Rpmsg_Task）代码就绪 2026-09-10，用户 CubeMX 勾 OPENAMP Regenerate 后即可编译联调（流程/验收见 `m4_fw/rpmsg.md`）；`tools/load_m4.sh` 已内置 start 前幂等释放 FDCAN2（can0 down + unbind 4400f000.can）。

## 文件地图

| 文件 | 职责 | 对应任务 |
|---|---|---|
| `rpmsg_types.h` | 协议常量/结构（type、payload 上限、上行载荷布局） | — |
| `rpmsg_proto.h/.c` | CRC16(XMODEM)、组帧、增量拆帧状态机（粘包/坏帧重同步/seq 丢帧统计）、payload 解码 | P3-07 |
| `rpmsg_link.h/.c` | 通道管理层：打开 raw、RX 线程 poll、双向心跳 0x7E、1s 断链判定、自动重开 + 0x13 重同步、线程安全发送 | P3-06/08/09/10 |
| `rpmsg_demo.c` | 演示/联调主程序（打印链路与上行帧，o/c/q 发指令） | P3-06~P3-10 验收入口 |
| `tools/rpmsg_cli/` | 打桩 CLI：hex 收发 / 坏 CRC 注入 / 统计（独立验证 M4） | P3-11 |
| `tools/load_m4.sh` | remoteproc start/stop/status + 等 `/dev/ttyRPMSG0` | P3-05 |
| `Makefile`（上一级） | 编译 demo 与 cli | — |

## 协议速记（详细见 docs/protocols.md §3）

- 帧：`AA 55 | type | seq(2B LE) | len(2B LE) | payload | CRC16(2B LE over type..payload)`；payload ≤ 480B。
- 下行 0x11 开闸 / 0x12 关闸 / 0x13 查询（空 payload）；0x14 配置预留。
- 上行 0x21 CAN 事件（17B）、0x22 节点离线/恢复（1B）、0x23 M4 全量状态（4B）、0x7E 心跳（1B 序号）。
- 心跳双向 500ms；1s 无任何有效帧 → LINK_DOWN；重连成功后自动发 0x13 全量重同步。

## 板端编译与运行

```sh
# 0) 先加载 M4（.elf 已放 /lib/firmware）；首次使用先加执行位
chmod +x tools/load_m4.sh tools/rpmsg_cli/rpmsg_cli 2>/dev/null || true
sudo ./tools/load_m4.sh start          # 停旧→start→等到 /dev/ttyRPMSG0

# 1) 编译（板端原生 gcc，或 ST SDK 交叉）
make                                   # or: make CROSS_COMPILE=arm-openstlinux-linux-gnueabihf-

# 2) 通道 demo（收 M4 心跳/事件，o=开闸 c=关闸 q=查询）
sudo ./rpmsg_demo

# 3) 打桩 CLI（独立验证 M4 行为）
sudo ./tools/rpmsg_cli/rpmsg_cli       # tx 11 / tx 13 / bad 21 ... / stats
```

## 断链/恢复自测（无 M4 时的本地法）

1. `sudo ./rpmsg_demo` 挂着；
2. 另一终端 `sudo sh -c 'echo stop > /sys/class/remoteproc/remoteproc0/state'`；
3. demo 应 **≤1s** 打 LINK DOWN（KPI），随后按 reopen_ms 周期重试；
4. `echo start > .../state` 恢复后自动 0x13 重同步、状态回 UP。

## 待办（板端）

- [ ] 编译冒烟：`make host-check`（PC/Linux 也可跑，只查语法）
- [ ] 与 M4 联调：心跳、0x21 事件、0x11/0x13 下行、断链 ≤1s、坏帧注入计数（G3）
- [ ] 第4步接入：业务在 `on_frame`/`on_link` 回调里消费（业务不碰 fd）
