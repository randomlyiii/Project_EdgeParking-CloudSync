/*
 * log.h - unified logger for the core0 business daemon (P4-08).
 *
 * Level-filtered logging with millisecond wall-clock timestamps.
 * Outputs to stderr (picked up by journald when run under systemd)
 * and optionally to a file (log_open_file). The file is best-effort:
 * on write error it is disabled silently (never blocks the business
 * loop, spec 5.8.3).
 *
 * Pure ASCII / portable (also built by the host selftest).
 */
#ifndef BIZ_LOG_H
#define BIZ_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
};

/* set/get the minimum level that is printed (default LOG_INFO) */
void log_set_level(int lvl);
int  log_get_level(void);

/* "debug"|"info"|"warn"|"error" -> level, -1 if unknown */
int log_level_from_str(const char *s);
const char *log_level_name(int lvl);

/* open an optional log file (append). 0 ok / -1 fail (non-fatal). */
int  log_open_file(const char *path);
void log_close_file(void);

void log_printf(int lvl, const char *tag, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define LOGD(tag, ...) log_printf(LOG_DEBUG, (tag), __VA_ARGS__)
#define LOGI(tag, ...) log_printf(LOG_INFO,  (tag), __VA_ARGS__)
#define LOGW(tag, ...) log_printf(LOG_WARN,  (tag), __VA_ARGS__)
#define LOGE(tag, ...) log_printf(LOG_ERROR, (tag), __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* BIZ_LOG_H */
