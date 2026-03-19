#include "appstore_model.h"
#include <QTimer>
#include <QSet>
#include <QDebug>
#include <QDateTime>
#include <QRegularExpression>
#include <algorithm>
#include <cstdio>

namespace {
QString sanitizeDisplayName(const QString& input) {
    QString result;
    result.reserve(input.size());
    for (QChar ch : input) {
        if (ch.isPrint() && (ch.isLetterOrNumber() || ch.isSpace() || ch == '-' || ch == '_' || ch == '.' || ch == '+')) {
            result.append(ch);
        }
    }
    result = result.simplified();
    // Remove leading non-alphanumeric characters (fix random leading symbols)
    while (!result.isEmpty() && !result.at(0).isLetterOrNumber()) {
        result.remove(0, 1);
    }
    // Remove common suffixes for locale/language packs
    QString lower = result.toLower();
    if (lower.contains("language pack") || lower.contains("langpack") || lower.contains("localization") ||
        lower.contains("l10n") || lower.contains("locale")) {
        return QString();
    }
    return result;
}

QString sanitizePackageName(const QString& input) {
    QString result = input;
    // Strip control characters and whitespace
    result.remove(QRegularExpression("[\\x00-\\x1F\\x7F]"));
    result = result.trimmed();
    // Package names should start with alnum; strip leading garbage if any
    while (!result.isEmpty() && !result.at(0).isLetterOrNumber()) {
        result.remove(0, 1);
    }
    return result;
}

QString fixRandomLeadingChar(const QString& displayName, const QString& packageName) {
    if (displayName.isEmpty() || packageName.isEmpty()) {
        return displayName;
    }
    if (displayName.size() == packageName.size()) {
        // If only the first character differs but the rest matches, fix it.
        if (displayName.mid(1) == packageName.mid(1) && displayName.at(0) != packageName.at(0)) {
            return packageName;
        }
    }
    // If displayName starts with non-alnum but packageName starts correctly, prefer packageName.
    if (!displayName.at(0).isLetterOrNumber() && packageName.at(0).isLetterOrNumber()) {
        return packageName;
    }
    return displayName;
}
}

AppStoreModel::AppStoreModel(QObject* parent)
    : QObject(parent)
    , m_appStream(new AppStreamMetadata(this))
    , m_repositoryBrowser(new RepositoryBrowser(this))
    , m_initialized(false)
    , m_appStreamLoaded(false)
    , m_repositoryLoaded(false)
{
    connect(m_appStream, &AppStreamMetadata::loadingFinished, 
            this, &AppStoreModel::onAppStreamLoadingFinished);
    connect(m_appStream, &AppStreamMetadata::loadingProgress,
            this, &AppStoreModel::onAppStreamLoadingProgress);
    connect(m_repositoryBrowser, &RepositoryBrowser::searchCompleted,
            this, &AppStoreModel::onRepositorySearchCompleted);
}

AppStoreModel::~AppStoreModel() {
}

void AppStoreModel::initialize() {
    if (m_initialized) {
        return;
    }
    
    m_allApps.clear();
    m_popularApps.clear();
    m_newApps.clear();
    m_recommendedApps.clear();
    
    // TEMPORARILY DISABLED: Load AppStream metadata
    // AppStream loading causes memory corruption issues
    // if (m_appStream->isAvailable()) {
    //     QList<AppInfo> appStreamApps = m_appStream->loadAllApps();
    //     for (const AppInfo& app : appStreamApps) {
    //         m_allApps.append(app);
    //     }
    //     m_appStreamLoaded = true;
    // }
    m_appStreamLoaded = false;
    
    // Merge and process data first (with AppStream data)
    mergeAppData();
    
    m_initialized = true;
    emit initialized();
    
    // Load popular packages from repositories asynchronously (after UI is ready)
    // This won't block the UI initialization
    QTimer::singleShot(500, this, [this]() {
        loadPopularPackagesFromRepositories();
    });
}

void AppStoreModel::mergeAppData() {
    // Process all apps to determine collections
    for (AppInfo& app : m_allApps) {
        app.isPopular = isAppPopular(app);
        app.isNew = isAppNew(app);
        app.isRecommended = isAppRecommended(app);
        app.priority = calculateAppScore(app);
    }
    
    // Sort all apps by score
    sortAppsByScore(m_allApps);
    
    // Build collections
    m_popularApps.clear();
    m_newApps.clear();
    m_recommendedApps.clear();
    
    for (const AppInfo& app : m_allApps) {
        if (app.isPopular) {
            m_popularApps.append(app);
        }
        if (app.isNew) {
            m_newApps.append(app);
        }
        if (app.isRecommended) {
            m_recommendedApps.append(app);
        }
    }
    
    // Sort collections
    sortAppsByScore(m_popularApps);
    sortAppsByScore(m_newApps);
    sortAppsByScore(m_recommendedApps);
    
    // Update category counts
    m_categoryCounts.clear();
    for (const AppInfo& app : m_allApps) {
        if (app.categories.isEmpty()) {
            QString category = "Other";
            m_categoryCounts[category] = m_categoryCounts.value(category, 0) + 1;
        } else {
            for (const QString& category : app.categories) {
                m_categoryCounts[category] = m_categoryCounts.value(category, 0) + 1;
            }
        }
    }
}

