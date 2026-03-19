#include "store/store_indexer.h"
#include <QDateTime>

StoreIndexer::StoreIndexer(StoreRepository* repository, QObject* parent)
    : QObject(parent)
    , m_repository(repository)
    , m_initialized(false) {}

void StoreIndexer::initialize() {
    if (m_initialized) {
        return;
    }
    m_baseApps = m_repository->loadAppStreamApps();
    m_byId.clear();
    for (const StoreAppEntry& app : m_baseApps) {
        QString key = !app.appId.isEmpty() ? app.appId : app.packageName;
        if (!key.isEmpty()) {
            m_byId.insert(key, app);
        }
    }
    buildCollections();
    m_initialized = true;
}

QStringList StoreIndexer::categories() const {
    QSet<QString> unique;
    for (const StoreAppEntry& app : m_baseApps) {
        for (const QString& cat : app.categories) {
            if (!cat.isEmpty()) {
                unique.insert(cat);
            }
        }
    }
    QStringList list = unique.values();
    std::sort(list.begin(), list.end());
    return list;
}

QList<StoreAppEntry> StoreIndexer::search(const QString& query) const {
    if (query.trimmed().isEmpty()) {
        return m_baseApps;
    }
    QList<StoreAppEntry> results;
    for (const StoreAppEntry& app : m_baseApps) {
        if (matchesQuery(app, query)) {
            results.append(app);
        }
    }
    return results;
}

QList<StoreAppEntry> StoreIndexer::searchWithFallback(const QString& query) const {
    QList<StoreAppEntry> results = search(query);
    if (!results.isEmpty()) {
        return results;
    }
    return m_repository->searchRepositories(query);
}

QList<StoreAppEntry> StoreIndexer::appsByCategory(const QString& category) const {
    if (category.isEmpty()) {
        return m_baseApps;
    }
    QList<StoreAppEntry> results;
    for (const StoreAppEntry& app : m_baseApps) {
        if (app.categories.contains(category)) {
            results.append(app);
        }
    }
    return results;
}

QList<StoreAppEntry> StoreIndexer::byCollection(StoreCollectionType type) const {
    if (type == StoreCollectionType::All) {
        return m_baseApps;
    }
    if (type == StoreCollectionType::Popular) {
        return m_popularApps;
    }
    if (type == StoreCollectionType::New) {
        return m_newApps;
    }
    return m_recommendedApps;
}

QString StoreIndexer::normalizeToken(const QString& value) const {
    // Work with bytes to avoid QString corruption
    QByteArray bytes = value.toLower().simplified().toUtf8();
    QByteArray result;
    result.reserve(bytes.size());
    
    for (int i = 0; i < bytes.size(); ++i) {
        char ch = bytes.at(i);
        if (ch == '-') {
            result.append(' ');
        } else {
            result.append(ch);
        }
    }
    
    return QString::fromUtf8(result);
}

bool StoreIndexer::matchesQuery(const StoreAppEntry& app, const QString& query) const {
    QString q = normalizeToken(query);
    if (q.isEmpty()) return true;
    const QString name = normalizeToken(app.effectiveName());
    const QString pkg = normalizeToken(app.packageName);
    const QString summary = normalizeToken(app.summary);
    const QString desc = normalizeToken(app.description);
    if (name.contains(q) || pkg.contains(q) || summary.contains(q) || desc.contains(q)) {
        return true;
    }
    for (const QString& cat : app.categories) {
        if (normalizeToken(cat).contains(q)) {
            return true;
        }
    }
    return false;
}

bool StoreIndexer::isPopular(const StoreAppEntry& app) const {
    const QStringList list = popularList();
    const QString name = normalizeToken(app.effectiveName());
    const QString pkg = normalizeToken(app.packageName);
    for (const QString& item : list) {
        QString token = normalizeToken(item);
        if (name == token || pkg == token || name.contains(token) || pkg.contains(token)) {
            return true;
        }
    }
    return false;
}

bool StoreIndexer::isRecommended(const StoreAppEntry& app) const {
    return !app.summary.isEmpty() && !app.categories.isEmpty() &&
           (!app.iconKey.isEmpty() || !app.iconPath.isEmpty());
}

bool StoreIndexer::isNew(const StoreAppEntry& app) const {
    if (app.releaseDate.isValid()) {
        return app.releaseDate > QDateTime::currentDateTime().addMonths(-6);
    }
    return false;
}

QStringList StoreIndexer::popularList() const {
    return {
        "org.mozilla.firefox",
        "org.chromium.Chromium",
        "org.libreoffice.LibreOffice",
        "org.videolan.VLC",
        "org.gimp.GIMP",
        "org.kde.krita",
        "org.blender.Blender",
        "org.kde.kdenlive",
        "org.mozilla.Thunderbird",
        "com.valvesoftware.Steam",
        "com.discordapp.Discord",
        "org.telegram.desktop",
        "com.visualstudio.code",
        "md.obsidian.Obsidian",
        "org.gnome.Settings",
        "org.gnome.Software",
        "org.kde.dolphin"
    };
}

QStringList StoreIndexer::keysForApp(const StoreAppEntry& app) const {
    QStringList keys;
    if (!app.appId.isEmpty()) keys << app.appId;
    if (!app.desktopId.isEmpty()) keys << app.desktopId;
    if (!app.packageName.isEmpty()) keys << app.packageName;
    QString name = app.effectiveName();
    if (!name.isEmpty()) keys << name;
    for (QString& key : keys) {
        key = normalizeToken(key);
    }
    keys.removeDuplicates();
    return keys;
}

void StoreIndexer::buildCollections() {
    m_popularApps = buildPopular();
    m_newApps = buildNew();
    m_recommendedApps = buildRecommended();
}

QList<StoreAppEntry> StoreIndexer::buildPopular() const {
    QList<StoreAppEntry> results;
    QSet<QString> seen;
    QStringList targets = popularList();
    for (const QString& target : targets) {
        QString targetKey = normalizeToken(target);
        for (const StoreAppEntry& app : m_baseApps) {
            const QStringList keys = keysForApp(app);
            if (keys.contains(targetKey)) {
                QString id = !app.appId.isEmpty() ? app.appId : app.packageName;
                if (!seen.contains(id)) {
                    results.append(app);
                    seen.insert(id);
                }
                break;
            }
        }
    }
    return results;
}

QList<StoreAppEntry> StoreIndexer::buildNew() const {
    QList<StoreAppEntry> results;
    QDateTime cutoff = QDateTime::currentDateTime().addMonths(-6);
    for (const StoreAppEntry& app : m_baseApps) {
        if (app.releaseDate.isValid() && app.releaseDate > cutoff) {
            results.append(app);
        }
    }
    std::sort(results.begin(), results.end(), [](const StoreAppEntry& a, const StoreAppEntry& b) {
        return a.releaseDate > b.releaseDate;
    });
    return results;
}

QList<StoreAppEntry> StoreIndexer::buildRecommended() const {
    QList<StoreAppEntry> results;
    for (const StoreAppEntry& app : m_baseApps) {
        if (isRecommended(app)) {
            results.append(app);
        }
    }
    std::sort(results.begin(), results.end(), [](const StoreAppEntry& a, const StoreAppEntry& b) {
        return a.effectiveName().toLower() < b.effectiveName().toLower();
    });
    return results;
}

