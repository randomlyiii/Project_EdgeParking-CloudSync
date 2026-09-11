/*
 * core0_selftest.c - host selftest for the core0 business state machine
 * (P4-01..P4-06 acceptance without hardware).
 *
 * Drives business/ with a virtual clock and a fake platform (gate
 * commands and notify events recorded), covering:
 *   S1  happy path: shade -> whitelisted result -> open -> cooldown ->
 *       passage count on shade clear (entry mode)
 *   S2  recognition timeout -> deny, late result dropped
 *   S3  cloud_pending extends 3s -> 6s hard cap
 *   S4  core1 dead at registration -> immediate downgrade, no trigger
 *   S5  whitelist reject (no entry), DENY keeps last plate
 *   S6  expiry + deny-permission entries
 *   S7  remote open/close dedup (idempotent, observed-state aware)
 *   S8  passage counted exactly once (no duplicate counting)
 *   S9  slots clamp at total (entry mode), exit mode counts -1
 *   S10 config parse + invalid value rejection + whitelist section
 *   S11 duplicate shade arrival ignored while busy
 *   S12 link down -> fault bit0, link_flags update
 *   S13 log level filter + file target, storage-off is a no-op (P4-07/P4-08)
 *
 * Build (any Linux/MinGW gcc):
 *   gcc -std=gnu11 -Ibusiness -Istorage -o core0_selftest \
 *       tools/core0_selftest.c business/business.c business/whitelist.c \
 *       business/app_config.c business/log.c storage/store.c -lpthread
 */
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "business.h"
#include "log.h"
#include "store.h"
#include "whitelist.h"

static int g_checks = 0, g_fails = 0;

#define CHECK(cond) do {                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fails++;                                                    \
            printf("FAIL line %d: %s\n", __LINE__, #cond);                \
        }                                                                 \
    } while (0)

/* ---- fake platform ---- */

typedef struct {
    int          gate_open_calls;
    int          gate_close_calls;
    int          query_calls;
    int          notify_01, notify_03, notify_04;
    biz_public_t pub;
    int          pub_cnt;
} fake_t;

static int fk_gate_open(void *ctx)
{
    ((fake_t *)ctx)->gate_open_calls++;
    return 0;
}

static int fk_gate_close(void *ctx)
{
    ((fake_t *)ctx)->gate_close_calls++;
    return 0;
}

static int fk_query(void *ctx)
{
    ((fake_t *)ctx)->query_calls++;
    return 0;
}

static void fk_notify(void *ctx, uint8_t code)
{
    fake_t *f = (fake_t *)ctx;
    if (code == 0x01) f->notify_01++;
    else if (code == 0x03) f->notify_03++;
    else if (code == 0x04) f->notify_04++;
}

static void fk_publish(void *ctx, const biz_public_t *pub)
{
    fake_t *f = (fake_t *)ctx;
    f->pub = *pub;
    f->pub_cnt++;
}

static void fake_platform(biz_platform_t *p, fake_t *f)
{
    memset(f, 0, sizeof(*f));
    p->ctx = f;
    p->gate_open = fk_gate_open;
    p->gate_close = fk_gate_close;
    p->query_state = fk_query;
    p->notify_core1 = fk_notify;
    p->publish = fk_publish;
}

/*苏 = E8 8B 8F, 粤 = E7 B2 A4 (UTF-8, escaped to keep the source ASCII) */
#define PLATE_OK1  "\xE8\x8B\x8F""A12345"      /* SuA12345 */
#define PLATE_OK2  "\xE7\xB2\xA4""B88888"      /* YueB88888 */
#define PLATE_BAD  "B99999"

static void post(biz_t *b, fake_t *f, biz_ev_type_t t, int64_t now)
{
    biz_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = t;
    biz_post(b, &ev, now);
    (void)f;
}

static void post_result(biz_t *b, const char *plate, float conf, int src,
                        int64_t now)
{
    biz_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = BIZ_EV_RECOG_RESULT;
    snprintf(ev.plate, sizeof(ev.plate), "%s", plate);
    ev.conf = conf;
    ev.source = (uint8_t)src;
    biz_post(b, &ev, now);
}