QList<AppInfo> AppStoreModel::getApps(CollectionType type) {
    if (!m_initialized) {
        initialize();
    }
    
    switch (type) {
        case CollectionType::Popular:
            return m_popularApps;
        case CollectionType::New:
            return m_newApps;
        case CollectionType::Recommended:
            return m_recommendedApps;
        case CollectionType::All:
        default:
            return m_allApps;
    }
}

QList<AppInfo> AppStoreModel::getAppsByCategory(const QString& category) {
    if (!m_initialized) {
        initialize();
    }
    
    QList<AppInfo> result;
    
    // If category is empty, return all apps
    if (category.isEmpty()) {
        return m_allApps;
    }
    
    for (const AppInfo& app : m_allApps) {
        // Check if app has this category (exact match or display name match)
        bool hasCategory = false;
        for (const QString& appCategory : app.categories) {
            if (appCategory == category) {
                hasCategory = true;
                break;
            }
        }
        
        // Also check by display name
        if (!hasCategory) {
            QString categoryDisplay = AppStreamMetadata::categoryDisplayName(category);
            for (const QString& appCategory : app.categories) {
                if (AppStreamMetadata::categoryDisplayName(appCategory) == categoryDisplay) {
                    hasCategory = true;
                    break;
                }
            }
        }
        
        if (hasCategory) {
            result.append(app);
        }
    }
    
    sortAppsByScore(result);
    return result;
}

QList<AppInfo> AppStoreModel::searchApps(const QString& query) {
    if (!m_initialized) {
        initialize();
    }
    
    if (query.isEmpty()) {
        return m_allApps;
    }
    
    QString lowerQuery = query.toLower();
    QList<AppInfo> results;
    QMap<QString, int> scores; // App name -> relevance score
    
    // First, search in already loaded apps (AppStream)
    for (const AppInfo& app : m_allApps) {
        int score = 0;
        
        QString appNameLower = app.name.toLower();
        QString displayNameLower = app.displayName.toLower();
        QString packageNameLower = app.packageName.toLower();

        // Exact name match (highest priority)
        if (appNameLower == lowerQuery || displayNameLower == lowerQuery || packageNameLower == lowerQuery) {
            score += 1000;
        }
        // Name starts with query
        else if (appNameLower.startsWith(lowerQuery) || 
                 displayNameLower.startsWith(lowerQuery) || 
                 packageNameLower.startsWith(lowerQuery)) {
            score += 500;
        }
        // Name contains query
        else if (appNameLower.contains(lowerQuery) || 
                 displayNameLower.contains(lowerQuery) || 
                 packageNameLower.contains(lowerQuery)) {
            score += 200;
        }
        
        // Description contains query
        if (app.description.toLower().contains(lowerQuery)) {
            score += 50;
        }
        if (app.longDescription.toLower().contains(lowerQuery)) {
            score += 30;
        }
        
        // Category matches
        for (const QString& category : app.categories) {
            if (category.toLower().contains(lowerQuery)) {
                score += 100;
            }
        }
        
        // Developer matches
        if (app.developerName.toLower().contains(lowerQuery)) {
            score += 40;
        }
        
        if (score > 0) {
            scores[app.name] = score + app.priority; // Add base priority
        }
    }
    
    // If no results found in AppStream, trigger repository search
    // (results will come asynchronously via signal)
    if (results.isEmpty() && scores.isEmpty()) {
        searchAppsInRepositories(query);
    }
    
    // Sort by score
    QList<QPair<QString, int>> sortedScores;
    for (auto it = scores.begin(); it != scores.end(); ++it) {
        sortedScores.append(qMakePair(it.key(), it.value()));
    }
    
    std::sort(sortedScores.begin(), sortedScores.end(),
              [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
                  return a.second > b.second;
              });
    
    // Build result list
    for (const auto& pair : sortedScores) {
        for (const AppInfo& app : m_allApps) {
            if (app.name == pair.first) {
                results.append(app);
                break;
            }
        }
    }
    
    return results;
}

