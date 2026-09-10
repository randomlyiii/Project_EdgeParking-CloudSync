/*
 * app_config.h - local config file: the single source of truth for all
 * tunable business parameters (P4-05, spec 5.5).
 *
 * File format (see core0.conf.example):
 *   [settings]  key = value lines
 *   [whitelist] one plate per line: "<plate> [allow|deny] [YYYY-MM-DD]"
 *
 * Hot reload: the daemon stats the file periodically; on change it is
 * re-parsed over a copy of the current config. Invalid values are
 * rejected (previous value kept) with an ERROR log; a wholly
 * unreadable file keeps the previous config entirely (spec 5.5.3).
 *
 * Pure ASCII / portable (also built by the host selftest).
 */
#ifndef BIZ_APP_CONFIG_H
#define BIZ_APP_CONFIG_H

#include <stddef.h>      /* size_t (app_config_load) */

#include "whitelist.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int   total_slots;        /* 1..999,                       default 20    */
    int   count_mode;         /* 0=entry(+1) 1=exit(-1),       default entry */
    int   recog_timeout_ms;   /* 500..60000,                   default 3000  */
    int   cloud_timeout_ms;   /* recog..60000 hard cap,        default 6000  */
    int   cool_down_ms;       /* 1000..60000,                  default 4000  */
    float conf_threshold;     /* 0.0..1.0,                     default 0.60  */
    int   log_level;          /* LOG_* enum,                   default INFO  */
    char  log_file[128];      /* optional log file,            default none  */
    char  storage_dir[128];   /* used with ENABLE_STORAGE,     default ""    */
    whitelist_t wl;           /* [whitelist] section                         */
} app_config_t;

void app_config_defaults(app_config_t *c);

/*
 * Parse 'path' over 'base' into 'out'.
 * Returns 0 on success (missing file counts as success with a warning
 * in errbuf -> defaults/blank whitelist at startup, spec 5.3.3.1),
 * -1 when the file exists but cannot be opened (hot reload keeps the
 * previous config in that case). Invalid values keep the base value
 * and are reported through errbuf/ERROR log.
 */
int app_config_load(const app_config_t *base, app_config_t *out,
                    const char *path, char *errbuf, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* BIZ_APP_CONFIG_H */
