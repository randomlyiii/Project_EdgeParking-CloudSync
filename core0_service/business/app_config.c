/*
 * app_config.c - config file parser (P4-05).
 *
 * Simple line based parser: "key = value" in [settings], one plate per
 * line in [whitelist]. UTF-8 plates pass through as opaque bytes.
 */
#include "app_config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_config_defaults(app_config_t *c)
{
    if (c == NULL) return;
    memset(c, 0, sizeof(*c));
    c->total_slots      = 20;
    c->count_mode       = 0;
    c->recog_timeout_ms = 3000;
    c->cloud_timeout_ms = 6000;
    c->cool_down_ms     = 4000;
    c->conf_threshold   = 0.60f;
    c->log_level        = LOG_INFO;
    wl_init(&c->wl);
}

static char *trim(char *s)
{
    char *end;

    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

/* apply one settings key; unknown keys warn, invalid values keep base */
static void apply_kv(app_config_t *out, const app_config_t *base,
                     const char *key, const char *val, int lineno)
{
    if (strcmp(key, "total_slots") == 0) {
        int v = atoi(val);
        if (v >= 1 && v <= 999) out->total_slots = v;
        else { LOGE("config", "line %d: total_slots '%s' out of range 1..999, keep %d",
                    lineno, val, base->total_slots); }
    } else if (strcmp(key, "count_mode") == 0) {
        if (strcmp(val, "entry") == 0) out->count_mode = 0;
        else if (strcmp(val, "exit") == 0) out->count_mode = 1;
        else LOGE("config", "line %d: count_mode '%s' invalid (entry|exit), keep %d",
                  lineno, val, base->count_mode);
    } else if (strcmp(key, "recog_timeout_ms") == 0) {
        int v = atoi(val);
        if (v >= 500 && v <= 60000) out->recog_timeout_ms = v;
        else LOGE("config", "line %d: recog_timeout_ms '%s' out of range, keep %d",
                  lineno, val, base->recog_timeout_ms);
    } else if (strcmp(key, "cloud_timeout_ms") == 0) {
        int v = atoi(val);
        if (v >= 500 && v <= 60000 && v >= out->recog_timeout_ms)
            out->cloud_timeout_ms = v;
        else LOGE("config", "line %d: cloud_timeout_ms '%s' invalid, keep %d",
                  lineno, val, base->cloud_timeout_ms);
    } else if (strcmp(key, "cool_down_ms") == 0) {
        int v = atoi(val);
        if (v >= 1000 && v <= 60000) out->cool_down_ms = v;
        else LOGE("config", "line %d: cool_down_ms '%s' out of range, keep %d",
                  lineno, val, base->cool_down_ms);
    } else if (strcmp(key, "conf_threshold") == 0) {
        float v = (float)atof(val);
        if (v >= 0.0f && v <= 1.0f) out->conf_threshold = v;
        else LOGE("config", "line %d: conf_threshold '%s' outside [0,1], keep %.2f",
                  lineno, val, base->conf_threshold);
    } else if (strcmp(key, "log_level") == 0) {
        int v = log_level_from_str(val);
        if (v >= 0) out->log_level = v;
        else LOGE("config", "line %d: log_level '%s' unknown, keep %s",
                  lineno, val, log_level_name(base->log_level));
    } else if (strcmp(key, "log_file") == 0) {
        snprintf(out->log_file, sizeof(out->log_file), "%s", val);
    } else if (strcmp(key, "storage_dir") == 0) {
        snprintf(out->storage_dir, sizeof(out->storage_dir), "%s", val);
    } else {
        LOGW("config", "line %d: unknown key '%s' ignored", lineno, key);
    }
}

int app_config_load(const app_config_t *base, app_config_t *out,
                    const char *path, char *errbuf, size_t errlen)
{
    FILE *fp;
    char line[512];
    int  lineno = 0;
    int  section = 0;             /* 0 = settings, 1 = whitelist */

    if (errbuf != NULL && errlen > 0) errbuf[0] = '\0';

    /* start from base: on any problem the base value survives */
    *out = *base;

    if (path == NULL || path[0] == '\0') {
        snprintf(errbuf != NULL ? errbuf : line,
                 errlen > 0 ? errlen : sizeof(line), "no config path given");
        LOGW("config", "no config path given: built-in defaults in effect");
        return 0;
    }
    fp = fopen(path, "r");
    if (fp == NULL) {
        snprintf(errbuf != NULL ? errbuf : line,
                 errlen > 0 ? errlen : sizeof(line), "cannot open %s", path);
        LOGW("config", "cannot open %s: %s", path,
             "using previous/default config (blank whitelist)");
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *s = line;
        lineno++;

        if (strchr(s, '\n') == NULL && !feof(fp)) {
            /* overlong line: skip rest */
            int ch;
            while ((ch = fgetc(fp)) != '\n' && ch != EOF) { }
        }
        s = trim(s);
        if (s[0] == '\0' || s[0] == '#' || s[0] == ';')
            continue;
        if (s[0] == '[') {
            char *close = strchr(s, ']');
            if (close == NULL) continue;
            *close = '\0';
            if (strcmp(s + 1, "settings") == 0) section = 0;
            else if (strcmp(s + 1, "whitelist") == 0) section = 1;
            else section = 2;    /* unknown section: ignore its lines */
            continue;
        }

        if (section == 0) {
            char *eq = strchr(s, '=');
            char *key, *val;
            if (eq == NULL) {
                LOGW("config", "line %d: '%s' is not key=value, skipped",
                     lineno, s);
                continue;
            }
            *eq = '\0';
            key = trim(s);
            val = trim(eq + 1);
            apply_kv(out, base, key, val, lineno);
        } else if (section == 1) {
            /* "<plate> [allow|deny] [YYYY-MM-DD]" - plate is opaque UTF-8 */
            char  buf[512];
            char *tok, *save = NULL;
            char *plate;
            int   allow = 1;
            int32_t expiry = 0;

            snprintf(buf, sizeof(buf), "%s", s);
            tok = strtok_r(buf, " \t", &save);
            if (tok == NULL) continue;
            plate = tok;
            tok = strtok_r(NULL, " \t", &save);
            if (tok != NULL &&
                (strcmp(tok, "allow") == 0 || strcmp(tok, "deny") == 0)) {
                allow = (strcmp(tok, "allow") == 0);
                tok = strtok_r(NULL, " \t", &save);
            }
            if (tok != NULL)
                expiry = wl_parse_expiry(tok);
            if (wl_add(&out->wl, plate, allow, expiry) != 0)
                LOGE("config", "line %d: whitelist entry '%s' rejected "
                     "(full table or plate too long)", lineno, plate);
        }
    }
    fclose(fp);
    return 0;
}