void AppStoreModel::searchAppsInRepositories(const QString& query) {
    m_currentRepositorySearchQuery = query;
    m_pendingRepositoryResults.clear();
    m_repositoryBrowser->searchPackages(query);
}

QList<AppInfo> AppStoreModel::getPopularApps(int limit) {
    QList<AppInfo> apps = getApps(CollectionType::Popular);
    if (limit > 0 && apps.size() > limit) {
        return apps.mid(0, limit);
    }
    return apps;
}

QList<AppInfo> AppStoreModel::getNewApps(int limit) {
    QList<AppInfo> apps = getApps(CollectionType::New);
    if (limit > 0 && apps.size() > limit) {
        return apps.mid(0, limit);
    }
    return apps;
}

QList<AppInfo> AppStoreModel::getRecommendedApps(int limit) {
    QList<AppInfo> apps = getApps(CollectionType::Recommended);
    if (limit > 0 && apps.size() > limit) {
        return apps.mid(0, limit);
    }
    return apps;
}

AppInfo AppStoreModel::getApp(const QString& name) {
    if (!m_initialized) {
        initialize();
    }
    
    for (const AppInfo& app : m_allApps) {
        if (app.name == name) {
            return app;
        }
    }
    
    return AppInfo();
}

QMap<QString, int> AppStoreModel::getCategoriesWithCounts() {
    if (!m_initialized) {
        initialize();
    }
    
    return m_categoryCounts;
}

void AppStoreModel::refresh() {
    m_initialized = false;
    m_appStreamLoaded = false;
    m_repositoryLoaded = false;
    initialize();
}

bool AppStoreModel::isAppPopular(const AppInfo& app) {
    // Get list of popular apps
    QStringList popularApps = getPopularAppsList();
    QString appNameLower = app.name.toLower();
    QString displayNameLower = app.displayName.toLower();
    QString packageNameLower = app.packageName.toLower();
    
    // Check if app name is in popular apps list (exact match or contains)
    for (const QString& popularName : popularApps) {
        if (appNameLower == popularName.toLower() || 
            displayNameLower == popularName.toLower() ||
            packageNameLower == popularName.toLower() ||
            appNameLower.contains(popularName.toLower()) ||
            displayNameLower.contains(popularName.toLower()) ||
            packageNameLower.contains(popularName.toLower())) {
            return true;
        }
    }
    
    // Also check by priority (lowered threshold)
    if (app.priority > 30) {
        return true;
    }
    
    // Check if has description and categories (well-maintained app)
    if (!app.description.isEmpty() && !app.categories.isEmpty()) {
        return true;
    }
    
    // Check repository (main repositories)
    if (app.repository == "main" || app.repository == "universe" || 
        app.repository == "community" || app.repository == "extra" ||
        app.repository == "core") {
        return true;
    }
    
    return false;
}

bool AppStoreModel::isAppNew(const AppInfo& app) {
    // Consider app new if:
    if (app.categories.isEmpty()) {
        return false;
    }
    // 1. Release date is within last 6 months
    // 2. Version number suggests recent release
    // 3. Recently updated in repository
    
    if (app.releaseDate.isValid()) {
        QDateTime sixMonthsAgo = QDateTime::currentDateTime().addMonths(-6);
        if (app.releaseDate > sixMonthsAgo) {
            return true;
        }
    }
    
    // Check version for patterns like "2024", "2023", etc.
    QString version = app.version.toLower();
    int currentYear = QDateTime::currentDateTime().date().year();
    int currentMonth = QDateTime::currentDateTime().date().month();
    
    // Check for year in version
    if (version.contains(QString::number(currentYear)) || 
        version.contains(QString::number(currentYear - 1))) {
        return true;
    }
    
    // Check for date patterns like "2024.01", "202401", etc.
    QString yearMonth = QString("%1.%2").arg(currentYear).arg(currentMonth, 2, 10, QChar('0'));
    QString yearMonthNoDot = QString("%1%2").arg(currentYear).arg(currentMonth, 2, 10, QChar('0'));
    if (version.contains(yearMonth) || version.contains(yearMonthNoDot)) {
        return true;
    }
    
    // Check repository for "testing", "unstable" (newer packages)
    if (app.repository.contains("testing", Qt::CaseInsensitive) ||
        app.repository.contains("unstable", Qt::CaseInsensitive) ||
        app.repository.contains("sid", Qt::CaseInsensitive)) {
        return true;
    }
    
    return false;
}

