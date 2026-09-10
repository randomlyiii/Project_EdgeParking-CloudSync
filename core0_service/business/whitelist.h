/*
 * whitelist.h - parking whitelist table (P4-03).
 *
 * In-memory table loaded from the [whitelist] section of the local
 * config file. Matching is exact (byte compare of the UTF-8 plate).
 * Entries may carry an optional expiry day (YYYYMMDD, inclusive) and
 * an optional permission flag (allow=1 default).
 *
 * Pure ASCII / portable (also built by the host selftest).
 */
#ifndef BIZ_WHITELIST_H
#define BIZ_WHITELIST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WL_MAX_ENTRIES 64
#define WL_PLATE_MAX   16     /* == park_shm_t.plate, incl. NUL */

typedef struct {
    char    plate[WL_PLATE_MAX];   /* exact match key, UTF-8 */
    uint8_t allow;                 /* 1 = may pass (default), 0 = blocked */
    int32_t expiry;                /* YYYYMMDD inclusive, 0 = never */
} wl_entry_t;

typedef struct {
    wl_entry_t e[WL_MAX_ENTRIES];
    int        n;
} whitelist_t;

typedef enum {
    WL_MATCH_OK = 0,
    WL_NO_ENTRY,
    WL_EXPIRED,
    WL_NOT_ALLOWED
} wl_match_t;

void        wl_init(whitelist_t *w);
/* returns 0 ok, -1 if table full or plate empty/too long */
int         wl_add(whitelist_t *w, const char *plate, int allow, int32_t expiry);
void        wl_clear(whitelist_t *w);
int         wl_count(const whitelist_t *w);

/* today = YYYYMMDD of the decision moment */
wl_match_t  wl_match(const whitelist_t *w, const char *plate, int32_t today);
const char *wl_match_str(wl_match_t m);

/* helper: parse "YYYY-MM-DD" or "YYYYMMDD" -> YYYYMMDD, 0 on failure */
int32_t wl_parse_expiry(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* BIZ_WHITELIST_H */
