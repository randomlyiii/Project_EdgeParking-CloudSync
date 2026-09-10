#ifndef PARK_SHM_H
#define PARK_SHM_H
/* park_shm_t - dual A7 shared memory layout (PhaseMd/07 P6-01).
 * Writer  : core0_service/ipc_shm (shm_open("/park_shm"))
 * Reader  : core1_ui/qt_gui (its copy: core1_ui/qt_gui/src/park_shm.h)
 * NOTE    : two copies until P6 unification; keep both files IDENTICAL
 *           (the static asserts below fire if the layouts drift apart).
 * Consistency: writer bumps seq before/after update; reader copies the whole
 *           struct and re-reads seq, retrying while unequal (torn read).
 *
 * Version history
 *   v1 2026-09-07  initial 52B layout (through hb_core1)
 *   v2 2026-09-10  appended req_gate_open / req_gate_close / fault_bits /
 *                  conf_threshold (P4-05: UI remote gate + threshold sync)
 *   v3 2026-09-11  appended the cross-process event channel (evt_*), see the
 *                  field comment. Both processes must be rebuilt together:
 *                  the reader rejects a version mismatch by design.
 */
#include <stddef.h>
#include <stdint.h>

#define PARK_SHM_MAGIC   0x5041524BU  /* "PARK" */
#define PARK_SHM_VERSION 3U

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
    /* --- v2 additions (appended; do not reorder fields above) --- */
    volatile uint8_t req_gate_open;   /* pulse: core1 writes 1, core0 clears after exec */
    volatile uint8_t req_gate_close;  /* pulse: core1 writes 1, core0 clears after exec */
    volatile uint8_t fault_bits;      /* fault word, core0 business spec 6.4 */
    float    conf_threshold;          /* recog confidence threshold, core0 authoritative */
    /* --- v3 additions (appended; do not reorder fields above) ---
     * Cross-process event channel. eventfd(2) cannot be shared between two
     * independent processes: an anonymous fd is not openable by core1, and
     * EFD_CLOEXEC drops it across exec. The event codes therefore travel
     * through these words: the producer sets evt_bits_c* then bumps
     * evt_seq_c*; the consumer polls the seq and acts once per change
     * (<=200ms at the reader's tick, inside the spec 4.1.3 budget of 500ms).
     * Bit value for code C is PARK_EVB(C). */
    volatile uint32_t evt_seq_c0;     /* core0 bumps after setting evt_bits_c0 */
    volatile uint8_t  evt_bits_c0;    /* pending events core0 -> core1 (bitmask) */
    volatile uint32_t evt_seq_c1;     /* core1 bumps after setting evt_bits_c1 */
    volatile uint8_t  evt_bits_c1;    /* pending events core1 -> core0 (bitmask) */
} park_shm_t;

/* event codes (PhaseMd/07 P6-02; used as bit positions in the v3 channel) */
#define PARK_EVT_TRIGGER     0x01   /* core0->core1 trigger recognition */
#define PARK_EVT_RESULT      0x02   /* core1->core0 result ready */
#define PARK_EVT_STATE       0x03   /* core0->core1 state change (UI refresh) */
#define PARK_EVT_RESYNC      0x04   /* core0->core1 resync request */

/* v3 bitmask helpers: bit set == that code is pending */
#define PARK_EVB(code)       (1u << (uint32_t)(code))
#define PARK_EVB_TRIGGER     PARK_EVB(PARK_EVT_TRIGGER)   /* 0x02 */
#define PARK_EVB_RESULT      PARK_EVB(PARK_EVT_RESULT)    /* 0x04 */
#define PARK_EVB_STATE       PARK_EVB(PARK_EVT_STATE)     /* 0x08 */
#define PARK_EVB_RESYNC      PARK_EVB(PARK_EVT_RESYNC)    /* 0x10 */

/* Layout guards: compile-time proof that both copies agree and that no
 * compiler inserted different padding (this is the bug class that made the
 * v1 reader reject the v2 writer: PARK_SHM_VERSION drifted silently). */
#if defined(__cplusplus)
  #define PARK_SHM_ASSERT(cond, tag) static_assert(cond, #tag)
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
  #define PARK_SHM_ASSERT(cond, tag) _Static_assert(cond, #tag)
#else
  #define PARK_SHM_ASSERT(cond, tag) \
      typedef char park_shm_assert_##tag[(cond) ? 1 : -1]
#endif
PARK_SHM_ASSERT(sizeof(park_shm_t) == 76, park_shm_size_is_76);
PARK_SHM_ASSERT(offsetof(park_shm_t, hb_core1) == 48, hb_core1_at_48);
PARK_SHM_ASSERT(offsetof(park_shm_t, req_gate_open) == 52, req_gate_open_at_52);
PARK_SHM_ASSERT(offsetof(park_shm_t, conf_threshold) == 56, conf_threshold_at_56);
PARK_SHM_ASSERT(offsetof(park_shm_t, evt_seq_c0) == 60, evt_seq_c0_at_60);
PARK_SHM_ASSERT(offsetof(park_shm_t, evt_seq_c1) == 68, evt_seq_c1_at_68);

#endif /* PARK_SHM_H */