bool AppStoreModel::isAppRecommended(const AppInfo& app) {
    // Consider app recommended if:
    if (app.categories.isEmpty()) {
        return false;
    }
    // 1. Has AppStream metadata (well-maintained)
    // 2. Has icon and screenshots
    // 3. In popular categories
    // 4. High priority
    
    if (app.priority > 30 && !app.icon.isNull()) {
        return true;
    }
    
    if (!app.screenshots.isEmpty() && !app.categories.isEmpty()) {
        QStringList popularCategories = {"AudioVideo", "Development", "Game", 
                                         "Graphics", "Office", "Network"};
        for (const QString& cat : app.categories) {
            if (popularCategories.contains(cat)) {
                return true;
            }
        }
    }
    
    return false;
}

int AppStoreModel::calculateAppScore(const AppInfo& app) {
    int score = 0;
    
    // Base score from repository priority
    if (app.repository == "main" || app.repository == "core") {
        score += 50;
    } else if (app.repository == "universe" || app.repository == "community" || 
               app.repository == "extra") {
        score += 30;
    } else {
        score += 10;
    }
    
    // Bonus for having AppStream metadata
    if (!app.icon.isNull()) {
        score += 20;
    }
    
    if (!app.screenshots.isEmpty()) {
        score += 10;
    }
    
    if (!app.longDescription.isEmpty()) {
        score += 5;
    }
    
    // Bonus for popular categories
    QStringList popularCategories = {"AudioVideo", "Development", "Game", 
                                     "Graphics", "Office", "Network", "WebBrowser"};
    for (const QString& cat : app.categories) {
        if (popularCategories.contains(cat)) {
            score += 5;
        }
    }
    
    // Bonus for having developer info
    if (!app.developerName.isEmpty()) {
        score += 5;
    }
    
    return score;
}

void AppStoreModel::sortAppsByScore(QList<AppInfo>& apps) {
    std::sort(apps.begin(), apps.end(),
              [](const AppInfo& a, const AppInfo& b) {
                  return a.priority > b.priority;
              });
}

void AppStoreModel::onRepositorySearchCompleted(QList<PackageInfo> results) {
    QList<AppInfo> appResults;
    
    fprintf(stderr, "=== onRepositorySearchCompleted: processing %d packages ===\n", results.size());
    fflush(stderr);
    
    // Convert PackageInfo to AppInfo
    for (int idx = 0; idx < results.size(); idx++) {
        // Get raw bytes first
        QByteArray nameBytes = results[idx].name.toUtf8();
        QString rawPackageName = QString::fromUtf8(nameBytes.constData(), nameBytes.size());
        QString pkgName = sanitizePackageName(rawPackageName);
        
        fprintf(stderr, "Processing package[%d]: pkgName='%s'\n", idx, pkgName.toUtf8().constData());
        fflush(stderr);
        
        // Filter out libraries and dependencies
        QString pkgNameLower = pkgName.toLower();
        if (pkgNameLower.startsWith("lib") && 
            (pkgNameLower.endsWith("-dev") || pkgNameLower.endsWith("-common") || 
             pkgNameLower.endsWith("-data") || pkgNameLower.endsWith("-dbg"))) {
            continue;
        }
        
        // Create AppInfo with SEPARATE copies for each field
        // Each field gets its own independent QString to prevent shared buffer issues
        QByteArray descBytes = results[idx].description.toUtf8();
        QByteArray versionBytes = results[idx].version.toUtf8();
        QByteArray archBytes = results[idx].architecture.toUtf8();
        QByteArray repoBytes = results[idx].repository.toUtf8();
        
        AppInfo app;
        // Create independent QStrings and sanitize display fields
        QString displayName = sanitizeDisplayName(rawPackageName);
        if (displayName.isEmpty()) {
            displayName = rawPackageName.simplified();
        }
        QString cleanPackageName = pkgName.isEmpty() ? rawPackageName.simplified() : pkgName;
        // Repair random leading character if tail matches package name
        displayName = fixRandomLeadingChar(displayName, cleanPackageName);
        app.name = displayName;
        app.packageName = cleanPackageName;
        app.displayName = displayName;
        app.version = QString::fromUtf8(versionBytes.constData(), versionBytes.size());
        app.description = QString::fromUtf8(descBytes.constData(), descBytes.size());
        app.size = results[idx].size;
        app.architecture = QString::fromUtf8(archBytes.constData(), archBytes.size());
        app.repository = QString::fromUtf8(repoBytes.constData(), repoBytes.size());
        app.source = results[idx].source;
        app.categories << "Other";
        
        fprintf(stderr, "  Created app: name='%s' displayName='%s'\n", 
                app.name.toUtf8().constData(), app.displayName.toUtf8().constData());
        fflush(stderr);
        
        // Try to find existing AppInfo with same name
        bool found = false;
        for (AppInfo& existingApp : m_allApps) {
            if (existingApp.name == app.name || 
                existingApp.name.toLower() == app.name.toLower() ||
                existingApp.packageName.toLower() == pkgNameLower) {
                // Merge: keep AppStream data, update package info
                existingApp.version = app.version;
                existingApp.size = app.size;
                existingApp.architecture = app.architecture;
                existingApp.repository = app.repository;
                existingApp.downloadUrl = app.downloadUrl;
                existingApp.source = app.source;
                if (!app.displayName.isEmpty()) {
                    existingApp.displayName = app.displayName;
                    existingApp.name = app.displayName;
                }
                if (!app.categories.isEmpty()) {
                    existingApp.categories = app.categories;
                }
                if (existingApp.description.isEmpty() && !app.description.isEmpty()) {
                    existingApp.description = app.description;
                }
                app = existingApp; // Use merged version
                found = true;
                break;
            }
        }
        
        if (!found) {
            // Add new app to main list
            m_allApps.append(app);
        }
        
        appResults.append(app);
    }
    
    // If this was a search query, emit results
    if (!m_currentRepositorySearchQuery.isEmpty()) {
        emit repositorySearchCompleted(appResults);
        m_currentRepositorySearchQuery.clear();
    }
    
    m_repositoryLoaded = true;
    if (m_appStreamLoaded && m_repositoryLoaded) {
        mergeAppData();
    }
}