static void post_link(biz_t *b, int up, int64_t now)
{
    biz_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = BIZ_EV_LINK;
    ev.b0 = (uint8_t)(up ? 1 : 0);
    biz_post(b, &ev, now);
}

static void post_m4(biz_t *b, int gate, int node, int err, int64_t now)
{
    biz_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = BIZ_EV_M4_STATE;
    ev.b0 = (uint8_t)gate;
    ev.b1 = (uint8_t)node;
    ev.u16 = (uint16_t)err;
    biz_post(b, &ev, now);
}

static void periodic(biz_t *b, fake_t *f, int64_t now, uint32_t hb,
                     int cloud)
{
    biz_poll_in_t pin;
    pin.hb_core1 = hb;
    pin.cloud_pending = (uint8_t)cloud;
    biz_periodic(b, now, &pin);
    (void)f;
}

/* realistic heartbeat: core1 increments once per second */
static void periodic_alive(biz_t *b, fake_t *f, int64_t now, int cloud)
{
    periodic(b, f, now, (uint32_t)(now / 1000), cloud);
}

/* config with one whitelisted plate */
static void make_cfg(app_config_t *c)
{
    app_config_defaults(c);
    wl_init(&c->wl);
    wl_add(&c->wl, PLATE_OK1, 1, 0);
}

/* ================================================================== */

static void s1_happy_path(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S1 happy path\n");
    make_cfg(&cfg);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    CHECK(b != NULL);

    periodic_alive(b, &fk, 100000, 0);            /* core1 alive (hb nonzero) */
    CHECK(fk.pub.link_flags & 0x04);

    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 100000);
    CHECK(biz_state(b) == BIZ_ST_RECOGNIZING);
    CHECK(fk.notify_01 == 1);
    CHECK(fk.pub.recog_pending == 1);

    post_result(b, PLATE_OK1, 0.95f, 0, 100100);
    CHECK(biz_state(b) == BIZ_ST_COOL_DOWN);
    CHECK(fk.gate_open_calls == 1);
    CHECK(fk.gate_close_calls == 0);
    CHECK(fk.pub.recog_pending == 0);
    CHECK(strcmp(fk.pub.plate, PLATE_OK1) == 0);

    periodic_alive(b, &fk, 104200, 0);            /* 4.2s later: cooldown done */
    CHECK(biz_state(b) == BIZ_ST_IDLE);

    post(b, &fk, BIZ_EV_SHADE_CLEAR, 104300);  /* passage completes */
    CHECK(fk.pub.used_slots == 1);
    CHECK(fk.pub.free_slots == 19);

    post(b, &fk, BIZ_EV_SHADE_CLEAR, 104400);  /* no edge: no double count */
    CHECK(fk.pub.used_slots == 1);

    biz_destroy(b);
}

static void s2_timeout(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S2 recognition timeout -> deny\n");
    make_cfg(&cfg);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic_alive(b, &fk, 200000, 0);

    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 200000);
    CHECK(biz_state(b) == BIZ_ST_RECOGNIZING);

    periodic_alive(b, &fk, 201000, 0);
    CHECK(biz_state(b) == BIZ_ST_RECOGNIZING);  /* before 3s deadline */

    periodic_alive(b, &fk, 203100, 0);             /* past 3s */
    CHECK(biz_state(b) == BIZ_ST_COOL_DOWN);
    CHECK(fk.gate_open_calls == 0);
    CHECK(fk.pub.recog_pending == 0);

    post_result(b, PLATE_OK1, 0.95f, 0, 203200); /* late result dropped */
    CHECK(fk.gate_open_calls == 0);

    periodic_alive(b, &fk, 207300, 0);
    CHECK(biz_state(b) == BIZ_ST_IDLE);

    biz_destroy(b);
}

