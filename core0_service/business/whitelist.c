/*
 * whitelist.c - parking whitelist table implementation (P4-03).
 *
 * Exact match on the UTF-8 plate string; expired or non-allowed
 * entries reject with a distinct reason so DENY records can say why
 * (spec 5.3.1.4).
 */
#include "whitelist.h"

#include <stdio.h>
#include <string.h>

void wl_init(whitelist_t *w)
{
    if (w == NULL) return;
    memset(w, 0, sizeof(*w));
}

void wl_clear(whitelist_t *w)
{
    wl_init(w);
}

int wl_count(const whitelist_t *w)
{
    return (w != NULL) ? w->n : 0;
}

int wl_add(whitelist_t *w, const char *plate, int allow, int32_t expiry)
{
    size_t len;

    if (w == NULL || plate == NULL)
        return -1;
    len = strlen(plate);
    if (len == 0 || len >= WL_PLATE_MAX)
        return -1;                      /* empty or would truncate */
    if (w->n >= WL_MAX_ENTRIES)
        return -1;                      /* table full */

    memset(w->e[w->n].plate, 0, WL_PLATE_MAX);
    memcpy(w->e[w->n].plate, plate, len);
    w->e[w->n].allow  = (uint8_t)(allow ? 1 : 0);
    w->e[w->n].expiry = expiry;
    w->n++;
    return 0;
}

wl_match_t wl_match(const whitelist_t *w, const char *plate, int32_t today)
{
    int i;

    if (w == NULL || plate == NULL || plate[0] == '\0')
        return WL_NO_ENTRY;

    for (i = 0; i < w->n; i++) {
        if (strcmp(w->e[i].plate, plate) != 0)
            continue;
        if (!w->e[i].allow)
            return WL_NOT_ALLOWED;
        if (w->e[i].expiry != 0 && today > w->e[i].expiry)
            return WL_EXPIRED;
        return WL_MATCH_OK;
    }
    return WL_NO_ENTRY;
}

const char *wl_match_str(wl_match_t m)
{
    switch (m) {
    case WL_MATCH_OK:    return "whitelist hit";
    case WL_NO_ENTRY:    return "not in whitelist";
    case WL_EXPIRED:     return "whitelist entry expired";
    case WL_NOT_ALLOWED: return "entry not allowed";
    default:             return "unknown";
    }
}

int32_t wl_parse_expiry(const char *s)
{
    int y, m, d;
    int n;

    if (s == NULL)
        return 0;
    /* accept both YYYY-MM-DD and YYYYMMDD */
    n = sscanf(s, "%4d-%2d-%2d", &y, &m, &d);
    if (n != 3) {
        n = sscanf(s, "%8d", &y);
        if (n == 1 && y >= 19000101 && y <= 99991231) {
            d = y % 100;
            m = (y / 100) % 100;
            y = y / 10000;
            if (m < 1 || m > 12 || d < 1 || d > 31)
                return 0;
            return y * 10000 + m * 100 + d;
        }
        return 0;
    }
    if (y < 1900 || y > 9999 || m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    return y * 10000 + m * 100 + d;
}
