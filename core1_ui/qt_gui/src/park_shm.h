#ifndef PARK_SHM_H
#define PARK_SHM_H
/* park_shm_t - dual A7 shared memory layout, defined by PhaseMd/07 P6-01.
 * Writer  : core0_service/ipc_shm (shm_open("/park_shm"), P6-01)
 * Reader  : core1_ui/qt_gui (this copy)
 * NOTE    : when P6-01 lands in core0_service, replace the two copies with one
 *           common header; field order here must stay in sync until then.
 * Consistency: writer bumps seq before/after update; reader copies whole struct
 * and re-reads seq, retries while unequal (torn-read protection).
 */
#include <stdint.h>

#define PARK_SHM_MAGIC   0x5041524BU  /* "PARK" */
#define PARK_SHM_VERSION 1U

typedef struct {
    uint32_t magic, version;
    volatile uint32_t seq;            /* writer increments; see note above */
    /* --- core0 -> core1 (business/UI data) --- */
    int32_t  free_slots, used_slots;
    uint8_t  gate_state;              /* 0 closed 1 open */
    uint8_t  link_flags;              /* bit0 rpmsg bit1 m4-online bit2 core1-online */
    uint8_t  recog_pending;           /* car waiting for recognition */
    /* --- core1 -> core0 (recognition result) --- */
    uint8_t  cloud_pending;           /* cloud fallback in progress */
    uint8_t  result_valid;            /* cleared by core0 after read */
    uint8_t  result_source;           /* 0=edge 1=cloud */
    float    confidence;
    char     plate[16];               /* UTF-8, province char + alnum */
    /* --- heartbeat --- */
    volatile uint32_t hb_core1;       /* core1 +1 per second, core0 checks 3s */
} park_shm_t;

/* eventfd codes (PhaseMd/07 P6-02) */
#define PARK_EVT_TRIGGER     0x01   /* core0->core1 trigger recognition */
#define PARK_EVT_RESULT      0x02   /* core1->core0 result ready */
#define PARK_EVT_STATE       0x03   /* core0->core1 state change (UI refresh) */
#define PARK_EVT_RESYNC      0x04   /* core0->core1 resync request */

#endif /* PARK_SHM_H */