static void s3_cloud_extend(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S3 cloud_pending extends timeout 3s -> 6s\n");
    make_cfg(&cfg);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic_alive(b, &fk, 300000, 0);

    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 300000);
    periodic_alive(b, &fk, 303100, 1);             /* past 3s but cloud pending */
    CHECK(biz_state(b) == BIZ_ST_RECOGNIZING);

    periodic_alive(b, &fk, 306200, 1);             /* past 6s hard cap */
    CHECK(biz_state(b) == BIZ_ST_COOL_DOWN);
    CHECK(fk.gate_open_calls == 0);

    biz_destroy(b);
}

static void s4_core1_dead(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S4 core1 dead at registration -> immediate downgrade\n");
    make_cfg(&cfg);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic(b, &fk, 400000, 0, 0);             /* hb stays 0: never alive */
    CHECK((fk.pub.fault_bits & 0x04) != 0);

    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 400000);
    CHECK(biz_state(b) == BIZ_ST_COOL_DOWN);    /* via DENY */
    CHECK(fk.notify_01 == 0);                   /* no recognition trigger */
    CHECK(fk.gate_open_calls == 0);

    biz_destroy(b);
}

static void s5_whitelist_reject(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S5 whitelist reject keeps last plate\n");
    make_cfg(&cfg);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic_alive(b, &fk, 500000, 0);

    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 500000);
    post_result(b, PLATE_BAD, 0.90f, 0, 500100);
    CHECK(biz_state(b) == BIZ_ST_COOL_DOWN);
    CHECK(fk.gate_open_calls == 0);
    CHECK(strcmp(fk.pub.plate, PLATE_BAD) == 0);

    biz_destroy(b);
}

static void s6_expiry_and_deny(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S6 expiry + deny permission\n");
    app_config_defaults(&cfg);
    wl_init(&cfg.wl);
    wl_add(&cfg.wl, "C11111", 1, 19000101);     /* expired (virtual clock
                                                   maps times to 1970) */
    wl_add(&cfg.wl, "D22222", 0, 0);            /* explicitly denied */
    wl_add(&cfg.wl, "E33333", 1, 20991231);     /* far future: ok */
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic_alive(b, &fk, 600000, 0);

    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 600000);
    post_result(b, "C11111", 0.9f, 0, 600100);
    CHECK(biz_state(b) == BIZ_ST_COOL_DOWN);
    CHECK(fk.gate_open_calls == 0);
    post(b, &fk, BIZ_EV_SHADE_CLEAR, 601000);   /* denied car leaves */
    periodic_alive(b, &fk, 604200, 0);
    CHECK(biz_state(b) == BIZ_ST_IDLE);

    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 610000);
    post_result(b, "D22222", 0.9f, 0, 610100);
    CHECK(fk.gate_open_calls == 0);
    post(b, &fk, BIZ_EV_SHADE_CLEAR, 611000);
    periodic_alive(b, &fk, 614200, 0);

    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 620000);
    post_result(b, "E33333", 0.9f, 0, 620100);
    CHECK(fk.gate_open_calls == 1);

    biz_destroy(b);
}

