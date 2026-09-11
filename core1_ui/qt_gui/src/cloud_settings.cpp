#include "cloud_settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>
#include <QUrl>

/* Defaults match PhaseMd/08: DeepSeek-compatible chat endpoint, 5s single-shot
 * request, one retry, cloud called below 0.60 confidence. The prompt is ASCII
 * on purpose: every GUI source file must stay pure ASCII (board rule), and the
 * operator can edit it (Chinese included) from the settings page / conf file. */
CloudSettings cloud_settings_defaults()
{
    CloudSettings s;
    s.apiBase = QStringLiteral("https://api.deepseek.com/chat/completions");
    s.apiKey.clear();
    s.model = QStringLiteral("deepseek-chat");
    s.prompt = QStringLiteral(
        "You are a license plate reader. Look at the image and answer with "
        "JSON only: {\"plate\":\"<plate text>\",\"confidence\":0.00}. "
        "If no plate is readable, use an empty plate and confidence 0. "
        "Do not output anything else.");
    s.triggerConf = 0.60;
    s.acceptConf = 0.50;
    s.timeoutMs = 5000;
    s.retry = 1;
    /* auto: Qt transport first, python3 fallback when Qt cannot complete the
     * request (this board's Qt 5.12 + OpenSSL 1.1.1 stalls on TLS 1.3). */
    s.transport = QStringLiteral("auto");
    s.autoFallback = true;
    s.writeback = true;
    s.insecureTls = false;
    s.fakeResult = false;
    return s;
}

QString cloud_settings_path()
{
    const QByteArray env = qgetenv("PARK_CLOUD_CONF");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    return QStringLiteral("/etc/park/cloud.conf");
}

/* Endpoint host -> provider id. Each provider needs its own API key, and the
 * LCD switches providers by changing the endpoint, so the id is what the
 * per-provider key table is indexed by. */
QString cloud_provider_for_base(const QString &apiBase)
{
    const QString host = QUrl(apiBase).host().toLower();
    if (host.contains(QLatin1String("deepseek")))
        return QStringLiteral("deepseek");
    if (host.contains(QLatin1String("dashscope")) ||
        host.contains(QLatin1String("aliyuncs")))
        return QStringLiteral("dashscope");
    if (host.contains(QLatin1String("openai")))
        return QStringLiteral("openai");
    if (host.isEmpty())
        return QStringLiteral("custom");
    return QStringLiteral("custom");
}

void cloud_settings_set_key(CloudSettings *s, const QString &key)
{
    if (s == nullptr)
        return;
    s->apiKey = key.trimmed();
    s->keys.insert(cloud_provider_for_base(s->apiBase), s->apiKey);
}

bool cloud_settings_adopt_key(CloudSettings *s)
{
    if (s == nullptr)
        return false;
    const QString prov = cloud_provider_for_base(s->apiBase);
    const QMap<QString, QString>::const_iterator it = s->keys.constFind(prov);
    if (it == s->keys.constEnd() || it.value().trimmed().isEmpty()) {
        s->apiKey.clear();
        return false;
    }
    s->apiKey = it.value();
    return true;
}

static QString trim(const QString &s)
{
    return s.trimmed();
}

static bool toBool(const QString &v, bool dflt)
{
    const QString t = v.toLower();
    if (t == QLatin1String("1") || t == QLatin1String("true") ||
        t == QLatin1String("on") || t == QLatin1String("yes"))
        return true;
    if (t == QLatin1String("0") || t == QLatin1String("false") ||
        t == QLatin1String("off") || t == QLatin1String("no"))
        return false;
    return dflt;
}

