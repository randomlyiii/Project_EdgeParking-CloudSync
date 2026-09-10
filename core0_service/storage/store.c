/*
 * store.c - optional local storage implementation (P4-07).
 *
 * Without ENABLE_STORAGE every entry point is a no-op and no thread
 * or file is created.
 */
#include "store.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if ENABLE_STORAGE

#define STORE_QUEUE_MAX 32

typedef enum { STORE_Q_EVENT = 0, STORE_Q_GATE } store_q_kind_t;

typedef struct {
    store_q_kind_t kind;
    time_t   ts;
    char     a[32];      /* event: type   | gate: action */
    char     b[64];      /* event: plate  | gate: source */
    char     c[96];      /* event: detail | gate: -      */
} store_item_t;

static pthread_mutex_t g_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static pthread_t       g_thread;
static int             g_started = 0;
static int             g_stop = 0;
static int             g_drop_warned = 0;

static store_item_t    g_q[STORE_QUEUE_MAX];
static int             g_head = 0, g_tail = 0, g_count = 0;

static char            g_dir[128];

static FILE *open_log(const char *name)
{
    char path[192];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    FILE *fp = fopen(path, "a");
    return fp;
}

static void write_ts(FILE *fp, time_t ts)
{
    struct tm tmv;
    char stamp[32];
#if defined(_WIN32)
    localtime_s(&tmv, &ts);
#else
    localtime_r(&ts, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tmv);
    fprintf(fp, "%s\t", stamp);
}

static void *store_thread(void *arg)
{
    (void)arg;
    while (1) {
        store_item_t it;
        pthread_mutex_lock(&g_mtx);
        while (g_count == 0 && !g_stop)
            pthread_cond_wait(&g_cond, &g_mtx);
        if (g_count == 0 && g_stop) {
            pthread_mutex_unlock(&g_mtx);
            break;
        }
        it = g_q[g_head];
        g_head = (g_head + 1) % STORE_QUEUE_MAX;
        g_count--;
        pthread_mutex_unlock(&g_mtx);

        if (it.kind == STORE_Q_EVENT) {
            FILE *fp = open_log("events.log");
            if (fp != NULL) {
                write_ts(fp, it.ts);
                fprintf(fp, "%s\t%s\t%s\n", it.a, it.b, it.c);
                if (ferror(fp)) {
                    LOGE("store", "events.log write failed, record lost");
                }
                fclose(fp);
            } else {
                LOGE("store", "cannot open events.log for append");
            }
        } else {
            FILE *fp = open_log("gate_log");
            if (fp != NULL) {
                write_ts(fp, it.ts);
                fprintf(fp, "%s\t%s\n", it.a, it.b);
                if (ferror(fp)) {
                    LOGE("store", "gate_log write failed, record lost");
                }
                fclose(fp);
            } else {
                LOGE("store", "cannot open gate_log for append");
            }
        }
    }
    return NULL;
}

static void enqueue(const store_item_t *it)
{
    pthread_mutex_lock(&g_mtx);
    if (g_count >= STORE_QUEUE_MAX) {
        pthread_mutex_unlock(&g_mtx);
        if (!g_drop_warned) {
            LOGE("store", "storage queue full, dropping records");
            g_drop_warned = 1;
        }
        return;                      /* never block the business thread */
    }
    g_q[g_tail] = *it;
    g_tail = (g_tail + 1) % STORE_QUEUE_MAX;
    g_count++;
    g_drop_warned = 0;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mtx);
}

int store_init(const char *dir)
{
    if (dir == NULL || dir[0] == '\0') {
        LOGW("store", "ENABLE_STORAGE set but storage_dir empty: storage off");
        return 0;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    g_stop = 0;
    if (pthread_create(&g_thread, NULL, store_thread, NULL) != 0) {
        LOGE("store", "writer thread create failed");
        return -1;
    }
    g_started = 1;
    LOGI("store", "storage enabled, dir=%s", g_dir);
    return 0;
}

void store_event(const char *type, const char *plate, const char *detail)
{
    store_item_t it;
    if (!g_started) return;
    memset(&it, 0, sizeof(it));
    it.kind = STORE_Q_EVENT;
    it.ts = time(NULL);
    snprintf(it.a, sizeof(it.a), "%s", type ? type : "-");
    snprintf(it.b, sizeof(it.b), "%s", plate ? plate : "");
    snprintf(it.c, sizeof(it.c), "%s", detail ? detail : "");
    enqueue(&it);
}

void store_gate(const char *action, const char *source)
{
    store_item_t it;
    if (!g_started) return;
    memset(&it, 0, sizeof(it));
    it.kind = STORE_Q_GATE;
    it.ts = time(NULL);
    snprintf(it.a, sizeof(it.a), "%s", action ? action : "-");
    snprintf(it.b, sizeof(it.b), "%s", source ? source : "-");
    enqueue(&it);
}

void store_shutdown(void)
{
    if (!g_started) return;
    pthread_mutex_lock(&g_mtx);
    g_stop = 1;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mtx);
    pthread_join(g_thread, NULL);
    g_started = 0;
}

#else  /* !ENABLE_STORAGE */

int store_init(const char *dir)
{
    (void)dir;
    return 0;
}

void store_event(const char *type, const char *plate, const char *detail)
{
    (void)type; (void)plate; (void)detail;
}

void store_gate(const char *action, const char *source)
{
    (void)action; (void)source;
}

void store_shutdown(void)
{
}

#endif /* ENABLE_STORAGE */
