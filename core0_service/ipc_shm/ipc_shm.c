/*
 * ipc_shm.c - /park_shm writer + eventfd helpers implementation.
 */
#include "ipc_shm.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int ipc_shm_create(park_shm_t **out)
{
    int fd;
    park_shm_t *p;

    if (out == NULL)
        return -1;
    *out = NULL;

    fd = shm_open(PARK_SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        LOGE("shm", "shm_open(%s) failed: %s", PARK_SHM_NAME, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)sizeof(park_shm_t)) != 0) {
        LOGE("shm", "ftruncate failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    p = (park_shm_t *)mmap(NULL, sizeof(park_shm_t),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        LOGE("shm", "mmap failed: %s", strerror(errno));
        return -1;
    }

    /* fresh state each start: zero everything, then stamp identity */
    memset(p, 0, sizeof(*p));
    p->magic = PARK_SHM_MAGIC;
    p->version = PARK_SHM_VERSION;
    p->seq = 0;
    __sync_synchronize();
    LOGI("shm", "%s created, size=%d bytes, version=%u",
         PARK_SHM_NAME, (int)sizeof(park_shm_t), (unsigned)PARK_SHM_VERSION);
    *out = p;
    return 0;
}

void ipc_shm_publish(park_shm_t *s, const park_shm_t *fields)
{
    if (s == NULL || fields == NULL)
        return;

    s->seq++;                       /* begin update */
    __sync_synchronize();

    /* core0-owned fields only (see header comment) */
    s->free_slots     = fields->free_slots;
    s->used_slots     = fields->used_slots;
    s->gate_state     = fields->gate_state;
    s->link_flags     = fields->link_flags;
    s->recog_pending  = fields->recog_pending;
    s->fault_bits     = fields->fault_bits;
    s->conf_threshold = fields->conf_threshold;
    memcpy((void *)s->plate, fields->plate, sizeof(s->plate));
    s->confidence     = fields->confidence;
    s->result_source  = fields->result_source;

    __sync_synchronize();
    s->seq++;                       /* end update */
}

int ipc_shm_take_result(park_shm_t *s, char *plate, size_t cap,
                        float *conf, uint8_t *source)
{
    if (s == NULL || plate == NULL || cap == 0)
        return -1;
    if (!s->result_valid)
        return 0;

    /* read fields first, the valid flag last (release order by writer);
     * copy before clearing to survive a concurrent writer */
    {
        char tmp[16];
        memcpy(tmp, (const char *)s->plate, sizeof(tmp));
        tmp[sizeof(tmp) - 1] = '\0';
        if (conf != NULL) *conf = s->confidence;
        if (source != NULL) *source = s->result_source;
        snprintf(plate, cap, "%s", tmp);
    }
    s->result_valid = 0;
    return 1;
}

int ipc_shm_take_requests(park_shm_t *s, uint8_t *req_open, uint8_t *req_close)
{
    uint8_t o, c;

    if (s == NULL)
        return 0;
    o = s->req_gate_open;
    c = s->req_gate_close;
    if (o) s->req_gate_open = 0;
    if (c) s->req_gate_close = 0;
    if (req_open)  *req_open = o;
    if (req_close) *req_close = c;
    return (o || c) ? 1 : 0;
}

int ipc_evt_create(void)
{
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0)
        LOGE("shm", "eventfd create failed: %s", strerror(errno));
    return fd;
}

int ipc_evt_notify(int fd, uint64_t code)
{
    ssize_t w;

    if (fd < 0)
        return -1;
    do {
        w = write(fd, &code, sizeof(code));
    } while (w < 0 && errno == EINTR);
    return (w == (ssize_t)sizeof(code)) ? 0 : -1;
}

void ipc_evt_drain(int fd)
{
    uint64_t n;
    while (read(fd, &n, sizeof(n)) == (ssize_t)sizeof(n)) {
        /* consume accumulated counter */
    }
}

/* ---- v3 cross-process event channel ---- */

void ipc_shm_evt_notify(park_shm_t *s, uint8_t code)
{
    if (s == NULL || code == 0)
        return;
    /* set the bit first, then publish the seq (consumer polls the seq and
     * only then reads the bits -> no lost or half-published event) */
    s->evt_bits_c0 |= (uint8_t)PARK_EVB(code);
    __sync_synchronize();
    s->evt_seq_c0++;
}

uint8_t ipc_shm_evt_take_c1(park_shm_t *s)
{
    uint8_t bits;

    if (s == NULL)
        return 0;
    bits = s->evt_bits_c1;
    if (bits != 0) {
        s->evt_bits_c1 = 0;
        __sync_synchronize();
    }
    return bits;
}

uint8_t ipc_shm_evt_peek_c0(const park_shm_t *s)
{
    return (s == NULL) ? 0 : s->evt_bits_c0;
}
