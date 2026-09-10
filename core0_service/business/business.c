/*
 * business.c - core0 business daemon state machine implementation.
 *
 * Every state transition logs one line (source state, target state,
 * reason) so a full run can be replayed from the log (spec 5.1.1.6,
 * P4-01 acceptance).
 */
#include "business.h"
#include "log.h"
#include "store.h"
#include "whitelist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HB_CHECK_PERIOD_MS   3000u   /* spec 5.6.1.1 */
#define HB_LOST_AFTER_MS     3000u   /* no change for 3s -> lost */

const char *biz_state_name(biz_state_t s)
{
    switch (s) {
    case BIZ_ST_IDLE:            return "IDLE";
    case BIZ_ST_CAR_WAIT:        return "CAR_WAIT";
    case BIZ_ST_RECOGNIZING:     return "RECOGNIZING";
    case BIZ_ST_WHITELIST_CHECK: return "WHITELIST_CHECK";
    case BIZ_ST_GATE_OPEN:       return "GATE_OPEN";
    case BIZ_ST_DENY:            return "DENY";
    case BIZ_ST_COOL_DOWN:       return "COOL_DOWN";
    default:                     return "?";
    }
}

struct biz {
    const app_config_t *cfg;
    biz_platform_t      plat;

    biz_state_t st;
    int64_t     now;

    /* recognition */
    int64_t     recog_start;
    int64_t     recog_deadline;
    int         cloud_extended;
    uint8_t     recog_pending;
    char        last_plate[16];
    float       last_conf;
    uint8_t     last_source;

    /* gate */
    uint8_t     gate_state;        /* observed, from M4 0x23 */
    uint8_t     last_cmd;          /* 0 none, 1 open, 2 close */
    int64_t     last_cmd_ms;

    /* slots / passage */
    int32_t     used;
    uint8_t     shading;
    uint8_t     pass_pending;

    /* health / fault bits */
    uint8_t     link_up;
    uint8_t     node_online;
    uint8_t     core1_alive;
    uint16_t    can_err;
    uint32_t    hb_last;
    int64_t     hb_seen_ms;
    int64_t     hb_last_check_ms;
    int         hb_check_armed;
    uint8_t     cloud_pending;

    /* cooldown */
    int64_t     cool_deadline;

