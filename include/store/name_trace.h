#ifndef STORE_NAME_TRACE_H
#define STORE_NAME_TRACE_H

#include <QByteArray>
#include <QCryptographicHash>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QString>
#include <QDebug>
#include <cstdio>

namespace StoreNameTrace {

struct TraceEntry {
    QByteArray hash;
    QString name;
    QString stage;
};

inline bool isEnabled() {
    static bool enabled = qEnvironmentVariableIsSet("APPALCHEMIST_TRACE_NAMES");
    return enabled;
}

inline bool matchesFilter(const QString& key, const QString& name) {
    QByteArray rawFilter = qgetenv("APPALCHEMIST_TRACE_FILTER");
    if (rawFilter.isEmpty()) {
        return true;
    }
    QString filter = QString::fromUtf8(rawFilter);
    return key.contains(filter, Qt::CaseInsensitive) || name.contains(filter, Qt::CaseInsensitive);
}

inline QByteArray fingerprint(const QString& name) {
    return QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha1);
}

inline void trace(const char* stage, const QString& key, const QString& name) {
    if (!isEnabled()) {
        return;
    }
    if (!matchesFilter(key, name)) {
        return;
    }
    QString traceKey = key.isEmpty() ? name : key;
    if (traceKey.isEmpty()) {
        return;
    }

    static QMutex mutex;
    QMutexLocker locker(&mutex);
    static QHash<QString, TraceEntry> last;

    QByteArray hash = fingerprint(name);
    auto it = last.find(traceKey);
    if (it == last.end()) {
        last.insert(traceKey, TraceEntry{hash, name, QString::fromUtf8(stage)});
        fprintf(stderr, "[NameTrace] %s key=%s name=%s\n",
                stage,
                traceKey.toUtf8().constData(),
                name.toUtf8().constData());
        fflush(stderr);
        return;
    }

    if (it->hash != hash) {
        fprintf(stderr, "[NameTrace] MISMATCH key=%s from=%s(%s) to=%s(%s)\n",
                traceKey.toUtf8().constData(),
                it->name.toUtf8().constData(),
                it->stage.toUtf8().constData(),
                name.toUtf8().constData(),
                stage);
        fflush(stderr);
        it->hash = hash;
        it->name = name;
        it->stage = QString::fromUtf8(stage);
    }
}

} // namespace StoreNameTrace

#endif // STORE_NAME_TRACE_H