void AppStoreModel::onRepositorySearchFinished() {
    // Handle search completion if needed
}

void AppStoreModel::onAppStreamLoadingFinished() {
    m_appStreamLoaded = true;
    if (m_appStreamLoaded && m_repositoryLoaded) {
        mergeAppData();
    }
}

void AppStoreModel::onAppStreamLoadingProgress(int current, int total) {
    emit loadingProgress(current, total);
}

QStringList AppStoreModel::getPopularAppsList() {
    return QStringList({
        "discord", "firefox", "chromium", "steam", "vlc", "gimp", "inkscape",
        "libreoffice", "thunderbird", "code", "codium", "atom", "sublime-text",
        "gedit", "kate", "nano", "vim", "emacs", "git", "python3", "nodejs",
        "docker", "virtualbox", "wine", "audacity", "blender", "kdenlive",
        "obs-studio", "spotify", "telegram-desktop", "skype", "zoom", "slack",
        "gparted", "htop", "neofetch", "ranger", "tmux", "zsh", "fish",
        "neovim", "alacritty", "kitty", "wezterm", "rofi", "dmenu", "i3",
        "sway", "gnome", "kde", "xfce", "mate", "cinnamon", "lxde", "openbox",
        "calibre", "okular", "evince", "zathura", "mpv", "ffmpeg", "imagemagick",
        "gphoto2", "darktable", "rawtherapee", "krita", "mypaint", "pencil2d",
        "ardour", "lmms", "musescore", "tuxguitar", "guitarix", "hydrogen",
        "arduino", "eclipse", "netbeans", "qtcreator", "android-studio",
        "intellij-idea", "pycharm", "clion", "rider", "webstorm", "phpstorm",
        "datagrip", "goland", "rubymine", "appcode", "fleet",
        "discord-canary", "firefox-developer", "chromium-dev", "steam-native",
        "vlc-plugin", "gimp-plugin", "libreoffice-writer", "libreoffice-calc",
        "libreoffice-impress", "thunderbird-locale", "code-oss", "codium-bin"
    });
}

void AppStoreModel::loadPopularPackagesFromRepositories() {
    static QSet<AppStoreModel*> loadingModels;
    if (loadingModels.contains(this)) {
        return;
    }
    loadingModels.insert(this);

    QStringList popularApps = getPopularAppsList();
    int batchSize = qMin(5, popularApps.size());

    QTimer* loadTimer = new QTimer(this);
    loadTimer->setSingleShot(false);
    loadTimer->setInterval(200);

    int* currentIndex = new int(0);

    QObject::connect(loadTimer, &QTimer::timeout, this, [this, popularApps, batchSize, loadTimer, currentIndex]() {
        if (*currentIndex < batchSize) {
            m_repositoryBrowser->searchPackages(popularApps[*currentIndex]);
            emit loadingProgress(*currentIndex + 1, batchSize);
            (*currentIndex)++;
        } else {
            loadTimer->stop();
            loadTimer->deleteLater();
            delete currentIndex;
            loadingModels.remove(this);
        }
    });

    QTimer::singleShot(100, loadTimer, [loadTimer]() {
        loadTimer->start();
    });
}

