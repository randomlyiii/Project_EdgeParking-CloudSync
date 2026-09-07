/*
 * rpmsg_link.h — Linux(Core0) 侧 RPMSG 通道管理层
 *
 * 对应 PhaseMd/04 P3-06/08/09/10：
 *   P3-06 通道初始化：open("/dev/ttyRPMSG0", O_RDWR|O_NOCTTY|O_NONBLOCK) + termios raw
 *   P3-08 双向心跳：每 500ms 发 0x7E；1s 无任何有效帧 → LINK_DOWN（可配）
 *   P3-09 断链恢复：自动重开 + 清 RX 缓冲 + 发 0x13 全量查询重同步
 *   P3-10 业务对接：只通过回调拿帧（业务不碰 fd）；线程安全发送 API
 *
 * 线程模型：单 RX 线程 poll(fd)；写侧带锁，可被任意业务线程调用。
 */
#ifndef RPMSG_LINK_H
#define RPMSG_LINK_H

#include <stdint.h>
#include <stddef.h>
#include "rpmsg_types.h"
#include "rpmsg_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RPMSG_LINK_DOWN = 0,
    RPMSG_LINK_UP   = 1
} rpmsg_link_state_t;

/* 链路事件回调 */
typedef void (*rpmsg_link_event_cb)(rpmsg_link_state_t state, void *opaque);

/* 有效帧回调（在 RX 线程内调用；业务侧请仅做拷贝入队，勿阻塞） */
typedef void (*rpmsg_link_frame_cb)(const rpmsg_frame_t *f, void *opaque);

typedef struct {
    const char          *device;          /* 默认 "/dev/ttyRPMSG0" */
    rpmsg_link_event_cb  on_link;         /* 链路 UP/DOWN 通知（可空） */
    rpmsg_link_frame_cb  on_frame;        /* 每个 CRC 通过帧（可空） */
    void                *opaque;
    uint32_t             hb_interval_ms;  /* 心跳周期，默认 500ms */
    uint32_t             timeout_ms;      /* 无有效帧判 DOWN，默认 1000ms */
    uint32_t             reopen_ms;       /* DOWN 后重开间隔，默认 1000ms */
} rpmsg_link_cfg_t;

typedef struct rpmsg_link rpmsg_link_t;

/* 创建（不启动） */
rpmsg_link_t *rpmsg_link_create(const rpmsg_link_cfg_t *cfg);

/* 启动（后台 RX 线程开始尝试打开设备；打开前返回 0，链路事件经 on_link 通知） */
int rpmsg_link_start(rpmsg_link_t *lk);

/* 停止并释放（内部 join 线程） */
void rpmsg_link_stop(rpmsg_link_t *lk);
void rpmsg_link_free(rpmsg_link_t *lk);

rpmsg_link_state_t rpmsg_link_get_state(const rpmsg_link_t *lk);

/* ---- 发送（线程安全；type/seq 由本模块自动计数） ---- */
int rpmsg_link_send(rpmsg_link_t *lk, uint8_t type,
                    const uint8_t *payload, uint16_t plen);
/* 业务便捷封装 */
int rpmsg_link_send_gate_open(rpmsg_link_t *lk);   /* 0x11 */
int rpmsg_link_send_gate_close(rpmsg_link_t *lk);  /* 0x12 */
int rpmsg_link_send_query(rpmsg_link_t *lk);       /* 0x13 */

/* ---- 统计/快照（读侧线程安全：取快照拷贝） ---- */
uint64_t rpmsg_link_get_last_rx_ms(const rpmsg_link_t *lk);
void     rpmsg_link_get_stats(const rpmsg_link_t *lk, rpmsg_rx_stats_t *out);

/* 最近一次 0x23 携带的 M4 状态快照（0 表示尚未收到） */
int rpmsg_link_get_m4_state(const rpmsg_link_t *lk, rpmsg_m4_state_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RPMSG_LINK_H */
