/*
 * business.h - core0 business daemon state machine (P4-01..P4-06).
 *
 * 7 states (spec 5.1.1):
 *   IDLE -> CAR_WAIT -> RECOGNIZING -> WHITELIST_CHECK
 *        -> GATE_OPEN -> COOL_DOWN -> IDLE
 *        -> DENY -> COOL_DOWN (downgrade paths)
 *
 * Design rules:
 *  - Single-threaded: all entry points (biz_post / biz_periodic /
 *    biz_apply_config) must be called from one owner thread (the main
 *    epoll loop). Platform callbacks are invoked from that thread.
 *  - Pure logic: no OS calls beyond <time.h>; the clock is injected
 *    (now_ms arguments) and hardware access is injected via
 *    biz_platform_t. This makes the state machine host-testable
 *    (tools/core0_selftest.c).
 *  - Trigger uniqueness: only the node "car arrive" event (RPMSG 0x21 with
 *    EVT_CAR_ARRIVE) enters the recognition flow; remote/UI gate requests
 *    take the independent downgrade channel and never touch the state
 *    machine (spec 5.1.1.4).
 *  - Gate command idempotency: repeated commands for the same target
 *    within the cooldown window or when the observed gate already
 *    matches are skipped (spec 4.2.3 / 5.5.3.3).
 *
 * Pure ASCII / portable (also built by the host selftest).
 */
#ifndef BIZ_BUSINESS_H
#define BIZ_BUSINESS_H

#include <stdint.h>

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BIZ_ST_IDLE = 0,
    BIZ_ST_CAR_WAIT,
    BIZ_ST_RECOGNIZING,
    BIZ_ST_WHITELIST_CHECK,
    BIZ_ST_GATE_OPEN,
    BIZ_ST_DENY,
    BIZ_ST_COOL_DOWN,
    BIZ_ST_COUNT
} biz_state_t;

const char *biz_state_name(biz_state_t s);

typedef enum {
    BIZ_EV_CAR_ARRIVE = 0,     /* 0x21 EVT_CAR_ARRIVE (vehicle detected)    */
    BIZ_EV_CAR_LEAVE,          /* 0x21 EVT_CAR_LEAVE (detection cleared)    */
    BIZ_EV_GATE_STATE,         /* 0x21 EVT_GATE_STATE: b0 = 0 closed / 1 open */
    BIZ_EV_NODE_STATE,         /* 0x22: b0 = 0 offline / 1 online          */
    BIZ_EV_M4_STATE,           /* 0x23: b0 gate b1 node_online u16 can_err */
    BIZ_EV_LINK,               /* rpmsg link: b0 = 0 down / 1 up           */
    BIZ_EV_RECOG_RESULT,       /* result from core1 shm                    */
    BIZ_EV_REQ_GATE_OPEN,      /* remote open pulse from core1 (ui)        */
    BIZ_EV_REQ_GATE_CLOSE      /* remote close pulse from core1 (ui)       */
} biz_ev_type_t;

typedef struct {
    biz_ev_type_t type;
    uint8_t  b0, b1;           /* generic small args                        */
    uint16_t u16;              /* can_err counter for BIZ_EV_M4_STATE       */
    char     plate[16];
    float    conf;
    uint8_t  source;           /* 0=edge 1=cloud                            */
} biz_event_t;

/* values core0 publishes to core1 via /park_shm (spec 5.5.1.6) */
typedef struct {
    int32_t  free_slots;
    int32_t  used_slots;
    uint8_t  gate_state;
    uint8_t  link_flags;       /* bit0 rpmsg bit1 m4-online bit2 core1-online */
    uint8_t  recog_pending;
    uint8_t  fault_bits;       /* fault word, spec 6.4                        */
    float    conf_threshold;
    char     plate[16];
    float    confidence;
    uint8_t  result_source;
} biz_public_t;

/* platform callbacks, all called from the owner thread */
typedef struct {
    void *ctx;
    int  (*gate_open)(void *ctx);      /* send RPMSG 0x11, 0 = ok  */
    int  (*gate_close)(void *ctx);     /* send RPMSG 0x12, 0 = ok  */
    int  (*query_state)(void *ctx);    /* send RPMSG 0x13, 0 = ok  */
    /* eventfd 0x01 trigger / 0x03 state-change / 0x04 resync.
     * Placeholder in P4 when no fd is wired yet (spec 5.1.1.5). */
    void (*notify_core1)(void *ctx, uint8_t evt_code);
    void (*publish)(void *ctx, const biz_public_t *pub);
} biz_platform_t;

typedef struct biz biz_t;

biz_t *biz_create(const app_config_t *cfg, const biz_platform_t *plat);
void   biz_destroy(biz_t *b);

/* swap in a freshly loaded config (hot reload): threshold is
 * re-published so core1 reads the new value at once (spec 5.5.1.9) */
void   biz_apply_config(biz_t *b, const app_config_t *cfg);

void   biz_post(biz_t *b, const biz_event_t *ev, int64_t now_ms);

/* inputs polled from shm by the owner thread */
typedef struct {
    uint32_t hb_core1;
    uint8_t  cloud_pending;
} biz_poll_in_t;

/* periodic housekeeping: core1 heartbeat check (3s), recognition
 * timeout (3s / 6s with cloud fallback), cooldown expiry */
void   biz_periodic(biz_t *b, int64_t now_ms, const biz_poll_in_t *in);

biz_state_t biz_state(const biz_t *b);
void   biz_public(const biz_t *b, biz_public_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BIZ_BUSINESS_H */
