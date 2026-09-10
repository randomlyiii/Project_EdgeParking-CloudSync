/*
 * core0_main.c - core0 business daemon entry (A7-Core0, step 4).
 *
 * Wiring (event driven, spec 4.1.5):
 *   - rpmsg_link RX thread  -> event queue -> epoll eventfd -> business
 *   - 200 ms timerfd        -> shm poll (result / remote requests /
 *                             hb_core1 / cloud_pending) + config hot
 *                             reload check + biz_periodic()
 *   - signalfd              -> SIGINT/SIGTERM graceful exit
 *
 * The rpmsg frame -> business event decoding (CAN semantics of 0x21,
 * spec docs/protocols.md 1/3) lives here; the state machine itself
 * stays pure (business/).
 *
 * Usage: sudo ./core0d [-c core0.conf] [-d /dev/ttyRPMSG0]
 *                      [--evt-c1 <fd>] [-l debug|info|warn|error]
 *   --evt-c1 : optional pre-created eventfd number, inherited by the
 *              core1 process for 0x01/0x03/0x04 events (P6 wiring).
 *
 * Board build: see Makefile (native gcc on the MP157 / SDK cross).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "app_config.h"
#include "business.h"
#include "ipc_shm.h"
#include "log.h"
#include "rpmsg_link.h"
#include "rpmsg_proto.h"
#include "store.h"

#define TICK_PERIOD_MS   200u    /* shm poll / timers cadence (<=500ms KPI) */
#define QUEUE_MAX        64

/* ------------------------------------------------------------------ */
/* event queue: rpmsg RX thread -> main thread                          */

typedef struct {
    biz_event_t  items[QUEUE_MAX];
    int          head, tail, count;
    int          notify_fd;        /* eventfd used to wake the main loop */
    pthread_mutex_t mtx;
} biz_queue_t;

static void queue_init(biz_queue_t *q, int notify_fd)
{
    memset(q, 0, sizeof(*q));
    q->notify_fd = notify_fd;
    pthread_mutex_init(&q->mtx, NULL);
}

/* Called from the RPMSG RX thread. Enqueue, then bump the eventfd so the
 * main loop's epoll_wait() returns and drains the queue. Without this
 * notify the events sit in the queue until some unrelated wakeup, i.e. the
 * whole RPMSG -> business path (link up/down, 0x21/0x22/0x23) is dead. */
static void queue_push(biz_queue_t *q, const biz_event_t *ev)
{
    int pushed = 0;

    pthread_mutex_lock(&q->mtx);
    if (q->count < QUEUE_MAX) {
        q->items[q->tail] = *ev;
        q->tail = (q->tail + 1) % QUEUE_MAX;
        q->count++;
        pushed = 1;
    } else {
        LOGW("queue", "event queue full, dropping %d", (int)ev->type);
    }
    pthread_mutex_unlock(&q->mtx);

    if (pushed && q->notify_fd >= 0)
        ipc_evt_notify(q->notify_fd, 1u);
}

static int queue_pop(biz_queue_t *q, biz_event_t *ev)
{
    int ok = 0;
    pthread_mutex_lock(&q->mtx);
    if (q->count > 0) {
        *ev = q->items[q->head];
        q->head = (q->head + 1) % QUEUE_MAX;
        q->count--;
        ok = 1;
    }
    pthread_mutex_unlock(&q->mtx);
    return ok;
}

/* ------------------------------------------------------------------ */
/* app context passed as platform ctx                                   */

typedef struct {
    park_shm_t    *shm;
    int            evt_c1_fd;          /* core0 -> core1 eventfd, may be -1 */
    biz_public_t   last_pub;
    rpmsg_link_t  *link;
} app_ctx_t;

static uint64_t now_ms_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

static int plat_gate_open(void *ctx)
{
    app_ctx_t *a = (app_ctx_t *)ctx;
    return rpmsg_link_send_gate_open(a->link);
}

static int plat_gate_close(void *ctx)
{
    app_ctx_t *a = (app_ctx_t *)ctx;
    return rpmsg_link_send_gate_close(a->link);
}

