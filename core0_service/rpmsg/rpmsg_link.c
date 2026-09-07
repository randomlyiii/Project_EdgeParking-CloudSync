/*
 * rpmsg_link.c — Linux(Core0) 侧 RPMSG 通道管理层实现
 *
 * 线程模型：单 RX 线程 poll(ttyFD + wakePipe)。
 *   - 心跳：UP 后每 hb_interval_ms 发 0x7E（1B 序号 0..255 循环）
 *   - 保鲜：任何 CRC 通过的帧（0x7E/0x21/0x23…）都刷新 last_rx
 *   - DOWN：last_rx 超时 / 读 EOF / POLLERR|POLLHUP → 关 fd → 重开 → 清 RX → 发 0x13 重同步
 * 锁纪律：RX 线程内先持锁完成“读→拆帧→收集/更新快照”，再释放锁后调用用户 on_frame；
 *         用户回调里可自由调用 rpmsg_link_send*（不会死锁）。统计/快照读取全走同一把锁。
 */
#include "rpmsg_link.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define RPMSG_DEFAULT_DEVICE "/dev/ttyRPMSG0"
#define RPMSG_PENDING_MAX    64u  /* RX 线程内收集帧容量：单次 read(≤512B) 的理论帧数上限 */

struct rpmsg_link {
    rpmsg_link_cfg_t cfg;

    pthread_t   thread;
    int         fd;          /* <0 = 未打开 */
    int         wake_rd;     /* 唤醒管道（stop 用） */
    int         wake_wr;
    volatile int running;    /* 0 = 请求退出 */

    pthread_mutex_t lock;    /* 保护 fd/统计/快照/seq；用户回调绝不在持锁时调用 */

    rpmsg_rx_t  rx;
    uint16_t    tx_seq[256];     /* 发送方按 type 各自计数 */
    uint8_t     hb_ord;          /* 0x7E payload 序号（0..255 循环） */
    uint64_t    hb_next_ms;

    uint64_t    last_rx_ms;      /* 最后有效帧时刻（monotonic ms） */
    rpmsg_m4_state_t m4;         /* 最近 0x23 快照 */
    int         have_m4;

    /* RX 线程内帧收集（feed 回调只做拷贝，不跑用户代码） */
    rpmsg_frame_t pending[RPMSG_PENDING_MAX];
    size_t        pending_cnt;
};

/* ---------------- 内部工具 ---------------- */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static void sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000u;
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

static void link_set_state(rpmsg_link_t *lk, int up)
{
    if (lk->cfg.on_link != NULL)
        lk->cfg.on_link(up ? RPMSG_LINK_UP : RPMSG_LINK_DOWN, lk->cfg.opaque);
}

/* termios raw：rpmsg 字符设备按 tty 处理；失败可忽略（非 tty） */
static void cfg_raw(int fd)
{
    struct termios tio;

    if (!isatty(fd))
        return;
    if (tcgetattr(fd, &tio) != 0)
        return;
    cfmakeraw(&tio);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tio);
}

