/*
 * rpmsg_types.h — A7-Core0 ↔ M4 RPMSG 协议常量与结构（第3步）[M4 副本]
 *
 * 协议权威定义：docs/protocols.md §3（草案母本 PhaseMd/10 §2）
 * 帧：| 帧头 0xAA 0x55 | type(1B) | seq(2B LE) | len(2B LE) | payload | CRC16(2B LE, type..payload) |
 *
 * ⭐ 本文件 = core0_service/rpmsg/rpmsg_types.h 的 M4 逐字节拷贝副本
 *    （单一事实源在 A7 侧；两端必须一致，改协议先改母本 docs/protocols.md §3
 *    → 同步 A7 原件 → 同步本副本 → 两处变更记录）。
 *    M4 侧副本仅允许裁剪编译器差异，不得改协议字段。
 */
#ifndef RPMSG_TYPES_H
#define RPMSG_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* ---- 帧级常量（协议定版） ---- */
#define RPMSG_MAGIC0          0xAAu
#define RPMSG_MAGIC1          0x55u
#define RPMSG_PAYLOAD_MAX     480u    /* 单帧 payload 上限（总帧 ≤489B < ttyRPMSG0 MTU≈496） */
#define RPMSG_FRAME_MAX       (2u + 1u + 2u + 2u + RPMSG_PAYLOAD_MAX + 2u)  /* 完整帧缓冲上限 489 */

/* ---- type：下行 Core0→M4 ---- */
#define RPMSG_TX_GATE_OPEN    0x11u   /* 开闸，空 payload */
#define RPMSG_TX_GATE_CLOSE   0x12u   /* 关闸，空 payload */
#define RPMSG_TX_QUERY_STATE  0x13u   /* 查询全量状态，空 payload → M4 回 0x23 */
#define RPMSG_TX_CONFIG       0x14u   /* 配置下发（预留，TLV） */

/* ---- type：上行 M4→Core0 ---- */
#define RPMSG_RX_NODE_EVENT   0x21u   /* 节点语义事件 v2，9B（v1 的 17B CAN 原始透传已废弃） */
#define RPMSG_RX_NODE_STATE   0x22u   /* 从节点离线/恢复：1B（0=离线 1=恢复） */
#define RPMSG_RX_M4_STATE     0x23u   /* M4 全量状态：4B = 闸(1B)+在线(1B)+CAN错误计数 u16 LE */
#define RPMSG_RX_HEARTBEAT    0x7Eu   /* 双向心跳：1B 序号 */

/* ---- 上行 payload 布局（偏移从 0 起） ---- */
#define RPMSG_NODE_EVT_LEN    9u      /* code(1B)+arg(2B LE)+status(1B)+node_id(1B)+tick(4B LE) */
#define RPMSG_M4_STATE_LEN    4u
#define RPMSG_NODE_STATE_LEN  1u
#define RPMSG_HB_LEN          1u

/* ---- 节点事件码（与 CAN 0x200 d[0] 同一张表；权威表 docs/protocols.md §1） ---- */
#define EVT_CAR_ARRIVE        0x01u   /* arg=0：车/物体进入检测区 */
#define EVT_CAR_LEAVE         0x02u   /* arg=0：车/物体离开检测区 */
#define EVT_NODE_FAULT        0x03u   /* arg=故障类别：故障"发生"时上报一次 */
#define EVT_GATE_STATE        0x04u   /* arg=0 关 / 1 开（2=动作中，预留未用） */
#define EVT_NODE_READY        0x05u   /* arg=0：节点上电就绪 */

/* 故障类别（EVT_NODE_FAULT 的 arg） */
#define EVT_FAULT_SENSOR      1u
#define EVT_FAULT_ACTUATOR    2u
#define EVT_FAULT_CAN         3u

/* ---- 节点状态位（0x21 payload status；与 CAN 0x200 d[3] 同源） ---- */
#define NODE_STAT_GATE_OPEN    0x01u  /* bit0：执行器到位（闸开） */
#define NODE_STAT_PRESENCE     0x02u  /* bit1：检测区有车/物体 */
#define NODE_STAT_SENSOR_FAULT 0x04u  /* bit2：传感器子系统故障 */
#define NODE_STAT_CAN_ERR      0x08u  /* bit3：节点侧 CAN 发送异常 */

/* ---- 节点号（0x21 payload node_id） ---- */
#define RPMSG_NODE_ID_MAIN    0x01u   /* 主检测节点（当前 = C8T6 光感+舵机节点） */

/* 0x21 节点语义事件（v2）：
   ⭐ 只携带语义量 —— 不含 lux/drop% 等传感器原始值，
   板级因此对"下位机用什么传感器"零假设（换传感器不动 Linux 代码）。 */
typedef struct {
    uint8_t  code;                    /* 事件码 EVT_* */
    uint16_t arg;                     /* 语义参数，LE，含义由 code 决定 */
    uint8_t  status;                  /* 节点状态位快照 NODE_STAT_* */
    uint8_t  node_id;                 /* 节点号 RPMSG_NODE_ID_* */
    uint32_t tick;                    /* M4 接收时刻（osKernelGetTickCount，ms），LE */
} rpmsg_node_event_t;

/* 0x23 M4 全量状态 */
typedef struct {
    uint8_t  gate_state;              /* 0 关 / 1 开 */
    uint8_t  node_online;             /* 0 离线 / 1 在线 */
    uint16_t can_err_cnt;             /* CAN 错误累计计数，LE，饱和 */
} rpmsg_m4_state_t;

/* 一帧（解析后/待发送共用） */
typedef struct {
    uint8_t  type;
    uint16_t seq;                     /* 发送方按 type 各自单调计数 */
    uint16_t len;                     /* payload 长度 */
    uint8_t  payload[RPMSG_PAYLOAD_MAX];
} rpmsg_frame_t;

#endif /* RPMSG_TYPES_H */
