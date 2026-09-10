/*
 * log.c - unified logger implementation (P4-08).
 *
 * Line format: "HH:MM:SS.mmm LEVEL [tag] message"
 * Every state transition and key business action logs through this
 * module so the run can be replayed from the log (spec 4.4.2).
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static int         g_level = LOG_INFO;
static FILE       *g_file  = NULL;

void log_set_level(int lvl)
{
    if (lvl < LOG_DEBUG) lvl = LOG_DEBUG;
    if (lvl > LOG_ERROR) lvl = LOG_ERROR;
    g_level = lvl;
}

int log_get_level(void)
{
    return g_level;
}

int log_level_from_str(const char *s)
{
    if (s == NULL) return -1;
    if (strcmp(s, "debug") == 0 || strcmp(s, "DEBUG") == 0) return LOG_DEBUG;
    if (strcmp(s, "info")  == 0 || strcmp(s, "INFO")  == 0) return LOG_INFO;
    if (strcmp(s, "warn")  == 0 || strcmp(s, "WARN")  == 0) return LOG_WARN;
    if (strcmp(s, "error") == 0 || strcmp(s, "ERROR") == 0) return LOG_ERROR;
    return -1;
}

const char *log_level_name(int lvl)
{
    switch (lvl) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    default:        return "?";
    }
}

int log_open_file(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return 0;
    if (g_file != NULL) {
        fclose(g_file);
        g_file = NULL;
    }
    g_file = fopen(path, "a");
    if (g_file == NULL)
        return -1;
    setvbuf(g_file, NULL, _IOLBF, 0);   /* line buffered, best effort */
    return 0;
}

void log_close_file(void)
{
    if (g_file != NULL) {
        fclose(g_file);
        g_file = NULL;
    }
}

void log_printf(int lvl, const char *tag, const char *fmt, ...)
{
    struct timeval tv;
    struct tm tmv;
    time_t sec;
    char stamp[32];
    va_list ap;

    if (lvl < g_level)
        return;

    gettimeofday(&tv, NULL);
    sec = tv.tv_sec;
#if defined(_WIN32)
    localtime_s(&tmv, &sec);
#else
    localtime_r(&sec, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &tmv);

    fprintf(stderr, "%s.%03d %s [%s] ", stamp, (int)(tv.tv_usec / 1000),
            log_level_name(lvl), tag != NULL ? tag : "-");

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

    if (g_file != NULL) {
        fprintf(g_file, "%s.%03d %s [%s] ", stamp, (int)(tv.tv_usec / 1000),
                log_level_name(lvl), tag != NULL ? tag : "-");
        va_start(ap, fmt);
        vfprintf(g_file, fmt, ap);
        va_end(ap);
        fputc('\n', g_file);
        if (ferror(g_file)) {          /* storage full / read-only: give up */
            fclose(g_file);
            g_file = NULL;
        }
    }
}