static void s7_remote_dedup(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S7 remote open/close dedup\n");
    make_cfg(&cfg);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic_alive(b, &fk, 700000, 0);

    post(b, &fk, BIZ_EV_REQ_GATE_OPEN, 700000);       /* downgrade channel */
    CHECK(fk.gate_open_calls == 1);
    CHECK(biz_state(b) == BIZ_ST_IDLE);               /* state untouched */

    post(b, &fk, BIZ_EV_REQ_GATE_OPEN, 700100);       /* same, in window */
    CHECK(fk.gate_open_calls == 1);

    post_m4(b, 1, 1, 0, 700200);                      /* gate observed open */
    CHECK(fk.pub.gate_state == 1);

    post(b, &fk, BIZ_EV_REQ_GATE_CLOSE, 700300);
    CHECK(fk.gate_close_calls == 1);

    post_m4(b, 0, 1, 0, 700400);                      /* observed closed */
    post(b, &fk, BIZ_EV_REQ_GATE_CLOSE, 700500);      /* already closed */
    CHECK(fk.gate_close_calls == 1);

    /* --- board-found regression (2026-09-11) ---
     * On real hardware the M4 answers 0x23 only when asked, so no report
     * follows our own command: the "observed" state stayed stale and a CLOSE
     * right after OPEN was deduped away as "gate already closed" - the 0x12
     * never reached the C8T6 (symptom: touch "close" did nothing). The
     * commanded target is now the working state, and a deferred 0x13 resync
     * refreshes it from the M4 once the mechanics settle (spec 6.3). */
    biz_destroy(b);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic_alive(b, &fk, 710000, 0);

    post(b, &fk, BIZ_EV_REQ_GATE_OPEN, 710000);
    CHECK(fk.gate_open_calls == 1);
    CHECK(fk.pub.gate_state == 1);                    /* working state = cmd */

    /* a 0x23 generated BEFORE our command (in-flight answer to an earlier
     * periodic/edge resync) carries the old position and must not undo the
     * command: that was the "开 -> 一瞬间关 -> 开" flicker on hardware */
    post_m4(b, 0, 1, 0, 710400);
    CHECK(fk.pub.gate_state == 1);                    /* guard kept the cmd */

    post(b, &fk, BIZ_EV_REQ_GATE_CLOSE, 710500);      /* no valid 0x23 yet */
    CHECK(fk.gate_close_calls == 1);                  /* must not be deduped */
    CHECK(fk.query_calls == 0);                       /* nothing queried yet */
    periodic_alive(b, &fk, 711500, 0);                /* < 710500 + 1600ms */
    CHECK(fk.query_calls == 0);                       /* settle not due yet */
    periodic_alive(b, &fk, 712200, 0);                /* > 710500 + 1600ms */
    CHECK(fk.query_calls == 1);                       /* deferred resync out */

    /* a late authoritative answer (after the guard) is applied */
    post_m4(b, 1, 1, 0, 712500);
    CHECK(fk.pub.gate_state == 1);

    /* slow periodic resync while the link is up: keeps gate_state honest
     * even with no command and no spontaneous 0x23 from the M4 */
    {
        int q0;
        post_link(b, 1, 712600);                      /* on_link resyncs once */
        q0 = fk.query_calls;
        periodic_alive(b, &fk, 714300, 0);            /* > 712200 + 2000ms */
        CHECK(fk.query_calls == q0 + 1);
    }

    biz_destroy(b);
}

/*
 * S8 - P4-04 acceptance: 10 consecutive passages counted exactly once
 * each (no double count on a repeated shade-clear edge, no miss), then
 * exit mode counts down and clamps at 0 (spec 5.4.1.1/2/4).
 *
 * Each round emulates the real loop shape: shade arrive -> recognition
 * matches the whitelist -> auto open -> cooldown expires -> shade clear
 * completes the passage -> gate closes again (as C8T6 does), which also
 * re-arms the next open command (observed-state aware, spec 4.2.3).
 */
