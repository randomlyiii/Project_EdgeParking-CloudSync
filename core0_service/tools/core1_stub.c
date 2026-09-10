/*
 * core1_stub.c - board-side stub that plays the Core1 role over
 * /park_shm for step-4 acceptance (P4-06 / G4) before the real Qt
 * core1 is wired (step 6).
 *
 * What it does:
 *   - attaches /park_shm read-write (core0 must already be running)
 *   - heartbeats hb_core1 once per second (stop with 'h' toggle)
 *   - commands (stdin):
 *       r <plate> [conf]   post a recognition result (result_valid=1)
 *       o                   pulse req_gate_open  (remote open)
 *       c                   pulse req_gate_close (remote close)
 *       p                   print current core0-published state
 *       h                   toggle heartbeat on/off (test watchdog)
 *       x                   exit
 *
 * Build on board:  gcc -O2 -o core1_stub tools/core1_stub.c \
 *                       -Iipc_shm
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include "park_shm.h"

static park_shm_t *attach(void)
{
    int fd = shm_open("/park_shm", O_RDWR, 0);
    struct stat st;
    void *p;

    if (fd < 0) {
        printf("shm_open /park_shm failed: %s (core0 running?)\n",
               strerror(errno));
        return NULL;
    }
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(park_shm_t)) {
        printf("shm too small (%d bytes)\n", (int)st.st_size);
        close(fd);
        return NULL;
    }
    p = mmap(NULL, sizeof(park_shm_t), PROT_READ | PROT_WRITE,
             MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        printf("mmap failed: %s\n", strerror(errno));
        return NULL;
    }
    if (((park_shm_t *)p)->magic != PARK_SHM_MAGIC) {
        printf("bad magic (0x%08x)\n", ((park_shm_t *)p)->magic);
        munmap(p, sizeof(park_shm_t));
        return NULL;
    }
    if (((park_shm_t *)p)->version != PARK_SHM_VERSION) {
        printf("version mismatch: shm=%u expected=%u\n",
               ((park_shm_t *)p)->version, PARK_SHM_VERSION);
        munmap(p, sizeof(park_shm_t));
        return NULL;
    }
    return (park_shm_t *)p;
}

/* tear-free read of the whole struct (seq double read, same as Qt) */
static int read_shm(park_shm_t *s, park_shm_t *out)
{
    int t;
    for (t = 0; t < 8; t++) {
        uint32_t s1 = s->seq;
        memcpy(out, s, sizeof(*out));
        uint32_t s2 = s->seq;
        if (s1 == s2 && s1 != 0)
            return 0;
        usleep(1000);
    }
    return -1;
}

static void print_state(const park_shm_t *p)
{
    printf("free=%d used=%d gate=%s link=0x%02x fault=0x%02x "
           "recog_pending=%u thr=%.2f plate='%s' conf=%.2f src=%u\n",
           p->free_slots, p->used_slots,
           p->gate_state ? "OPEN" : "CLOSE",
           p->link_flags, p->fault_bits, p->recog_pending,
           p->conf_threshold, p->plate, p->confidence, p->result_source);
}

int main(void)
{
    park_shm_t *s = attach();
    park_shm_t snap;
    struct pollfd pfd;
    uint32_t hb = 0;
    int hb_on = 1;
    int64_t next_hb = 0;
    char line[128];

    if (s == NULL)
        return 1;
    printf("core1_stub attached to /park_shm (v%u)\n",
           (unsigned)s->version);
    printf("commands: r <plate> [conf] | o | c | k | p | h | x\n");

    for (;;) {
        int64_t now;
        struct timespec ts;
        int timeout;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

        if (hb_on && now >= next_hb) {
            hb++;
            s->hb_core1 = hb;            /* core1-owned field: plain store */
            next_hb = now + 1000;
        }

        timeout = hb_on ? (int)(next_hb - now) : 500;
        if (timeout < 0) timeout = 0;

        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, timeout) <= 0 || !(pfd.revents & POLLIN))
            continue;

        if (fgets(line, sizeof(line), stdin) == NULL)
            break;
        {
            char *cmd = strtok(line, " \t\r\n");
            if (cmd == NULL)
                continue;
            if (strcmp(cmd, "x") == 0)
                break;
            if (strcmp(cmd, "p") == 0) {
                if (read_shm(s, &snap) == 0)
                    print_state(&snap);
                else
                    printf("torn read\n");
            } else if (strcmp(cmd, "h") == 0) {
                hb_on = !hb_on;
                printf("heartbeat %s\n", hb_on ? "on" : "off");
            } else if (strcmp(cmd, "o") == 0) {
                s->req_gate_open = 1;    /* pulse; core0 clears it */
                printf("req_gate_open pulsed\n");
            } else if (strcmp(cmd, "c") == 0) {
                s->req_gate_close = 1;
                printf("req_gate_close pulsed\n");
            } else if (strcmp(cmd, "r") == 0) {
                char *plate = strtok(NULL, " \t\r\n");
                char *conf = strtok(NULL, " \t\r\n");
                if (plate == NULL) {
                    printf("usage: r <plate> [conf]\n");
                    continue;
                }
                /* writer order: fields first, valid flag last */
                snprintf((char *)s->plate, sizeof(s->plate), "%s", plate);
                s->confidence =
                    (float)(conf != NULL ? atof(conf) : 0.90);
                s->result_source = 0;    /* edge */
                __sync_synchronize();
                s->result_valid = 1;
                /* v3 cross-process event word: bit first, then the seq */
                s->evt_bits_c1 |= (uint8_t)PARK_EVB_RESULT;
                __sync_synchronize();
                s->evt_seq_c1++;
                printf("result posted: '%s'\n", plate);
            } else if (strcmp(cmd, "k") == 0) {
                /* toggle cloud_pending: verifies the 3s -> 6s timeout
                 * extension on hardware (P4-02 rule 2) */
                s->cloud_pending = (uint8_t)(s->cloud_pending ? 0 : 1);
                printf("cloud_pending=%u\n", (unsigned)s->cloud_pending);
            } else {
                printf("unknown command '%s'\n", cmd);
            }
        }
    }

    munmap(s, sizeof(park_shm_t));
    return 0;
}