static int plat_query_state(void *ctx)
{
    app_ctx_t *a = (app_ctx_t *)ctx;
    return rpmsg_link_send_query(a->link);
}

static void plat_notify_core1(void *ctx, uint8_t evt_code)
{
    app_ctx_t *a = (app_ctx_t *)ctx;

    /* v3 cross-process channel: this is the path that actually reaches the
     * core1 process (an anonymous eventfd cannot be shared, see park_shm.h).
     * The reader polls evt_seq_c0 every tick (<=200ms). */
    if (a->shm != NULL)
        ipc_shm_evt_notify(a->shm, evt_code);

    /* optional same-process fast path (fd inherited from the launcher) */
    if (a->evt_c1_fd >= 0) {
        if (ipc_evt_notify(a->evt_c1_fd, evt_code) != 0)
            LOGW("ipc", "eventfd notify 0x%02x failed", evt_code);
    }
}

static void plat_publish(void *ctx, const biz_public_t *pub)
{
    app_ctx_t *a = (app_ctx_t *)ctx;
    park_shm_t f;

    memset(&f, 0, sizeof(f));
    f.free_slots = pub->free_slots;
    f.used_slots = pub->used_slots;
    f.gate_state = pub->gate_state;
    f.link_flags = pub->link_flags;
    f.recog_pending = pub->recog_pending;
    f.fault_bits = pub->fault_bits;
    f.conf_threshold = pub->conf_threshold;
    f.result_source = pub->result_source;
    f.confidence = pub->confidence;
    memcpy((void *)f.plate, pub->plate, sizeof(f.plate));
    ipc_shm_publish(a->shm, &f);
    a->last_pub = *pub;
}

/* ------------------------------------------------------------------ */
/* rpmsg frame -> business event decoding (CAN semantics, protocols 1)  */

static void decode_frame(const rpmsg_frame_t *fr, void *opaque)
{
    biz_queue_t *q = (biz_queue_t *)opaque;
    biz_event_t ev;

    memset(&ev, 0, sizeof(ev));

    switch (fr->type) {
    case RPMSG_RX_CAN_EVENT: {
        rpmsg_can_event_t ce;
        if (rpmsg_decode_can_event(fr, &ce) != 0) {
            LOGW("rx", "0x21 decode failed, len=%u", (unsigned)fr->len);
            return;
        }
        if (ce.can_id == 0x200u && ce.dlc >= 1u) {
            uint16_t lux = (uint16_t)((ce.data[1] << 8) | ce.data[2]);
            /* full observation logging incl. lux/drop raw values */
            LOGI("rx", "CAN 0x200 ev=0x%02x lux=%u drop=%u%% status=0x%02x",
                 (unsigned)ce.data[0], (unsigned)lux,
                 ce.dlc >= 4u ? (unsigned)ce.data[3] : 0u,
                 ce.dlc >= 5u ? (unsigned)ce.data[4] : 0u);
            ev.type = (ce.data[0] == 0x01u) ? BIZ_EV_SHADE_ARRIVE
                                            : BIZ_EV_SHADE_CLEAR;
            queue_push(q, &ev);
        } else if (ce.can_id == 0x210u) {
            LOGD("rx", "CAN 0x210 heartbeat id=0x%03X dlc=%u",
                 (unsigned)ce.can_id, (unsigned)ce.dlc);
        } else {
            LOGD("rx", "CAN id=0x%03X dlc=%u (not business relevant)",
                 (unsigned)ce.can_id, (unsigned)ce.dlc);
        }
        break;
    }
    case RPMSG_RX_NODE_STATE:
        ev.type = BIZ_EV_NODE_STATE;
        ev.b0 = (uint8_t)(rpmsg_decode_node_state(fr) == 0 ? 0u : 1u);
        queue_push(q, &ev);
        break;
    case RPMSG_RX_M4_STATE: {
        rpmsg_m4_state_t st;
        if (rpmsg_decode_m4_state(fr, &st) != 0) {
            LOGW("rx", "0x23 decode failed, len=%u", (unsigned)fr->len);
            return;
        }
        ev.type = BIZ_EV_M4_STATE;
        ev.b0 = st.gate_state;
        ev.b1 = st.node_online;
        ev.u16 = st.can_err_cnt;
        queue_push(q, &ev);
        break;
    }
    case RPMSG_RX_HEARTBEAT:
        /* freshness handled by the link layer */
        break;
    default:
        LOGD("rx", "frame type=0x%02x ignored", (unsigned)fr->type);
        break;
    }
}