static void s8_passage_counting(void)
{
    app_config_t cfg, c2;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;
    int i;

    printf("S8 passage counted exactly once (10 rounds, both modes)\n");

    make_cfg(&cfg);
    cfg.total_slots = 30;
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    CHECK(b != NULL);
    periodic_alive(b, &fk, 190000, 0);          /* core1 alive */
    CHECK(fk.pub.used_slots == 0);
    CHECK(fk.pub.free_slots == 30);

    /* ---- 10 entry passages: +1 each, exactly once ---- */
    for (i = 0; i < 10; i++) {
        int64_t t = 200000 + (int64_t)i * 10000;

        post(b, &fk, BIZ_EV_SHADE_ARRIVE, t);
        post_result(b, PLATE_OK1, 0.95f, 0, t + 100);
        periodic_alive(b, &fk, t + 4300, 0);        /* cooldown (4s) expires */
        post(b, &fk, BIZ_EV_SHADE_CLEAR, t + 4400); /* passage completes */
        post(b, &fk, BIZ_EV_SHADE_CLEAR, t + 4500); /* same edge: no count */
        CHECK(fk.pub.used_slots == i + 1);
        CHECK(fk.pub.free_slots == 30 - (i + 1));
        CHECK(fk.gate_open_calls == i + 1);

        /* gate closes after the passage (C8T6 reaction) -> re-arms open */
        post(b, &fk, BIZ_EV_REQ_GATE_CLOSE, t + 5000);
        post_m4(b, 0, 1, 0, t + 5050);
    }
    CHECK(fk.pub.used_slots == 10);
    CHECK(fk.pub.free_slots == 20);
    CHECK(strcmp(fk.pub.plate, PLATE_OK1) == 0);

    /* ---- exit mode: same edge logic, counter goes down ---- */
    c2 = cfg;
    c2.count_mode = 1;                          /* 1 = exit (-1) */
    biz_apply_config(b, &c2);
    for (i = 0; i < 3; i++) {
        int64_t t = 400000 + (int64_t)i * 10000;

        post(b, &fk, BIZ_EV_SHADE_ARRIVE, t);
        post_result(b, PLATE_OK1, 0.95f, 0, t + 100);
        periodic_alive(b, &fk, t + 4300, 0);
        post(b, &fk, BIZ_EV_SHADE_CLEAR, t + 4400);
        CHECK(fk.pub.used_slots == 10 - (i + 1));
        post(b, &fk, BIZ_EV_REQ_GATE_CLOSE, t + 5000);
        post_m4(b, 0, 1, 0, t + 5050);
    }

    /* 8 more exits: 7 -> 0 then clamped (no negative, spec 5.4.3.1) */
    for (i = 0; i < 8; i++) {
        int64_t t = 500000 + (int64_t)i * 10000;

        post(b, &fk, BIZ_EV_SHADE_ARRIVE, t);
        post_result(b, PLATE_OK1, 0.95f, 0, t + 100);
        periodic_alive(b, &fk, t + 4300, 0);
        post(b, &fk, BIZ_EV_SHADE_CLEAR, t + 4400);
        CHECK(fk.pub.used_slots >= 0);
        CHECK(fk.pub.used_slots + fk.pub.free_slots == 30);
        post(b, &fk, BIZ_EV_REQ_GATE_CLOSE, t + 5000);
        post_m4(b, 0, 1, 0, t + 5050);
    }
    CHECK(fk.pub.used_slots == 0);
    CHECK(fk.pub.free_slots == 30);

    biz_destroy(b);
}

static void s9_slots_clamp(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S9 slots clamp at total (core1 dead: deny path, counting still on)\n");
    app_config_defaults(&cfg);
    cfg.total_slots = 2;
    wl_init(&cfg.wl);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic(b, &fk, 900000, 0, 0);             /* core1 dead: cars denied */

    /* passage 1: gate opens remotely, clear without prior arrive edge */
    post(b, &fk, BIZ_EV_REQ_GATE_OPEN, 900000);
    post_m4(b, 1, 1, 0, 900050);
    post(b, &fk, BIZ_EV_SHADE_CLEAR, 900100);   /* no edge: no count */
    CHECK(fk.pub.used_slots == 0);

    /* passage 2: proper arrive->clear edge completes the count */
    post(b, &fk, BIZ_EV_REQ_GATE_CLOSE, 905000);
    post_m4(b, 0, 1, 0, 905050);
    post(b, &fk, BIZ_EV_REQ_GATE_OPEN, 910000);
    post_m4(b, 1, 1, 0, 910050);
    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 910100);  /* deny path (core1 dead) */
    post(b, &fk, BIZ_EV_SHADE_CLEAR, 910200);
    CHECK(fk.pub.used_slots == 1);
    CHECK(fk.pub.free_slots == 1);

    /* passage 3: fills up to total */
    post(b, &fk, BIZ_EV_REQ_GATE_CLOSE, 915000);
    post_m4(b, 0, 1, 0, 915050);
    post(b, &fk, BIZ_EV_REQ_GATE_OPEN, 920000);
    post_m4(b, 1, 1, 0, 920050);
    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 920100);
    post(b, &fk, BIZ_EV_SHADE_CLEAR, 920200);
    CHECK(fk.pub.used_slots == 2);
    CHECK(fk.pub.free_slots == 0);

    /* passage 4: clamped at total */
    post(b, &fk, BIZ_EV_REQ_GATE_CLOSE, 925000);
    post_m4(b, 0, 1, 0, 925050);
    post(b, &fk, BIZ_EV_REQ_GATE_OPEN, 930000);
    post_m4(b, 1, 1, 0, 930050);
    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 930100);
    post(b, &fk, BIZ_EV_SHADE_CLEAR, 930200);
    CHECK(fk.pub.used_slots == 2);
    CHECK(fk.pub.free_slots == 0);

    biz_destroy(b);
}

