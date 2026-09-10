/*
 * ipc_shm.h - /park_shm writer + eventfd helpers (P4-05/P6-01 base).
 *
 * core0 owns the shm object: it creates /park_shm, initializes it and
 * publishes its business fields under seq protection. Fields owned by
 * core1 (result, requests, heartbeat) are only read/cleared here.
 *
 * eventfd direction core0->core1 (0x01/0x03/0x04): in P4 the fd is
 * usually not wired yet, so notify is a no-op then (spec 5.1.1.5).
 *
 * Linux only (shm_open/eventfd) - not part of the host selftest.
 */
#ifndef BIZ_IPC_SHM_H
#define BIZ_IPC_SHM_H

#include <stdint.h>
#include <stddef.h>

#include "park_shm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PARK_SHM_NAME "/park_shm"

/* create + initialize /park_shm (fresh state each start).
 * returns 0 ok, -1 fail (*out untouched). */
int  ipc_shm_create(park_shm_t **out);

/*
 * Publish core0-owned fields from 'fields' into shm under seq
 * protection (seq++ ... copy ... seq++). core1-owned fields
 * (cloud_pending, result_*, req_*, hb_core1) are not touched.
 */
void ipc_shm_publish(park_shm_t *s, const park_shm_t *fields);

/* if result_valid: copy plate/conf/source, clear result_valid, 1.
 * returns 0 when no result pending, -1 on bad args. */
int  ipc_shm_take_result(park_shm_t *s, char *plate, size_t cap,
                         float *conf, uint8_t *source);

/* fetch and clear remote gate request pulses; returns 1 when any
 * request was pending (pulse semantics, spec 6.6). */
int  ipc_shm_take_requests(park_shm_t *s, uint8_t *req_open, uint8_t *req_close);

/* ---- eventfd (core0 -> core1 direction; the fd may be inherited by
 *      the core1 process via launcher/systemd at P6) ---- */
int  ipc_evt_create(void);
int  ipc_evt_notify(int fd, uint64_t code);   /* 0 ok */
void ipc_evt_drain(int fd);

/* ---- v3 cross-process event channel (the one that actually works) ----
 * An anonymous eventfd cannot be shared by two independent processes, so
 * event codes are also published through the shm words evt_bits_c0/seq_c0.
 * Producers set the bit then bump the seq; consumers poll the seq and act
 * once per change. Callers may use this *and* the eventfd (same-process
 * fast path); the two are independent. */
void    ipc_shm_evt_notify(park_shm_t *s, uint8_t code);  /* core0 -> core1 */
/* returns the pending core1 -> core0 bitmask and clears it (0 = none). */
uint8_t ipc_shm_evt_take_c1(park_shm_t *s);
/* returns the pending core0 -> core1 bitmask (does not clear). */
uint8_t ipc_shm_evt_peek_c0(const park_shm_t *s);

#ifdef __cplusplus
}
#endif

#endif /* BIZ_IPC_SHM_H */