bool cloud_settings_load(CloudSettings *out, QString *warn, const QString &path)
{
    if (out == nullptr)
        return false;
    *out = cloud_settings_defaults();
    const QString p = path.isEmpty() ? cloud_settings_path() : path;

    QFile f(p);
    if (!f.exists()) {
        if (warn != nullptr)
            *warn = QStringLiteral("no %1 yet, built-in defaults in use").arg(p);
        /* still honour the environment key override */
        const QByteArray envKey = qgetenv("DEEPSEEK_API_KEY");
        if (!envKey.isEmpty())
            out->apiKey = QString::fromLocal8Bit(envKey);
        return false;
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (warn != nullptr)
            *warn = QStringLiteral("cannot read %1").arg(p);
        return false;
    }

    QTextStream ts(&f);
    ts.setCodec("UTF-8");
    while (!ts.atEnd()) {
        const QString line = trim(ts.readLine());
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) ||
            line.startsWith(QLatin1Char(';')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString k = trim(line.left(eq));
        const QString v = trim(line.mid(eq + 1));

        if (k == QLatin1String("api_base"))            out->apiBase = v;
        else if (k == QLatin1String("api_key"))        out->apiKey = v;
        else if (k.startsWith(QLatin1String("key_")))  out->keys.insert(k.mid(4), v);
        else if (k == QLatin1String("model"))          out->model = v;
        else if (k == QLatin1String("prompt"))         out->prompt = v;
        else if (k == QLatin1String("trigger_conf"))   out->triggerConf = v.toDouble();
        else if (k == QLatin1String("accept_conf"))    out->acceptConf = v.toDouble();
        else if (k == QLatin1String("timeout_ms"))     out->timeoutMs = v.toInt();
        else if (k == QLatin1String("retry"))          out->retry = v.toInt();
        else if (k == QLatin1String("ca_file"))        out->caFile = v;
        else if (k == QLatin1String("proxy"))          out->proxy = v;
        else if (k == QLatin1String("transport"))      out->transport = v;
        else if (k == QLatin1String("auto_fallback"))  out->autoFallback = toBool(v, out->autoFallback);
        else if (k == QLatin1String("writeback"))      out->writeback = toBool(v, out->writeback);
        else if (k == QLatin1String("insecure_tls"))   out->insecureTls = toBool(v, out->insecureTls);
        else if (k == QLatin1String("fake_result"))    out->fakeResult = toBool(v, out->fakeResult);
    }
    f.close();

    /* sanitise: never let a bad value break the trigger path (spec 5.5.3.1) */
    QStringList fixes;
    if (!(out->triggerConf > 0.0 && out->triggerConf <= 1.0)) {
        out->triggerConf = 0.60;
        fixes << QStringLiteral("trigger_conf");
    }
    if (!(out->acceptConf >= 0.0 && out->acceptConf <= 1.0)) {
        out->acceptConf = 0.50;
        fixes << QStringLiteral("accept_conf");
    }
    if (out->timeoutMs < 1000 || out->timeoutMs > 30000) {
        out->timeoutMs = 5000;
        fixes << QStringLiteral("timeout_ms");
    }
    if (out->retry < 0 || out->retry > 3) {
        out->retry = 1;
        fixes << QStringLiteral("retry");
    }
    if (out->apiBase.isEmpty())
        out->apiBase = cloud_settings_defaults().apiBase;
    if (out->model.isEmpty())
        out->model = cloud_settings_defaults().model;
    if (out->prompt.isEmpty())
        out->prompt = cloud_settings_defaults().prompt;

    const QByteArray envKey = qgetenv("DEEPSEEK_API_KEY");
    if (out->apiKey.isEmpty() && !envKey.isEmpty())
        out->apiKey = QString::fromLocal8Bit(envKey);

    /* Per-provider keys: the active key also lives in the table, and a file
     * that only carries key_<provider>= still activates the right one. */
    const QString prov = cloud_provider_for_base(out->apiBase);
    if (!out->apiKey.trimmed().isEmpty())
        out->keys.insert(prov, out->apiKey);
    else if (out->keys.contains(prov))
        out->apiKey = out->keys.value(prov);

    if (warn != nullptr && !fixes.isEmpty())
        *warn = QStringLiteral("bad values replaced by defaults: %1")
                    .arg(fixes.join(QLatin1Char(',')));
    return true;
}

bool cloud_settings_save(const CloudSettings &s, const QString &path)
{
    const QString p = path.isEmpty() ? cloud_settings_path() : path;
    const QFileInfo fi(p);
    if (!QDir().mkpath(fi.absolutePath()))
        return false;

    /* keep one generation back */
    if (QFile::exists(p)) {
        const QString bak = p + QStringLiteral(".bak");
        QFile::remove(bak);
        QFile::copy(p, bak);
    }

    QSaveFile out(p);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;
    QTextStream ts(&out);
    ts.setCodec("UTF-8");
    ts << "# EdgeParking cloud fallback settings (PhaseMd step 7)\n";
    ts << "# Written by park_ui. The API key stays here: never commit it.\n";
    ts << "api_base=" << s.apiBase << "\n";
    ts << "api_key=" << s.apiKey << "\n";
    ts << "model=" << s.model << "\n";
    ts << "prompt=" << s.prompt << "\n";
    ts << "trigger_conf=" << QString::number(s.triggerConf, 'f', 2) << "\n";
    ts << "accept_conf=" << QString::number(s.acceptConf, 'f', 2) << "\n";
    ts << "timeout_ms=" << s.timeoutMs << "\n";
    ts << "retry=" << s.retry << "\n";
    ts << "ca_file=" << s.caFile << "\n";
    ts << "proxy=" << s.proxy << "\n";
    ts << "transport=" << s.transport << "\n";
    /* one key per provider: `api_key` is the active one, key_<provider> keeps
     * the others so switching provider on the LCD does not ask again */
    QMap<QString, QString> keys = s.keys;
    const QString cur = cloud_provider_for_base(s.apiBase);
    if (!s.apiKey.trimmed().isEmpty())
        keys.insert(cur, s.apiKey);
    for (QMap<QString, QString>::const_iterator it = keys.constBegin();
         it != keys.constEnd(); ++it) {
        if (it.key().isEmpty() || it.value().isEmpty())
            continue;
        ts << "key_" << it.key() << "=" << it.value() << "\n";
    }
    ts << "auto_fallback=" << (s.autoFallback ? 1 : 0) << "\n";
    ts << "writeback=" << (s.writeback ? 1 : 0) << "\n";
    ts << "insecure_tls=" << (s.insecureTls ? 1 : 0) << "\n";
    ts << "fake_result=" << (s.fakeResult ? 1 : 0) << "\n";
    ts.flush();
    if (!out.commit())
        return false;
    /* The file holds the API key: QSaveFile creates its temp file with the
     * umask default, so force owner-only (0600) after the swap (P7-07). */
    QFile::setPermissions(p, QFile::ReadOwner | QFile::WriteOwner);
    return true;
}

QString cloud_settings_mask_key(const QString &key)
{
    if (key.isEmpty())
        return QStringLiteral("<empty>");
    if (key.size() <= 8)
        return QString(key.size(), QLatin1Char('*'));
    return key.left(4) + QStringLiteral("...") + key.right(4) +
           QStringLiteral(" (%1 chars)").arg(key.size());
}

bool cloud_settings_has_key(const CloudSettings &s)
{
    return !s.apiKey.trimmed().isEmpty();
}