static void s10_config(void)
{
    app_config_t base, out;
    char err[256];
    FILE *fp;
    int rc;

    printf("S10 config parse + invalid value rejection\n");
    app_config_defaults(&base);

    fp = fopen("st_core0.conf", "w");
    CHECK(fp != NULL);
    if (fp == NULL) return;
    fprintf(fp,
        "# test config\n"
        "[settings]\n"
        "total_slots = 30\n"
        "conf_threshold = 1.5\n"          /* invalid -> keep base 0.60 */
        "total_slots2 = 5\n"              /* unknown key -> ignored */
        "count_mode = exit\n"
        "[whitelist]\n"
        "\xE8\x8B\x8F""A12345 allow 2099-12-31\n"
        "XY9999 deny 2020-01-01\n");
    fclose(fp);

    rc = app_config_load(&base, &out, "st_core0.conf", err, sizeof(err));
    CHECK(rc == 0);
    CHECK(out.total_slots == 30);
    CHECK(out.count_mode == 1);
    CHECK(out.conf_threshold == 0.60f);
    CHECK(wl_count(&out.wl) == 2);
    CHECK(wl_match(&out.wl, PLATE_OK1, 20260910) == WL_MATCH_OK);
    CHECK(wl_match(&out.wl, "XY9999", 20260910) == WL_NOT_ALLOWED);
    CHECK(wl_match(&out.wl, "ZZ0000", 20260910) == WL_NO_ENTRY);

    /* reload with invalid ranges keeps base values */
    fp = fopen("st_core0.conf", "w");
    fprintf(fp, "[settings]\ntotal_slots = -3\nrecog_timeout_ms = 5\n");
    fclose(fp);
    rc = app_config_load(&out, &out, "st_core0.conf", err, sizeof(err));
    CHECK(rc == 0);
    CHECK(out.total_slots == 30);            /* kept previous */
    CHECK(out.recog_timeout_ms == 3000);     /* kept previous */
    remove("st_core0.conf");
}

static void s11_dup_shade(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S11 duplicate shade arrival ignored while busy\n");
    make_cfg(&cfg);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic_alive(b, &fk, 1100000, 0);

    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 1100000);
    CHECK(biz_state(b) == BIZ_ST_RECOGNIZING);
    post(b, &fk, BIZ_EV_SHADE_ARRIVE, 1100100);
    CHECK(fk.notify_01 == 1);                   /* no second trigger */
    CHECK(biz_state(b) == BIZ_ST_RECOGNIZING);

    biz_destroy(b);
}

static void s12_link_fault(void)
{
    app_config_t cfg;
    biz_platform_t plat;
    fake_t fk;
    biz_t *b;

    printf("S12 link down/up -> fault bit0 + resync query\n");
    make_cfg(&cfg);
    fake_platform(&plat, &fk);
    b = biz_create(&cfg, &plat);
    periodic_alive(b, &fk, 1200000, 0);
    CHECK((fk.pub.fault_bits & 0x01) != 0);     /* rpmsg down at start */

    post_link(b, 1, 1200050);                   /* up: edge */
    CHECK((fk.pub.fault_bits & 0x01) == 0);
    CHECK(fk.query_calls == 1);                 /* resync 0x13 on link up */

    post_link(b, 0, 1200100);                   /* down: bit0 set */
    CHECK((fk.pub.fault_bits & 0x01) != 0);
    post_link(b, 0, 1200150);                   /* no edge */
    CHECK((fk.pub.fault_bits & 0x01) != 0);
    post_link(b, 1, 1200200);                   /* up again */
    CHECK((fk.pub.fault_bits & 0x01) == 0);
    CHECK(fk.query_calls == 2);

    biz_destroy(b);
}

