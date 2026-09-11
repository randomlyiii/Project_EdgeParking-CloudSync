#ifndef CLOUD_SETTINGS_H
#define CLOUD_SETTINGS_H
/* CloudSettings - persistent settings of the step-7 cloud fallback module.
 *
 * Storage: plain ASCII key=value file, default /etc/park/cloud.conf (override
 * with $PARK_CLOUD_CONF, handy for tests). Saved atomically (QSaveFile) with a
 * .bak copy of the previous content kept, so a power cut during a settings
 * change cannot lose the working values.
 *
 * The API key lives in that file only: it must never be committed to git and
 * never printed to the journal (spec P7-07). Always log mask_key() instead.
 * $DEEPSEEK_API_KEY overrides an empty key from the file.
 *
 * One key PER PROVIDER: each endpoint needs its own key, and the operator
 * switches providers from the LCD (the PRESET button moves model+endpoint
 * together). So the file keeps `api_key=` (the active one, used for requests)
 * plus `key_<provider>=` for every provider seen so far; switching provider
 * adopts that provider's stored key automatically. Provider ids come from the
 * endpoint host (deepseek / dashscope / openai / custom).
 *
 * Step-7 spec references: PhaseMd/08 P7-02 (endpoint/model), P7-03 (prompt),
 * P7-04 (timeout/retry), P7-06 (trigger), P7-07 (key handling).
 */
#include <QMap>
#include <QString>

/* Which provider an endpoint belongs to: "deepseek", "dashscope", "openai" or
 * "custom" (unknown host). Keys are stored per provider id. */
QString cloud_provider_for_base(const QString &apiBase);

struct CloudSettings {
    QString apiBase;      /* chat/completions endpoint (any OpenAI-compatible) */
    QString apiKey;       /* secret - active key, never logged/committed */
    QString model;        /* e.g. deepseek-chat, or a custom vision model name */
    QString prompt;       /* instruction sent together with the image */
    double  triggerConf;  /* call the cloud when edge confidence < this */
    double  acceptConf;   /* accept a cloud result when confidence >= this */
    int     timeoutMs;    /* single request timeout (spec: 5000) */
    int     retry;        /* extra attempts after the first (spec: <= 1) */
    QString caFile;       /* TLS CA bundle; "" = auto-detect. The board rootfs
                           * has no ca-certificates.crt, so HTTPS needs either
                           * /etc/park/ca.pem or insecure_tls=1 */
    QString proxy;        /* "" = never use a proxy (default, deterministic);
                           * "env" = follow http(s)_proxy; else http://host:port */
    QString transport;    /* "auto" (default) = Qt first, python3 on failure;
                           * "qt" = Qt only; "python" = python3 only */
    QMap<QString, QString> keys;  /* provider id -> key (secret) */
    bool    autoFallback; /* low confidence / recognition failure -> cloud */
    bool    writeback;    /* manual cloud result is written back to /park_shm */
    bool    insecureTls;  /* skip certificate verification (demo only) */
    bool    fakeResult;   /* no network: answer with a canned plate (demo) */
};

/* Store/refresh a key for the provider of `apiBase`, and adopt it as active. */
void cloud_settings_set_key(CloudSettings *s, const QString &key);

/* Make the stored key of the endpoint's provider active. Returns false when
 * that provider has no key yet (the caller should ask the operator for one). */
bool cloud_settings_adopt_key(CloudSettings *s);

CloudSettings cloud_settings_defaults();

/* Effective config path: $PARK_CLOUD_CONF or /etc/park/cloud.conf */
QString cloud_settings_path();

/* Loads the file; missing file keeps the defaults (returns false).
 * Out-of-range values are clamped and reported through *warn (ASCII text). */
bool cloud_settings_load(CloudSettings *out, QString *warn = nullptr,
                         const QString &path = QString());

/* Atomic save + .bak of the previous content. Never writes the key to logs. */
bool cloud_settings_save(const CloudSettings &s, const QString &path = QString());

/* "sk-abcd...wxyz" style mask for logs and the UI. */
QString cloud_settings_mask_key(const QString &key);

/* True when the key looks configured (non-empty after env override). */
bool cloud_settings_has_key(const CloudSettings &s);

#endif /* CLOUD_SETTINGS_H */