/* 非阻塞写完全：EAGAIN 时短等 POLLOUT（调用方已持锁） */
static int write_all_locked(int fd, const uint8_t *buf, size_t n)
{
    size_t off = 0u;

    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 200) <= 0)
                return -1;
            continue;
        }
        if (w < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

/* 内部发送：调用方须已持锁 */
static int link_write_frame_locked(rpmsg_link_t *lk, uint8_t type,
                                   const uint8_t *payload, uint16_t plen)
{
    uint8_t frame[RPMSG_FRAME_MAX];
    uint16_t seq;
    int total;

    if (lk->fd < 0)
        return -1;

    seq = lk->tx_seq[type]++;
    total = rpmsg_frame_build(type, seq, payload, plen, frame, sizeof(frame));
    if (total < 0)
        return -1;
    return write_all_locked(lk->fd, frame, (size_t)total);
}

/* RX feed 的收集回调：只拷贝到 pending，绝不在持锁时跑用户代码 */
static int pending_collect(const rpmsg_frame_t *f, void *opaque)
{
    rpmsg_link_t *lk = (rpmsg_link_t *)opaque;

    if (lk->pending_cnt < RPMSG_PENDING_MAX) {
        lk->pending[lk->pending_cnt++] = *f;   /* 浅拷贝：payload 是内嵌数组 */
        return 0;
    }
    return 1;   /* 缓冲满：停止本次喂入（极罕见，防饿死循环） */
}

/* 处理一批收集到的帧：更新快照/保鲜在锁内；用户回调在锁外 */
static void dispatch_pending(rpmsg_link_t *lk)
{
    size_t i, n;

    if (lk->pending_cnt == 0u)
        return;

    pthread_mutex_lock(&lk->lock);
    n = lk->pending_cnt;
    for (i = 0u; i < n; i++) {
        const rpmsg_frame_t *f = &lk->pending[i];
        /* 0x23 → M4 全量状态快照（payload 布局见 docs/protocols.md §3） */
        if (f->type == RPMSG_RX_M4_STATE && f->len >= RPMSG_M4_STATE_LEN) {
            lk->m4.gate_state  = f->payload[0];
            lk->m4.node_online = f->payload[1];
            lk->m4.can_err_cnt = (uint16_t)(f->payload[2] | ((uint16_t)f->payload[3] << 8));
            lk->have_m4 = 1;
        }
    }
    lk->last_rx_ms = now_ms();   /* 任何有效帧保鲜 */
    lk->pending_cnt = 0u;
    pthread_mutex_unlock(&lk->lock);

    /* 锁外派发用户回调（回调可再调用 send*，不会死锁） */
    if (lk->cfg.on_frame != NULL) {
        for (i = 0u; i < n; i++)
            lk->cfg.on_frame(&lk->pending[i], lk->cfg.opaque);
    }
}

/* ---------------- 公开 API ---------------- */

rpmsg_link_t *rpmsg_link_create(const rpmsg_link_cfg_t *cfg)
{
    rpmsg_link_t *lk;

    lk = (rpmsg_link_t *)calloc(1, sizeof(*lk));
    if (lk == NULL)
        return NULL;

    if (cfg != NULL)
        lk->cfg = *cfg;
    if (lk->cfg.device == NULL)
        lk->cfg.device = RPMSG_DEFAULT_DEVICE;
    if (lk->cfg.hb_interval_ms == 0u)
        lk->cfg.hb_interval_ms = 500u;
    if (lk->cfg.timeout_ms == 0u)
        lk->cfg.timeout_ms = 1000u;
    if (lk->cfg.reopen_ms == 0u)
        lk->cfg.reopen_ms = 1000u;

    lk->fd = -1;
    lk->wake_rd = -1;
    lk->wake_wr = -1;
    lk->running = 0;
    pthread_mutex_init(&lk->lock, NULL);
    rpmsg_rx_init(&lk->rx);
    return lk;
}

static void *rx_thread_main(void *arg)
{
    rpmsg_link_t *lk = (rpmsg_link_t *)arg;

    while (lk->running) {
        /* ---- 确保设备打开 ---- */
        if (lk->fd < 0) {
            int fd = open(lk->cfg.device, O_RDWR | O_NOCTTY | O_NONBLOCK);

            if (fd < 0) {
                /* 设备不存在（remoteproc 未加载/已停）：按 reopen_ms 节奏重试，不崩溃 */
                sleep_ms(lk->cfg.reopen_ms);
                continue;
            }
            cfg_raw(fd);

            pthread_mutex_lock(&lk->lock);
            lk->fd = fd;
            rpmsg_rx_reset(&lk->rx);               /* 旧缓冲/旧 seq 作废 */
            lk->last_rx_ms = now_ms();
            lk->hb_next_ms = lk->last_rx_ms + lk->cfg.hb_interval_ms;
            pthread_mutex_unlock(&lk->lock);

            link_set_state(lk, 1);
            /* 重同步：连上即发 0x13 全量查询（P3-09）；恢复后 M4 回 0x23 刷新快照 */
            rpmsg_link_send_query(lk);
            continue;
        }

        /* ---- poll 等待可读/唤醒 ---- */
        {
            struct pollfd pfds[2];
            int timeout_ms;

            pthread_mutex_lock(&lk->lock);
            {
                uint64_t now = now_ms();
                uint64_t deadline = lk->last_rx_ms + lk->cfg.timeout_ms;
                uint64_t hb_dead = lk->hb_next_ms;
                uint64_t next = (hb_dead < deadline) ? hb_dead : deadline;
                if (now >= next)
                    timeout_ms = 0;
                else if (next - now > 500u)
                    timeout_ms = 500;
                else
                    timeout_ms = (int)(next - now);
            }
            pthread_mutex_unlock(&lk->lock);

            pfds[0].fd = lk->fd;
            pfds[0].events = POLLIN;
            pfds[0].revents = 0;
            pfds[1].fd = lk->wake_rd;
            pfds[1].events = POLLIN;
            pfds[1].revents = 0;

            if (poll(pfds, 2, timeout_ms) < 0) {
                if (errno == EINTR)
                    continue;
                /* poll 失败：按断链处理 */
                pthread_mutex_lock(&lk->lock);
                if (lk->fd >= 0) { close(lk->fd); lk->fd = -1; }
                pthread_mutex_unlock(&lk->lock);
                link_set_state(lk, 0);
                continue;
            }

            /* 唤醒（stop） */
            if (pfds[1].revents & POLLIN)
                break;

            /* 设备异常 */
            if (pfds[0].revents & (POLLERR | POLLHUP)) {
                pthread_mutex_lock(&lk->lock);
                if (lk->fd >= 0) { close(lk->fd); lk->fd = -1; }
                pthread_mutex_unlock(&lk->lock);
                link_set_state(lk, 0);
                continue;
            }

            /* 读数据：持锁拆帧+收集，解锁后派发 */
            if (pfds[0].revents & POLLIN) {
                uint8_t tmp[512];
                int eof_err = 0;

                for (;;) {
                    ssize_t r = read(lk->fd, tmp, sizeof(tmp));
                    if (r > 0) {
                        pthread_mutex_lock(&lk->lock);
                        rpmsg_rx_feed(&lk->rx, tmp, (size_t)r,
                                      pending_collect, lk);
                        pthread_mutex_unlock(&lk->lock);
                        dispatch_pending(lk);
                    } else if (r == 0) {
                        eof_err = 1;   /* EOF：对端(remoteproc/M4)关闭 */
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;                       /* 本批读完 */
                        if (errno == EINTR)
                            continue;
                        eof_err = 1;
                        break;
                    }
                }
                if (eof_err) {
                    pthread_mutex_lock(&lk->lock);
                    if (lk->fd >= 0) { close(lk->fd); lk->fd = -1; }
                    pthread_mutex_unlock(&lk->lock);
                    link_set_state(lk, 0);
                    continue;   /* EOF/错误：回顶部重开 */
                }
                /* 正常读到数据：继续执行下方心跳/超时检查（避免高流量下饿死心跳） */
            }

            /* ---- 周期任务：心跳 & 超时（持锁短操作） ---- */
            pthread_mutex_lock(&lk->lock);
            {
                uint64_t now2 = now_ms();
                int want_close = 0;

                if (lk->fd >= 0 && now2 >= lk->hb_next_ms) {
                    uint8_t pl = lk->hb_ord++;
                    link_write_frame_locked(lk, RPMSG_RX_HEARTBEAT, &pl, RPMSG_HB_LEN);
                    lk->hb_next_ms = now2 + lk->cfg.hb_interval_ms;
                }
                if (lk->fd >= 0 && now2 - lk->last_rx_ms >= lk->cfg.timeout_ms)
                    want_close = 1;
                if (want_close && lk->fd >= 0) {
                    close(lk->fd);
                    lk->fd = -1;
                }
            }
            pthread_mutex_unlock(&lk->lock);

            if (lk->fd < 0)
                link_set_state(lk, 0);   /* 超时判 DOWN（重开在循环顶部） */
        }
    }
    return NULL;
}

int rpmsg_link_start(rpmsg_link_t *lk)
{
    int p[2];

    if (lk == NULL || lk->running)
        return -1;
    if (pipe(p) != 0)
        return -1;
    lk->wake_rd = p[0];
    lk->wake_wr = p[1];
    lk->running = 1;
    if (pthread_create(&lk->thread, NULL, rx_thread_main, lk) != 0) {
        lk->running = 0;
        close(p[0]);
        close(p[1]);
        lk->wake_rd = lk->wake_wr = -1;
        return -1;
    }
    return 0;
}

void rpmsg_link_stop(rpmsg_link_t *lk)
{
    if (lk == NULL || !lk->running)
        return;
    lk->running = 0;
    if (lk->wake_wr >= 0) {
        char c = 1;
        ssize_t w = write(lk->wake_wr, &c, 1);
        (void)w;
    }
    pthread_join(lk->thread, NULL);

    pthread_mutex_lock(&lk->lock);
    if (lk->fd >= 0) {
        close(lk->fd);
        lk->fd = -1;
    }
    pthread_mutex_unlock(&lk->lock);

    if (lk->wake_rd >= 0) close(lk->wake_rd);
    if (lk->wake_wr >= 0) close(lk->wake_wr);
    lk->wake_rd = lk->wake_wr = -1;
}

void rpmsg_link_free(rpmsg_link_t *lk)
{
    if (lk == NULL)
        return;
    rpmsg_link_stop(lk);
    pthread_mutex_destroy(&lk->lock);
    free(lk);
}

rpmsg_link_state_t rpmsg_link_get_state(const rpmsg_link_t *lk)
{
    rpmsg_link_state_t st = RPMSG_LINK_DOWN;

    if (lk == NULL)
        return st;
    pthread_mutex_lock((pthread_mutex_t *)&lk->lock);
    st = (lk->fd >= 0) ? RPMSG_LINK_UP : RPMSG_LINK_DOWN;
    pthread_mutex_unlock((pthread_mutex_t *)&lk->lock);
    return st;
}

int rpmsg_link_send(rpmsg_link_t *lk, uint8_t type,
                    const uint8_t *payload, uint16_t plen)
{
    int rc;

    if (lk == NULL)
        return -1;
    if (plen > RPMSG_PAYLOAD_MAX)
        return -1;
    if (payload == NULL && plen > 0u)
        return -1;

    pthread_mutex_lock(&lk->lock);
    rc = link_write_frame_locked(lk, type, payload, plen);
    pthread_mutex_unlock(&lk->lock);
    return rc;
}

int rpmsg_link_send_gate_open(rpmsg_link_t *lk)
{
    return rpmsg_link_send(lk, RPMSG_TX_GATE_OPEN, NULL, 0u);
}

int rpmsg_link_send_gate_close(rpmsg_link_t *lk)
{
    return rpmsg_link_send(lk, RPMSG_TX_GATE_CLOSE, NULL, 0u);
}

int rpmsg_link_send_query(rpmsg_link_t *lk)
{
    return rpmsg_link_send(lk, RPMSG_TX_QUERY_STATE, NULL, 0u);
}

uint64_t rpmsg_link_get_last_rx_ms(const rpmsg_link_t *lk)
{
    uint64_t v = 0u;

    if (lk == NULL)
        return v;
    pthread_mutex_lock((pthread_mutex_t *)&lk->lock);
    v = lk->last_rx_ms;
    pthread_mutex_unlock((pthread_mutex_t *)&lk->lock);
    return v;
}

void rpmsg_link_get_stats(const rpmsg_link_t *lk, rpmsg_rx_stats_t *out)
{
    if (lk == NULL || out == NULL)
        return;
    pthread_mutex_lock((pthread_mutex_t *)&lk->lock);
    *out = lk->rx.stats;
    pthread_mutex_unlock((pthread_mutex_t *)&lk->lock);
}

int rpmsg_link_get_m4_state(const rpmsg_link_t *lk, rpmsg_m4_state_t *out)
{
    int have = 0;

    if (lk == NULL || out == NULL)
        return -1;
    pthread_mutex_lock((pthread_mutex_t *)&lk->lock);
    have = lk->have_m4;
    if (have)
        *out = lk->m4;
    pthread_mutex_unlock((pthread_mutex_t *)&lk->lock);
    return have ? 0 : -1;
}
