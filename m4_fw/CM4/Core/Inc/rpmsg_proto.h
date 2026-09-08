/*
 * rpmsg_proto.h — RPMSG 帧编解码层（纯协议，无 I/O）[M4 副本]
 *
 * 对应 PhaseMd/04 P3-07；帧格式权威定义 docs/protocols.md §0/§3。
 * 职责：CRC16(XMODEM) / 组帧 / 增量拆帧状态机（粘包、坏帧重同步、丢帧统计）。
 * 本层不碰 fd、不感知链路状态，可单独用 rpmsg_cli/单测验证。
 *
 * ⭐ 本文件 = core0_service/rpmsg/rpmsg_proto.h 的完整拷贝副本（M4 侧
 *    rpmsg_decode_* 为死代码但保留，保证两端逐字节可 diff；单一事实源在
 *    A7 侧，改动需走协议同步流程：母本 §3 → A7 → 本副本 → 两处变更记录）。
 */
#ifndef RPMSG_PROTO_H
#define RPMSG_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include "rpmsg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- CRC16（XMODEM：poly 0x1021, init 0, 不反射, 无异或） ---------- */
uint16_t rpmsg_crc16(const uint8_t *data, size_t len);

/* ---------- 组帧（TX） ---------- */
/*
 * 把 type/seq/payload 组为一帧写入 out（容量 outcap）。
 * 成功返回整帧字节数；payload 超长/参数错返回 -1。
 */
int rpmsg_frame_build(uint8_t type, uint16_t seq,
                      const uint8_t *payload, uint16_t plen,
                      uint8_t *out, size_t outcap);

/* ---------- 拆帧统计 ---------- */
typedef struct {
    uint64_t frames_ok;      /* CRC 通过并交付的帧数 */
    uint64_t bad_crc;        /* CRC 失败帧数 */
    uint64_t bad_len;        /* len 超限帧数 */
    uint64_t seq_gaps;       /* seq 跳变累计缺口（丢帧估计） */
    uint64_t resyncs;        /* 帧头重同步次数（含垃圾字节清理） */
    uint16_t last_seq[256];  /* 每 type 通道最近 seq（0 表示未见） */
    uint8_t  seen[256];      /* 该 type 是否已见首帧 */
} rpmsg_rx_stats_t;

/* ---------- 增量拆帧（RX） ---------- */
typedef struct rpmsg_rx rpmsg_rx_t;

/* 解析器上下文：可在栈上声明后调用 rpmsg_rx_init；M4 侧勿放任务栈（buf 2048B） */
struct rpmsg_rx {
    uint8_t      buf[2048];           /* 累积缓冲（≥2×RPMSG_FRAME_MAX） */
    size_t       len;                 /* 有效字节数 */
    rpmsg_rx_stats_t stats;
};

void rpmsg_rx_init(rpmsg_rx_t *rx);               /* 清零缓冲+统计 */
void rpmsg_rx_reset(rpmsg_rx_t *rx);              /* 断链重连：清缓冲并作废旧 seq 基线（累计计数保留可留档） */

/* 帧交付回调：返回 0=继续，非 0=停止本次喂入（已解析帧数仍累计） */
typedef int (*rpmsg_frame_cb)(const rpmsg_frame_t *f, void *opaque);

/*
 * 喂入一段字节流，内部处理粘包/坏帧/seq 统计。
 * 每解析出一帧调用一次 cb。返回解析出的帧数（可为 0）。
 */
size_t rpmsg_rx_feed(rpmsg_rx_t *rx, const uint8_t *data, size_t n,
                     rpmsg_frame_cb cb, void *opaque);

/* 便捷：单帧解析（校验 CRC；ok=1 时 frame 被填充）。 */
int rpmsg_rx_parse_one(const uint8_t *frame, size_t n, rpmsg_frame_t *out);

/* ---------- 上行 payload 解码（字节序/布局见 docs/protocols.md §3） ---------- */
/* 0x21 → CAN 事件（成功返回 0） */
int rpmsg_decode_can_event(const rpmsg_frame_t *f, rpmsg_can_event_t *ev);
/* 0x23 → M4 全量状态（成功返回 0） */
int rpmsg_decode_m4_state(const rpmsg_frame_t *f, rpmsg_m4_state_t *st);
/* 0x22 → 节点离线/恢复：返回 0=离线 1=恢复；非 0x22/长度错返回 -1 */
int rpmsg_decode_node_state(const rpmsg_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif /* RPMSG_PROTO_H */
