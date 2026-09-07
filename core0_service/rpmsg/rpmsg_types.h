/*
 * rpmsg_types.h — A7-Core0 ↔ M4 RPMSG 协议常量与结构（第3步）
 *
 * 协议权威定义：docs/protocols.md §3（草案母本 PhaseMd/10 §2）
 * 帧：| 帧头 0xAA 0x55 | type(1B) | seq(2B LE) | len(2B LE) | payload | CRC16(2B LE, type..payload) |
 *
 * 本文件只含不依赖 Linux 的纯协议定义，方便跨端（M4 侧将来可直接参考）。
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
#define RPMSG_RX_CAN_EVENT    0x21u   /* CAN 事件转发，17B：id(4B LE)+dlc(1B)+data(8B)+tick(4B LE) */
#define RPMSG_RX_NODE_STATE   0x22u   /* 从节点离线/恢复：1B（0=离线 1=恢复） */
#define RPMSG_RX_M4_STATE     0x23u   /* M4 全量状态：4B = 闸(1B)+在线(1B)+CAN错误计数 u16 LE */
#define RPMSG_RX_HEARTBEAT    0x7Eu   /* 双向心跳：1B 序号 */

/* ---- 上行 payload 布局（偏移从 0 起） ---- */
#define RPMSG_CAN_EVT_LEN     17u
#define RPMSG_M4_STATE_LEN    4u
#define RPMSG_NODE_STATE_LEN  1u
#define RPMSG_HB_LEN          1u

/* 0x21 CAN 事件转发（与 docs/protocols.md §3 同步） */
typedef struct {
    uint32_t can_id;                  /* CAN ID（标准帧 11bit 装入 u32），LE */
    uint8_t  dlc;                     /* 数据长度 */
    uint8_t  data[8];                 /* 8B 数据域（未用 0 填充），字节原样搬运 */
    uint32_t tick;                    /* M4 本地毫秒，LE */
} rpmsg_can_event_t;

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