    /* publish dedup */
    biz_public_t pub;
    int          pub_valid;
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static int32_t today_yyyymmdd(int64_t now_ms)
{
    time_t sec = (time_t)(now_ms / 1000);
    struct tm tmv;
#if defined(_WIN32)
    localtime_s(&tmv, &sec);
#else
    localtime_r(&sec, &tmv);
#endif
    return (int32_t)((tmv.tm_year + 1900) * 10000 +
                     (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
}

static void to_state(biz_t *b, biz_state_t to, const char *reason)
{
    if (b->st == to)
        return;
    LOGI("state", "%s -> %s (%s)", biz_state_name(b->st),
         biz_state_name(to), reason);
    b->st = to;
}

static uint8_t fault_bits(const biz_t *b)
{
    uint8_t f = 0;
    if (!b->link_up)         f |= (1u << 0);   /* rpmsg down        */
    if (!b->node_online)     f |= (1u << 1);   /* node (C8T6) down  */
    if (!b->core1_alive)     f |= (1u << 2);   /* core1 lost        */
    if (b->can_err != 0)     f |= (1u << 3);   /* CAN errors seen   */
    /* bit4 cloud fallback failed: reserved, step 7 */
    return f;
}

static uint8_t link_flags(const biz_t *b)
{
    return (uint8_t)((b->link_up ? (1u << 0) : 0u) |
                     (b->node_online ? (1u << 1) : 0u) |
                     (b->core1_alive ? (1u << 2) : 0u));
}

static int pub_equal(const biz_public_t *a, const biz_public_t *b)
{
    return a->free_slots == b->free_slots &&
           a->used_slots == b->used_slots &&
           a->gate_state == b->gate_state &&
           a->link_flags == b->link_flags &&
           a->recog_pending == b->recog_pending &&
           a->fault_bits == b->fault_bits &&
           a->conf_threshold == b->conf_threshold &&
           strcmp(a->plate, b->plate) == 0 &&
           a->confidence == b->confidence &&
           a->result_source == b->result_source;
}

static void refresh_public(biz_t *b)
{
    biz_public_t p;

    p.free_slots = b->cfg->total_slots - b->used;
    if (p.free_slots < 0) p.free_slots = 0;               /* clamp [0,total] */
    if (p.free_slots > b->cfg->total_slots) p.free_slots = b->cfg->total_slots;
    p.used_slots = b->used;
    p.gate_state = b->gate_state;
    p.link_flags = link_flags(b);
    p.recog_pending = b->recog_pending;
    p.fault_bits = fault_bits(b);
    p.conf_threshold = b->cfg->conf_threshold;
    memcpy(p.plate, b->last_plate, sizeof(p.plate));
    p.confidence = b->last_conf;
    p.result_source = b->last_source;

    if (b->pub_valid && pub_equal(&p, &b->pub))
        return;

    b->pub = p;
    b->pub_valid = 1;
    if (b->plat.publish != NULL)
        b->plat.publish(b->plat.ctx, &p);
    if (b->plat.notify_core1 != NULL)
        b->plat.notify_core1(b->plat.ctx, 0x03);   /* state change */
}

/* enter DENY (with reason) then COOL_DOWN; recog_pending cleared */
static void enter_deny(biz_t *b, const char *reason)
{
    b->recog_pending = 0;
    to_state(b, BIZ_ST_DENY, reason);
    store_event("deny", b->last_plate, reason);
    to_state(b, BIZ_ST_COOL_DOWN, "after deny");
    b->cool_deadline = b->now + b->cfg->cool_down_ms;
    refresh_public(b);
}

/*
 * Gate command with idempotency (spec 4.2.3 / 5.5.3.3): skip when the
 * same action was sent inside the cooldown window or when the observed
 * gate already matches the target. Returns 0 when sent or skipped.
 */
static int gate_cmd(biz_t *b, int open, const char *source)
{
    uint8_t target = open ? 1u : 0u;
    uint8_t last_is_same = b->last_cmd == (open ? 1u : 2u);
    int rc;

    if (last_is_same &&
        b->now - b->last_cmd_ms < (int64_t)b->cfg->cool_down_ms) {
        LOGI("gate", "%s skipped: same command inside cooldown window",
             open ? "OPEN" : "CLOSE");
        return 0;
    }
    if (b->gate_state == target) {
        LOGI("gate", "%s skipped: gate already %s (observed)",
             open ? "OPEN" : "CLOSE", open ? "open" : "closed");
        return 0;
    }

    rc = open ? b->plat.gate_open(b->plat.ctx)
              : b->plat.gate_close(b->plat.ctx);
    if (rc != 0) {
        LOGE("gate", "%s command send failed (rpmsg down?)",
             open ? "OPEN 0x11" : "CLOSE 0x12");
        return -1;
    }
    b->last_cmd = open ? 1u : 2u;
    b->last_cmd_ms = b->now;
    LOGI("gate", "%s command sent (source=%s)",
         open ? "OPEN 0x11" : "CLOSE 0x12", source);
    store_gate(open ? "open" : "close", source);
    if (open)
        b->pass_pending = 1;   /* a passage may follow, counted on clear */
    return 0;
}

/* ------------------------------------------------------------------ */
/* event handlers                                                      */

static void on_shade_arrive(biz_t *b)
{
    if (b->shading) {
        LOGI("shade", "duplicate arrive while already shaded, ignored");
        return;
    }
    b->shading = 1;
    LOGI("shade", "car arrival observed (CAN 0x200 d0=0x01)");

    if (b->st != BIZ_ST_IDLE) {
        /* spec 5.1.3.2: no double registration / double trigger */
        LOGW("shade", "busy in %s: duplicate registration ignored",
             biz_state_name(b->st));
        return;
    }

    to_state(b, BIZ_ST_CAR_WAIT, "shade event 0x200 d0=0x01");
    store_event("car_arrive", "", "");

    if (!b->core1_alive) {
        /* spec 5.2.1.5: core1 lost -> straight to downgrade path */
        LOGW("biz", "core1 offline at registration: skip recognition");
        enter_deny(b, "core1 lost, downgrade path");
        return;
    }

    b->recog_start = b->now;
    b->recog_deadline = b->now + b->cfg->recog_timeout_ms;
    b->cloud_extended = 0;
    b->recog_pending = 1;
    refresh_public(b);          /* recog_pending=1 visible to core1 */
    /* eventfd 0x01; no-op placeholder until the IPC fd is wired (P6) */
    if (b->plat.notify_core1 != NULL)
        b->plat.notify_core1(b->plat.ctx, 0x01);
    to_state(b, BIZ_ST_RECOGNIZING, "recog_pending=1, trigger 0x01 sent");
}

static void on_shade_clear(biz_t *b)
{
    if (!b->shading) {
        LOGD("shade", "clear without prior arrival (query reply?), ignored");
        return;
    }
    b->shading = 0;
    LOGI("shade", "shading recovered (CAN 0x200 d0=0x00)");

    if (b->pass_pending) {
        int32_t used;
        b->pass_pending = 0;
        used = b->used + (b->cfg->count_mode == 0 ? 1 : -1);
        /* demo counting per spec 5.4.3: open + shade recover = passage;
         * re-driven cars may bias the count, accepted by design */
        if (used < 0) used = 0;
        if (used > b->cfg->total_slots) used = b->cfg->total_slots;
        if (used != b->used) {
            b->used = used;
            LOGI("slots", "passage counted (%s mode): used=%d free=%d",
                 b->cfg->count_mode == 0 ? "entry" : "exit",
                 b->used, b->cfg->total_slots - b->used);
            store_event("passage", b->last_plate,
                        b->cfg->count_mode == 0 ? "entry +1" : "exit -1");
        } else {
            LOGW("slots", "passage clamped at limit: used=%d", b->used);
        }
        refresh_public(b);
    }
}

static void on_recog_result(biz_t *b, const biz_event_t *ev)
{
    wl_match_t m;

    /* remember the latest result even when late (UI shows last plate) */
    snprintf(b->last_plate, sizeof(b->last_plate), "%s", ev->plate);
    b->last_conf = ev->conf;
    b->last_source = ev->source;

    if (b->st != BIZ_ST_RECOGNIZING) {
        /* spec 5.2.3.2: late / repeated result after timeout is dropped */
        LOGW("recog", "late result '%s' in %s: dropped (idempotent)",
             ev->plate, biz_state_name(b->st));
        return;
    }

    b->recog_pending = 0;
    to_state(b, BIZ_ST_WHITELIST_CHECK, "result arrived");
    store_event("recog", ev->plate, ev->source ? "source=cloud" : "source=edge");
    refresh_public(b);          /* last plate/conf/source visible to core1 */

    m = wl_match(&b->cfg->wl, ev->plate, today_yyyymmdd(b->now));
    if (m == WL_MATCH_OK) {
        to_state(b, BIZ_ST_GATE_OPEN, "whitelist hit");
        gate_cmd(b, 1, "auto");
        to_state(b, BIZ_ST_COOL_DOWN, "after gate open");
        b->cool_deadline = b->now + b->cfg->cool_down_ms;
    } else {
        /* spec 5.3.1.4: DENY keeps plate + reason for audit */
        LOGW("whitelist", "plate '%s' rejected: %s", ev->plate,
             wl_match_str(m));
        enter_deny(b, wl_match_str(m));
    }
}

static void on_remote_req(biz_t *b, int open)
{
    /* downgrade channel: independent of the state machine and of the
     * cooldown (spec 5.5.3.3), dedup handled inside gate_cmd */
    LOGI("remote", "gate %s request from core1 (ui)",
         open ? "OPEN" : "CLOSE");
    gate_cmd(b, open, "ui");
}

static void on_m4_state(biz_t *b, const biz_event_t *ev)
{
    uint8_t old_gate = b->gate_state;
    uint8_t old_node = b->node_online;

    b->gate_state  = ev->b0 ? 1u : 0u;
    b->node_online = ev->b1 ? 1u : 0u;
    b->can_err     = ev->u16;

    if (old_gate != b->gate_state) {
        LOGI("m4", "gate state now %s (M4 report)",
             b->gate_state ? "OPEN" : "CLOSED");
        store_gate(b->gate_state ? "open" : "close", "state");
    }
    if (old_node != b->node_online) {
        LOGI(b->node_online ? "m4" : "watchdog",
             b->node_online ? "CAN node (C8T6) back online"
                            : "CAN node (C8T6) offline");
    }
    if (b->can_err != 0)
        LOGW("m4", "M4 reports CAN error counter = %u", (unsigned)b->can_err);
    refresh_public(b);
}

static void on_link(biz_t *b, const biz_event_t *ev)
{
    int up = ev->b0 ? 1 : 0;
    if (up == (int)b->link_up)
        return;
    b->link_up = (uint8_t)up;
    LOGI("rpmsg", "link %s", up ? "UP" : "DOWN");
    if (up && b->plat.query_state != NULL) {
        /* resync: ask M4 for full state so gate_state gets corrected */
        b->plat.query_state(b->plat.ctx);
    }
    refresh_public(b);
}

static void on_node_state(biz_t *b, const biz_event_t *ev)
{
    b->node_online = ev->b0 ? 1u : 0u;
    refresh_public(b);
}

/* ------------------------------------------------------------------ */
/* public API                                                          */

biz_t *biz_create(const app_config_t *cfg, const biz_platform_t *plat)
{
    biz_t *b;

    if (cfg == NULL || plat == NULL)
        return NULL;
    b = (biz_t *)calloc(1, sizeof(*b));
    if (b == NULL)
        return NULL;
    b->cfg = cfg;
    b->plat = *plat;
    b->st = BIZ_ST_IDLE;
    b->used = 0;
    LOGI("biz", "business daemon created: state=IDLE slots=%d wl=%d "
         "recog_timeout=%dms cool_down=%dms threshold=%.2f",
         cfg->total_slots, wl_count(&cfg->wl), cfg->recog_timeout_ms,
         cfg->cool_down_ms, cfg->conf_threshold);
    refresh_public(b);          /* initial publish */
    return b;
}

void biz_destroy(biz_t *b)
{
    free(b);
}

void biz_apply_config(biz_t *b, const app_config_t *cfg)
{
    if (b == NULL || cfg == NULL)
        return;
    b->cfg = cfg;
    LOGI("config", "config applied: slots=%d wl=%d recog_timeout=%dms "
         "cloud_timeout=%dms cool_down=%dms threshold=%.2f",
         cfg->total_slots, wl_count(&cfg->wl), cfg->recog_timeout_ms,
         cfg->cloud_timeout_ms, cfg->cool_down_ms, cfg->conf_threshold);
    refresh_public(b);          /* re-publish (threshold may have changed) */
}

void biz_post(biz_t *b, const biz_event_t *ev, int64_t now_ms)
{
    if (b == NULL || ev == NULL)
        return;
    b->now = now_ms;

    switch (ev->type) {
    case BIZ_EV_SHADE_ARRIVE:  on_shade_arrive(b); break;
    case BIZ_EV_SHADE_CLEAR:   on_shade_clear(b); break;
    case BIZ_EV_NODE_STATE:    on_node_state(b, ev); break;
    case BIZ_EV_M4_STATE:      on_m4_state(b, ev); break;
    case BIZ_EV_LINK:          on_link(b, ev); break;
    case BIZ_EV_RECOG_RESULT:  on_recog_result(b, ev); break;
    case BIZ_EV_REQ_GATE_OPEN: on_remote_req(b, 1); break;
    case BIZ_EV_REQ_GATE_CLOSE:on_remote_req(b, 0); break;
    default:
        LOGW("biz", "unknown event type %d ignored", (int)ev->type);
        break;
    }
}

void biz_periodic(biz_t *b, int64_t now_ms, const biz_poll_in_t *in)
{
    if (b == NULL)
        return;
    b->now = now_ms;
    b->cloud_pending = (in != NULL) ? (in->cloud_pending ? 1u : 0u) : 0u;

    /* ---- core1 heartbeat watchdog (spec 5.6) ---- */
    if (!b->hb_check_armed) {
        b->hb_check_armed = 1;
        b->hb_last_check_ms = now_ms;
        b->hb_seen_ms = now_ms;
        b->hb_last = (in != NULL) ? in->hb_core1 : 0u;
        /* nonzero at arm time means core1 was already heartbeating */
        if (b->hb_last != 0u) {
            b->core1_alive = 1;
            LOGI("watchdog", "core1 heartbeat already active (hb=%u)",
                 (unsigned)b->hb_last);
            refresh_public(b);
        }
    } else if (now_ms - b->hb_last_check_ms >= (int64_t)HB_CHECK_PERIOD_MS) {
        b->hb_last_check_ms = now_ms;
        if (in != NULL && in->hb_core1 != b->hb_last) {
            b->hb_last = in->hb_core1;
            b->hb_seen_ms = now_ms;
            if (!b->core1_alive) {
                b->core1_alive = 1;
                LOGI("watchdog", "core1 heartbeat alive again");
                refresh_public(b);
            }
        } else if (now_ms - b->hb_seen_ms >= (int64_t)HB_LOST_AFTER_MS) {
            if (b->core1_alive) {
                b->core1_alive = 0;
                LOGW("watchdog", "core1 heartbeat lost >=3s: fault bit2 set");
                refresh_public(b);
                if (b->st == BIZ_ST_RECOGNIZING) {
                    /* spec 5.2.1.5: stop waiting, go downgrade path */
                    LOGW("biz", "core1 lost while recognizing: skip wait");
                    enter_deny(b, "core1 lost while recognizing");
                }
            }
        }
    }

    /* ---- recognition timeout (spec 5.2) ---- */
    if (b->st == BIZ_ST_RECOGNIZING && b->now >= b->recog_deadline) {
        if (b->cloud_pending && !b->cloud_extended) {
            /* cloud fallback in progress: extend to the 6s hard cap */
            b->cloud_extended = 1;
            b->recog_deadline = b->recog_start + b->cfg->cloud_timeout_ms;
            LOGI("recog", "cloud fallback pending: timeout extended to %dms",
                 b->cfg->cloud_timeout_ms);
        } else {
            LOGW("recog", "recognition timeout: downgrade path (ui open "
                 "stays available)");
            enter_deny(b, "recognition timeout");
        }
    }

    /* ---- cooldown expiry ---- */
    if (b->st == BIZ_ST_COOL_DOWN && b->now >= b->cool_deadline)
        to_state(b, BIZ_ST_IDLE, "cooldown elapsed");
}

biz_state_t biz_state(const biz_t *b)
{
    return (b != NULL) ? b->st : BIZ_ST_IDLE;
}

void biz_public(const biz_t *b, biz_public_t *out)
{
    if (b != NULL && out != NULL)
        *out = b->pub;
}