static void decode_link(rpmsg_link_state_t st, void *opaque)
{
    biz_queue_t *q = (biz_queue_t *)opaque;
    biz_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = BIZ_EV_LINK;
    ev.b0 = (st == RPMSG_LINK_UP) ? 1u : 0u;
    queue_push(q, &ev);
}

/* ------------------------------------------------------------------ */
/* config path handling + hot reload                                    */

static void config_paths(const char *arg, char *path, size_t cap)
{
    struct stat st;

    if (arg != NULL) {
        snprintf(path, cap, "%s", arg);
        return;
    }
    if (stat("core0.conf", &st) == 0) {
        snprintf(path, cap, "core0.conf");
        return;
    }
    snprintf(path, cap, "/etc/core0/core0.conf");
}

int main(int argc, char *argv[])
{
    const char      *dev = "/dev/ttyRPMSG0";
    const char      *conf_arg = NULL;
    const char      *log_env = NULL;
    int              evt_c1_arg = -1;
    int              i;

    char             conf_path[256];
    app_config_t     cfg_buf[2];
    int              cfg_cur = 0;
    char             errbuf[256];

    app_ctx_t        app;
    biz_platform_t   plat;
    biz_t           *biz = NULL;
    biz_queue_t      queue;

    int              epoll_fd = -1;
    int              tick_fd = -1;
    int              queue_efd = -1;
    int              sig_fd = -1;
    park_shm_t      *shm = NULL;
    rpmsg_link_t    *lk = NULL;

    struct stat      conf_st;
    int              conf_st_valid = 0;

    memset(&app, 0, sizeof(app));
    app.evt_c1_fd = -1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
            conf_arg = argv[++i];
        else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            dev = argv[++i];
        else if (strcmp(argv[i], "--evt-c1") == 0 && i + 1 < argc)
            evt_c1_arg = atoi(argv[++i]);
        else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            int lv = log_level_from_str(argv[++i]);
            if (lv >= 0) log_set_level(lv);
        } else if (strcmp(argv[i], "-h") == 0) {
            printf("usage: %s [-c core0.conf] [-d /dev/ttyRPMSG0] "
                   "[--evt-c1 <fd>] [-l level]\n", argv[0]);
            return 0;
        }
    }

    /* env overrides -l for quick journald debugging */
    log_env = getenv("CORE0_LOG_LEVEL");
    if (log_env != NULL) {
        int lv = log_level_from_str(log_env);
        if (lv >= 0) log_set_level(lv);
    }

    /* ---- config (P4-05): local file = single source of truth ---- */
    config_paths(conf_arg, conf_path, sizeof(conf_path));
    app_config_defaults(&cfg_buf[0]);
    errbuf[0] = '\0';
    if (app_config_load(&cfg_buf[0], &cfg_buf[1], conf_path,
                        errbuf, sizeof(errbuf)) != 0) {
        LOGE("config", "initial load failed: %s (defaults in use)", errbuf);
        cfg_buf[1] = cfg_buf[0];
    }
    cfg_cur = 1;
    log_set_level(cfg_buf[cfg_cur].log_level);
    if (cfg_buf[cfg_cur].log_file[0] != '\0')
        log_open_file(cfg_buf[cfg_cur].log_file);
    LOGI("config", "config file: %s", conf_path);

    /* ---- optional local storage (P4-07, default off) ---- */
    store_init(cfg_buf[cfg_cur].storage_dir);

    /* ---- shared memory (core0 owns /park_shm) ---- */
    if (ipc_shm_create(&shm) != 0) {
        LOGE("main", "cannot create /park_shm, exit");
        return 1;
    }
    app.shm = shm;
    app.evt_c1_fd = (evt_c1_arg >= 0) ? evt_c1_arg : -1;

    /* ---- rpmsg channel ---- */
    /* Wakeup fd for the RX thread -> main loop queue. It must exist before
     * the first frame can arrive (decode_frame/decode_link run in the RX
     * thread and push into this queue). */
    queue_efd = ipc_evt_create();
    if (queue_efd < 0) {
        LOGE("main", "queue eventfd create failed, exit");
        return 1;
    }
    queue_init(&queue, queue_efd);
    memset(&plat, 0, sizeof(plat));
    plat.ctx = &app;
    plat.gate_open = plat_gate_open;
    plat.gate_close = plat_gate_close;
    plat.query_state = plat_query_state;
    plat.notify_core1 = plat_notify_core1;
    plat.publish = plat_publish;

    {
        rpmsg_link_cfg_t lc;
        memset(&lc, 0, sizeof(lc));
        lc.device = dev;
        lc.on_frame = decode_frame;
        lc.on_link = decode_link;
        lc.opaque = &queue;
        lc.hb_interval_ms = 500u;
        lc.timeout_ms = 1000u;
        lc.reopen_ms = 1000u;
        lk = rpmsg_link_create(&lc);
    }
    if (lk == NULL) {
        LOGE("main", "rpmsg_link_create failed, exit");
        return 1;
    }
    app.link = lk;

    /* ---- business state machine ---- */
    biz = biz_create(&cfg_buf[cfg_cur], &plat);
    if (biz == NULL) {
        LOGE("main", "biz_create failed, exit");
        return 1;
    }

    /* ---- epoll sources ---- */
    epoll_fd = epoll_create1(0);
    tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    {
        struct itimerspec its;
        its.it_interval.tv_sec = TICK_PERIOD_MS / 1000u;
        its.it_interval.tv_nsec = (long)(TICK_PERIOD_MS % 1000u) * 1000000L;
        its.it_value = its.it_interval;
        timerfd_settime(tick_fd, 0, &its, NULL);
    }
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &mask, NULL);
        sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    }
    if (epoll_fd < 0 || queue_efd < 0 || tick_fd < 0 || sig_fd < 0) {
        LOGE("main", "epoll/eventfd/timerfd/signalfd setup failed");
        return 1;
    }
    {
        struct epoll_event e;
        e.events = EPOLLIN; e.data.fd = queue_efd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, queue_efd, &e);
        e.events = EPOLLIN; e.data.fd = tick_fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tick_fd, &e);
        e.events = EPOLLIN; e.data.fd = sig_fd;
        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sig_fd, &e);
    }

    if (stat(conf_path, &conf_st) == 0)
        conf_st_valid = 1;

    rpmsg_link_start(lk);
    LOGI("main", "core0 business daemon running: dev=%s conf=%s "
         "evt_c1_fd=%d", dev, conf_path, app.evt_c1_fd);

    /* ---- main loop ---- */
    for (;;) {
        struct epoll_event evs[4];
        int n = epoll_wait(epoll_fd, evs, 4, 1000);
        int k;

        for (k = 0; k < n; k++) {
            if (evs[k].data.fd == queue_efd) {
                biz_event_t bev;
                ipc_evt_drain(queue_efd);
                while (queue_pop(&queue, &bev))
                    biz_post(biz, &bev, (int64_t)now_ms_monotonic());
            } else if (evs[k].data.fd == tick_fd) {
                uint64_t ticks = 0;
                biz_poll_in_t pin;
                uint8_t req_open = 0, req_close = 0;
                char plate[16];
                float conf;
                uint8_t source;
                biz_event_t bev;

                {
                    /* consume the expiry count; the tick itself is unused
                     * (the work below runs on every timerfd wakeup) */
                    ssize_t tick_rd = read(tick_fd, &ticks, sizeof(ticks));
                    (void)tick_rd;
                }

                /* v3 cross-process events from core1 (0x02 result ready).
                 * Logged for traceability; the payload is picked up from the
                 * shm fields below (result_valid / gate request pulses), so a
                 * missed event word can never lose a result. */
                {
                    uint8_t c1bits = ipc_shm_evt_take_c1(shm);
                    if (c1bits) {
                        LOGD("ipc", "core1 event bits 0x%02x (result=%d)",
                             (unsigned)c1bits,
                             (c1bits & PARK_EVB_RESULT) ? 1 : 0);
                    }
                }

                /* recognition result (P4: stub-driven, P6: core1) */
                if (ipc_shm_take_result(shm, plate, sizeof(plate),
                                        &conf, &source) == 1) {
                    memset(&bev, 0, sizeof(bev));
                    bev.type = BIZ_EV_RECOG_RESULT;
                    snprintf(bev.plate, sizeof(bev.plate), "%s", plate);
                    bev.conf = conf;
                    bev.source = source;
                    LOGI("ipc", "recognition result: '%s' conf=%.2f src=%s",
                         plate, (double)conf, source ? "cloud" : "edge");
                    biz_post(biz, &bev, (int64_t)now_ms_monotonic());
                }

                /* remote gate requests from core1 UI (pulse semantics) */
                if (ipc_shm_take_requests(shm, &req_open, &req_close) == 1) {
                    if (req_open) {
                        memset(&bev, 0, sizeof(bev));
                        bev.type = BIZ_EV_REQ_GATE_OPEN;
                        biz_post(biz, &bev, (int64_t)now_ms_monotonic());
                    }
                    if (req_close) {
                        memset(&bev, 0, sizeof(bev));
                        bev.type = BIZ_EV_REQ_GATE_CLOSE;
                        biz_post(biz, &bev, (int64_t)now_ms_monotonic());
                    }
                }

                /* periodic: hb watchdog, timeouts, cooldown */
                pin.hb_core1 = shm->hb_core1;
                pin.cloud_pending = shm->cloud_pending;
                biz_periodic(biz, (int64_t)now_ms_monotonic(), &pin);

                /* config hot reload (mtime/size change, <=500ms KPI) */
                {
                    struct stat st2;
                    if (stat(conf_path, &st2) == 0) {
                        int changed = !conf_st_valid ||
                                      st2.st_mtime != conf_st.st_mtime ||
                                      st2.st_size != conf_st.st_size;
                        if (changed) {
                            app_config_t next;
                            conf_st = st2;
                            conf_st_valid = 1;
                            errbuf[0] = '\0';
                            if (app_config_load(&cfg_buf[cfg_cur], &next,
                                                conf_path, errbuf,
                                                sizeof(errbuf)) == 0) {
                                cfg_cur ^= 1;
                                cfg_buf[cfg_cur] = next;
                                log_set_level(cfg_buf[cfg_cur].log_level);
                                biz_apply_config(biz, &cfg_buf[cfg_cur]);
                                LOGI("config", "hot reload done: %s",
                                     errbuf[0] ? errbuf : "ok");
                            } else {
                                LOGE("config", "hot reload failed: %s "
                                     "(previous config kept)", errbuf);
                            }
                        }
                    }
                }
            } else if (evs[k].data.fd == sig_fd) {
                struct signalfd_siginfo si;
                while (read(sig_fd, &si, sizeof(si)) == sizeof(si)) {
                    LOGI("main", "signal %u received, exiting",
                         (unsigned)si.ssi_signo);
                }
                goto out;
            }
        }
    }

out:
    LOGI("main", "shutting down");
    rpmsg_link_stop(lk);
    rpmsg_link_free(lk);
    biz_destroy(biz);
    ipc_evt_drain(queue_efd);
    close(queue_efd);
    close(tick_fd);
    close(sig_fd);
    close(epoll_fd);
    munmap(shm, sizeof(park_shm_t));
    store_shutdown();
    log_close_file();
    return 0;
}