static void s13_log_and_storage(void)
{
    FILE *fp;
    char line[256];
    int seen_warn = 0, seen_err = 0, seen_leak = 0, stamp_ok = 0;

    printf("S13 log level filter + file target; storage off is a no-op "
           "(P4-07/P4-08)\n");

    /* P4-08: level helpers used by the config parser */
    CHECK(log_level_from_str("debug") == LOG_DEBUG);
    CHECK(log_level_from_str("WARN")  == LOG_WARN);
    CHECK(log_level_from_str("error") == LOG_ERROR);
    CHECK(log_level_from_str("nope")  == -1);
    CHECK(strcmp(log_level_name(LOG_INFO), "INFO") == 0);

    /* P4-08 rule 3: WARN hides DEBUG/INFO and keeps WARN/ERROR;
       rule 1: every emitted line carries level + tag + ms timestamp. */
    remove("st_log.txt");
    CHECK(log_open_file("st_log.txt") == 0);
    log_set_level(LOG_WARN);
    LOGD("selftest", "debug line must be filtered");
    LOGI("selftest", "info line must be filtered");
    LOGW("selftest", "warn line must appear");
    LOGE("selftest", "error line must appear");
    log_close_file();
    log_set_level(LOG_WARN);            /* restore the run's level */

    fp = fopen("st_log.txt", "r");
    CHECK(fp != NULL);
    if (fp != NULL) {
        while (fgets(line, sizeof(line), fp) != NULL) {
            if (strstr(line, "warn line must appear")  != NULL) seen_warn = 1;
            if (strstr(line, "error line must appear") != NULL) seen_err  = 1;
            if (strstr(line, "must be filtered")       != NULL) seen_leak = 1;
            if (strlen(line) > 12 && line[2] == ':' && line[5] == ':' &&
                line[8] == '.' && strstr(line, "WARN [selftest]") != NULL)
                stamp_ok = 1;
        }
        fclose(fp);
    }
    CHECK(seen_warn == 1);
    CHECK(seen_err == 1);
    CHECK(seen_leak == 0);
    CHECK(stamp_ok == 1);
    remove("st_log.txt");

    /* P4-07: default build compiles storage out -> entry points no-op,
       nothing is written anywhere (dir="." would be visible if it ran). */
    CHECK(store_init(".") == 0);
    store_event("arrive", PLATE_OK1, "selftest");
    store_gate("open", "auto");
    store_shutdown();
#if ENABLE_STORAGE
    fp = fopen("events.log", "r");
    CHECK(fp != NULL);
    if (fp != NULL) fclose(fp);
    fp = fopen("gate_log", "r");
    CHECK(fp != NULL);
    if (fp != NULL) fclose(fp);
    remove("events.log");
    remove("gate_log");
#else
    fp = fopen("events.log", "r");
    CHECK(fp == NULL);
    if (fp != NULL) fclose(fp);
    fp = fopen("gate_log", "r");
    CHECK(fp == NULL);
    if (fp != NULL) fclose(fp);
#endif
}

int main(void)
{
    log_set_level(LOG_WARN);    /* keep the output focused on failures */

    s1_happy_path();
    s2_timeout();
    s3_cloud_extend();
    s4_core1_dead();
    s5_whitelist_reject();
    s6_expiry_and_deny();
    s7_remote_dedup();
    s8_passage_counting();
    s9_slots_clamp();
    s10_config();
    s11_dup_shade();
    s12_link_fault();
    s13_log_and_storage();

    store_shutdown();
    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}
