#include "appstream_metadata.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <QImageReader>
#include <QDebug>
#include <QIcon>
#include <QRegularExpression>
#include <cstdio>
#include "store/name_trace.h"

namespace {
// Work with QByteArray to avoid QString implicit sharing issues
QByteArray sanitizeNameBytes(const QByteArray& input) {
    QByteArray result;
    result.reserve(input.size());
    for (int i = 0; i < input.size(); ++i) {
        char ch = input.at(i);
        // Allow printable ASCII, UTF-8 continuation bytes, and common punctuation
        if ((ch >= 32 && ch <= 126) || (ch & 0x80)) {  // Printable ASCII or UTF-8 high bit
            result.append(ch);
        }
    }
    return result.trimmed();
}

QString sanitizeName(const QString& input) {
    QByteArray utf8 = input.toUtf8();
    QByteArray sanitized = sanitizeNameBytes(utf8);
    return QString::fromUtf8(sanitized).simplified();
}
}

AppStreamMetadata::AppStreamMetadata(QObject* parent)
    : QObject(parent)
    , m_cacheLoaded(false)
{
    // Standard AppStream paths
    m_appStreamPaths << "/usr/share/app-info/xmls"
                     << "/usr/share/metainfo"
                     << "/usr/share/appdata"
                     << QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/app-info/xmls";
}

AppStreamMetadata::~AppStreamMetadata() {
}

bool AppStreamMetadata::isAvailable() const {
    QStringList files = findAppStreamFiles();
    return !files.isEmpty();
}

QStringList AppStreamMetadata::findAppStreamFiles() const {
    QStringList files;
    
    for (const QString& basePath : m_appStreamPaths) {
        QDir dir(basePath);
        if (!dir.exists()) continue;
        
        // Look for .xml, .appdata.xml, .metainfo.xml files
        QStringList filters;
        filters << "*.xml" << "*.appdata.xml" << "*.metainfo.xml";
        
        QFileInfoList entries = dir.entryInfoList(filters, QDir::Files);
        for (const QFileInfo& entry : entries) {
            files.append(entry.absoluteFilePath());
        }
        
        // Also check subdirectories
        QFileInfoList subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& subdir : subdirs) {
            QDir subdirPath(subdir.absoluteFilePath());
            QFileInfoList subEntries = subdirPath.entryInfoList(filters, QDir::Files);
            for (const QFileInfo& entry : subEntries) {
                files.append(entry.absoluteFilePath());
            }
        }
    }
    
    return files;
}

QList<AppInfo> AppStreamMetadata::loadAllApps() {
    if (m_cacheLoaded && !m_appCache.isEmpty()) {
        return m_appCache.values();
    }
    
    m_appCache.clear();
    QStringList files = findAppStreamFiles();
    
    if (files.isEmpty()) {
        emit loadingError("No AppStream metadata files found");
        return QList<AppInfo>();
    }
    
    int total = files.size();
    int current = 0;
    
    for (const QString& filePath : files) {
        QList<AppInfo> apps = parseAppStreamFile(filePath);
        for (const AppInfo& app : apps) {
            QString key = !app.packageName.isEmpty() ? app.packageName : app.name;
            if (!key.isEmpty()) {
                m_appCache[key] = app;
                StoreNameTrace::trace("appstream-cache", key, app.displayName);
            }
        }
        current++;
        emit loadingProgress(current, total);
    }
    
    m_cacheLoaded = true;
    emit loadingFinished();
    
    return m_appCache.values();
}

QList<AppInfo> AppStreamMetadata::parseAppStreamFile(const QString& filePath) {
    QList<AppInfo> apps;
    
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return apps;
    }
    
    QString xmlContent = QString::fromUtf8(file.readAll());
    file.close();
    
    AppInfo app = parseComponent(xmlContent, filePath);
    if (!app.name.isEmpty()) {
        apps.append(app);
    }
    
    return apps;
}

AppInfo AppStreamMetadata::parseComponent(const QString& xmlContent, const QString& filePath) {
    AppInfo app;
    QStringList elementStack;
    
    QXmlStreamReader xml(xmlContent);
    
    while (!xml.atEnd()) {
        xml.readNext();
        
        if (xml.isStartElement()) {
            elementStack.append(xml.name().toString());
            if (xml.name() == QString("component")) {
                QString type = xml.attributes().value("type").toString();
                if (type != "desktop" && type != "desktop-application") {
                    xml.skipCurrentElement();
                    if (!elementStack.isEmpty()) {
                        elementStack.removeLast();
                    }
                    continue;
                }
            } else if (xml.name() == QString("id")) {
                // Work with QByteArray to avoid QString corruption
                QByteArray rawIdBytes = xml.readElementText().trimmed().toUtf8();
                
                // Remove .desktop suffix at byte level
                if (rawIdBytes.endsWith(".desktop")) {
                    rawIdBytes.chop(8);  // Remove ".desktop"
                }
                
                app.appId = QString::fromUtf8(rawIdBytes);
                
                if (app.packageName.isEmpty()) {
                    app.packageName = app.appId;
                }
                if (!elementStack.isEmpty()) {
                    elementStack.removeLast();
                }
            } else if (xml.name() == QString("launchable")) {
                QString type = xml.attributes().value("type").toString();
                if (type == "desktop-id") {
                    QString desktopId = xml.readElementText().trimmed();
                    if (desktopId.endsWith(".desktop")) {
                        desktopId.chop(8);
                    }
                    app.desktopId = desktopId;
                } else {
                    xml.readElementText(); // consume
                }
                if (!elementStack.isEmpty()) {
                    elementStack.removeLast();
                }
            } else if (xml.name() == QString("name")) {
                QString parent = elementStack.size() >= 2 ? elementStack.at(elementStack.size() - 2) : QString();
                QString lang = xml.attributes().value("xml:lang").toString();
                QString text = xml.readElementText().trimmed();
                
                if (parent == QString("component") && (lang.isEmpty() || lang == "en" || lang == "C")) {
                    // Work directly with bytes to avoid QString corruption
                    QByteArray nameBytes = text.toUtf8();
                    QByteArray sanitizedBytes = sanitizeNameBytes(nameBytes);
                    
                    app.displayName = QString::fromUtf8(sanitizedBytes);
                    StoreNameTrace::trace("appstream-parse", app.appId.isEmpty() ? app.packageName : app.appId, app.displayName);
                    
                    if (app.name.isEmpty()) {
                        app.name = app.displayName;
                    }
                } else if (parent == QString("developer") && app.developerName.isEmpty()) {
                    app.developerName = text;
                }
                if (!elementStack.isEmpty()) {
                    elementStack.removeLast();
                }
            } else if (xml.name() == QString("summary")) {
                QString lang = xml.attributes().value("xml:lang").toString();
                if (lang.isEmpty() || lang == "en" || lang == "C") {
                    app.description = xml.readElementText().trimmed();
                    if (!elementStack.isEmpty()) {
                        elementStack.removeLast();
                    }
                }
            } else if (xml.name() == QString("description")) {
                QString lang = xml.attributes().value("xml:lang").toString();
                if (lang.isEmpty() || lang == "en" || lang == "C") {
                    QString desc = xml.readElementText().trimmed();
                    // Remove HTML tags
                    desc.remove(QRegularExpression("<[^>]*>"));
                    app.longDescription = desc.trimmed();
                    if (!elementStack.isEmpty()) {
                        elementStack.removeLast();
                    }
                }
            } else if (xml.name() == QString("developer_name")) {
                app.developerName = xml.readElementText().trimmed();
                if (!elementStack.isEmpty()) {
                    elementStack.removeLast();
                }
            } else if (xml.name() == QString("project_license")) {
                app.license = xml.readElementText().trimmed();
                if (!elementStack.isEmpty()) {
                    elementStack.removeLast();
                }
            } else if (xml.name() == QString("url") && xml.attributes().value("type").toString() == "homepage") {
                app.homepage = xml.readElementText().trimmed();
                if (!elementStack.isEmpty()) {
                    elementStack.removeLast();
                }
            } else if (xml.name() == QString("categories")) {
                // Categories will be parsed in the loop
            } else if (xml.name() == QString("category")) {
                QString category = xml.readElementText().trimmed();
                if (!category.isEmpty() && !app.categories.contains(category)) {
                    app.categories.append(category);
                }
                if (!elementStack.isEmpty()) {
                    elementStack.removeLast();
                }
            } else if (xml.name() == QString("icon")) {
                QString type = xml.attributes().value("type").toString();
                if (type == "local") {
                    QString iconPath = xml.readElementText().trimmed();
                    if (!iconPath.isEmpty()) {
                        app.iconPath = iconPath;
                    }
                    if (!elementStack.isEmpty()) {
                        elementStack.removeLast();
                    }
                } else if (type.isEmpty() || type == "stock") {
                    QString iconName = xml.readElementText().trimmed();
                    if (!iconName.isEmpty()) {
                        app.iconPath = findIconPath(iconName);
                    }
                    if (!elementStack.isEmpty()) {
                        elementStack.removeLast();
                    }
                }
            } else if (xml.name() == QString("screenshot")) {
                // Screenshots will be parsed in nested loop
            } else if (xml.name() == QString("image")) {
                QString imagePath = xml.readElementText().trimmed();
                if (!imagePath.isEmpty()) {
                    app.screenshotPaths.append(imagePath);
                }
                if (!elementStack.isEmpty()) {
                    elementStack.removeLast();
                }
            } else if (xml.name() == QString("releases")) {
                // Release date parsing
            } else if (xml.name() == QString("release")) {
                QString dateStr = xml.attributes().value("date").toString();
                if (!dateStr.isEmpty()) {
                    app.releaseDate = QDateTime::fromString(dateStr, Qt::ISODate);
                }
                if (!elementStack.isEmpty()) {
                    elementStack.removeLast();
                }
            }
        } else if (xml.isEndElement()) {
            if (!elementStack.isEmpty()) {
                elementStack.removeLast();
            }
        }
    }
    
    if (xml.hasError()) {
        qDebug() << "XML parsing error:" << xml.errorString();
    }
    
    if (app.displayName.isEmpty() && !app.name.isEmpty()) {
        app.displayName = sanitizeName(app.name);
    }
    if (app.name.isEmpty() && !app.displayName.isEmpty()) {
        app.name = app.displayName;
    }
    if (app.name.isEmpty() && !app.packageName.isEmpty()) {
        app.name = app.packageName;
        app.displayName = app.packageName;
    }
    if (app.packageName.isEmpty() && !app.appId.isEmpty()) {
        // Fallback: derive package-like name from appId
        QStringList parts = app.appId.split('.', Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            app.packageName = parts.last();
        } else {
            app.packageName = app.appId;
        }
    }

    if (app.desktopId.isEmpty() && !app.appId.isEmpty()) {
        app.desktopId = app.appId;
    }

    // Force deep copies to avoid implicit sharing corruption across boundaries
    auto deepCopy = [](const QString& value) {
        return QString::fromUtf8(value.toUtf8());
    };
    app.appId = deepCopy(app.appId);
    app.desktopId = deepCopy(app.desktopId);
    app.packageName = deepCopy(app.packageName);
    app.displayName = deepCopy(app.displayName);
    app.name = deepCopy(app.name);
    app.version = deepCopy(app.version);
    app.description = deepCopy(app.description);
    app.longDescription = deepCopy(app.longDescription);
    app.repository = deepCopy(app.repository);
    app.downloadUrl = deepCopy(app.downloadUrl);
    app.license = deepCopy(app.license);
    app.developer = deepCopy(app.developer);
    app.developerName = deepCopy(app.developerName);
    app.homepage = deepCopy(app.homepage);

    // Load icon if path is available
    if (!app.iconPath.isEmpty()) {
        app.icon = loadIcon(app.iconPath);
    }
    
    return app;
}

QPixmap AppStreamMetadata::loadIcon(const QString& iconPath, int size) {
    QPixmap pixmap;
    
    // Try direct path first
    if (QFile::exists(iconPath)) {
        pixmap = QPixmap(iconPath);
        if (!pixmap.isNull() && (pixmap.width() != size || pixmap.height() != size)) {
            pixmap = pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return pixmap;
    }
    
    // Try to find in standard icon paths
    QString foundPath = findIconPath(iconPath, size);
    if (!foundPath.isEmpty() && QFile::exists(foundPath)) {
        pixmap = QPixmap(foundPath);
        if (!pixmap.isNull() && (pixmap.width() != size || pixmap.height() != size)) {
            pixmap = pixmap.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    }
    
    return pixmap;
}

QString AppStreamMetadata::findIconPath(const QString& iconName, int size) {
    // Remove extension if present
    QString baseName = iconName;
    if (baseName.endsWith(".png") || baseName.endsWith(".svg") || baseName.endsWith(".xpm")) {
        baseName = baseName.left(baseName.lastIndexOf('.'));
    }
    
    // Standard icon paths
    QStringList iconPaths;
    iconPaths << QString("/usr/share/pixmaps/%1.png").arg(baseName)
              << QString("/usr/share/pixmaps/%1.svg").arg(baseName)
              << QString("/usr/share/pixmaps/%1.xpm").arg(baseName)
              << QString("/usr/share/icons/hicolor/%1x%1/apps/%2.png").arg(size).arg(baseName)
              << QString("/usr/share/icons/hicolor/%1x%1/apps/%2.svg").arg(size).arg(baseName)
              << QString("/usr/share/icons/hicolor/scalable/apps/%1.svg").arg(baseName)
              << QStandardPaths::locate(QStandardPaths::GenericDataLocation, 
                                       QString("icons/hicolor/%1x%1/apps/%2.png").arg(size).arg(baseName))
              << QStandardPaths::locate(QStandardPaths::GenericDataLocation, 
                                       QString("icons/hicolor/scalable/apps/%1.svg").arg(baseName));
    
    for (const QString& path : iconPaths) {
        if (QFile::exists(path)) {
            return path;
        }
    }
    
    return QString();
}

AppInfo AppStreamMetadata::getAppInfo(const QString& packageName) {
    if (!m_cacheLoaded) {
        loadAllApps();
    }
    
    if (m_appCache.contains(packageName)) {
        return m_appCache[packageName];
    }
    if (m_appCache.contains(packageName.toLower())) {
        return m_appCache[packageName.toLower()];
    }
    // Try appId/desktopId variants
    for (const auto& app : m_appCache) {
        if (app.appId == packageName || app.desktopId == packageName) {
            return app;
        }
    }
    
    // Return empty AppInfo if not found
    return AppInfo();
}

QPixmap AppStreamMetadata::getIcon(const QString& packageName, int size) {
    AppInfo app = getAppInfo(packageName);
    if (!app.icon.isNull()) {
        if (app.icon.width() != size || app.icon.height() != size) {
            return app.icon.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        return app.icon;
    }
    
    // Use improved icon finder
    return findIconForPackage(packageName, app.appId, app.desktopId, size);
}

QPixmap AppStreamMetadata::findIconForPackage(const QString& packageName, int size) {
    return findIconForPackage(packageName, QString(), QString(), size);
}

QPixmap AppStreamMetadata::findIconForPackage(const QString& packageName, const QString& appId, const QString& desktopId, int size) {
    // 1. Try to find .desktop file and extract icon from it (most reliable)
    QString desktopFile;
    if (!desktopId.isEmpty()) {
        desktopFile = findDesktopFile(desktopId);
    }
    if (desktopFile.isEmpty() && !appId.isEmpty()) {
        desktopFile = findDesktopFile(appId);
    }
    if (desktopFile.isEmpty()) {
        desktopFile = findDesktopFile(packageName);
    }
    if (!desktopFile.isEmpty()) {
        QString iconName = extractIconFromDesktop(desktopFile);
        if (!iconName.isEmpty()) {
            QIcon themeIcon = QIcon::fromTheme(iconName);
            if (!themeIcon.isNull()) {
                QPixmap themed = themeIcon.pixmap(size, size);
                if (!themed.isNull()) {
                    return themed;
                }
            }
            // Try as direct path first
            if (QFile::exists(iconName)) {
                QPixmap icon = loadIcon(iconName, size);
                if (!icon.isNull()) {
                    return icon;
                }
            }
            // Try as icon name
            QString iconPath = findIconPath(iconName, size);
            if (!iconPath.isEmpty()) {
                QPixmap icon = loadIcon(iconPath, size);
                if (!icon.isNull()) {
                    return icon;
                }
            }
        }
    }
    
    // 2. Try variations of package name with more aggressive search
    QStringList nameVariations;
    nameVariations << packageName;
    nameVariations << packageName.toLower();
    nameVariations << packageName.toUpper();
    
    // Remove common prefixes/suffixes
    QString baseName = packageName;
    baseName.remove(QRegularExpression("^lib"));
    baseName.remove(QRegularExpression("-dev$"));
    baseName.remove(QRegularExpression("-common$"));
    baseName.remove(QRegularExpression("-data$"));
    baseName.remove(QRegularExpression("-bin$"));
    baseName.remove(QRegularExpression("^kde-"));
    baseName.remove(QRegularExpression("^gnome-"));
    if (baseName != packageName) {
        nameVariations << baseName;
        nameVariations << baseName.toLower();
    }
    
    // Try variations
    for (const QString& name : nameVariations) {
        QIcon themeIcon = QIcon::fromTheme(name);
        if (!themeIcon.isNull()) {
            QPixmap themed = themeIcon.pixmap(size, size);
            if (!themed.isNull()) {
                return themed;
            }
        }
        QString iconPath = findIconPath(name, size);
        if (!iconPath.isEmpty()) {
            QPixmap icon = loadIcon(iconPath, size);
            if (!icon.isNull()) {
                return icon;
            }
        }
    }
    
    // 3. Search all desktop files for matching Exec or Name
    QDir applicationsDir("/usr/share/applications");
    if (applicationsDir.exists()) {
        QFileInfoList desktopFiles = applicationsDir.entryInfoList({"*.desktop"}, QDir::Files);
        QString packageNameLower = packageName.toLower();
        for (const QFileInfo& fileInfo : desktopFiles) {
            QFile file(fileInfo.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly)) {
                QString content = QString::fromUtf8(file.readAll());
                // Check if Exec or Name contains package name
                if (content.contains(packageNameLower, Qt::CaseInsensitive)) {
                    QString iconName = extractIconFromDesktop(fileInfo.absoluteFilePath());
                    if (!iconName.isEmpty()) {
                        QIcon themeIcon = QIcon::fromTheme(iconName);
                        if (!themeIcon.isNull()) {
                            QPixmap themed = themeIcon.pixmap(size, size);
                            if (!themed.isNull()) {
                                return themed;
                            }
                        }
                        QString iconPath = findIconPath(iconName, size);
                        if (!iconPath.isEmpty() && QFile::exists(iconName)) {
                            iconPath = iconName;
                        }
                        if (!iconPath.isEmpty()) {
                            QPixmap icon = loadIcon(iconPath, size);
                            if (!icon.isNull()) {
                                return icon;
                            }
                        }
                    }
                }
            }
        }
    }
    
    return QPixmap();
}

QString AppStreamMetadata::findDesktopFile(const QString& packageName) {
    // Standard desktop file locations
    QStringList desktopPaths;
    desktopPaths << QString("/usr/share/applications/%1.desktop").arg(packageName)
                 << QString("/usr/share/applications/%1.desktop").arg(packageName.toLower())
                 << QStandardPaths::locate(QStandardPaths::ApplicationsLocation, 
                                           QString("%1.desktop").arg(packageName))
                 << QStandardPaths::locate(QStandardPaths::ApplicationsLocation, 
                                           QString("%1.desktop").arg(packageName.toLower()));
    
    // Also search in all .desktop files (exact match only)
    QDir applicationsDir("/usr/share/applications");
    if (applicationsDir.exists()) {
        QFileInfoList desktopFiles = applicationsDir.entryInfoList({"*.desktop"}, QDir::Files);
        QString packageNameLower = packageName.toLower();
        for (const QFileInfo& fileInfo : desktopFiles) {
            QString fileName = fileInfo.baseName().toLower();
            if (fileName == packageNameLower) {
                return fileInfo.absoluteFilePath();
            }
        }
    }
    
    // Check user applications
    QString userAppsDir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    QDir userDir(userAppsDir);
    if (userDir.exists()) {
        QFileInfoList desktopFiles = userDir.entryInfoList({"*.desktop"}, QDir::Files);
        QString packageNameLower = packageName.toLower();
        for (const QFileInfo& fileInfo : desktopFiles) {
            QString fileName = fileInfo.baseName().toLower();
            if (fileName == packageNameLower) {
                return fileInfo.absoluteFilePath();
            }
        }
    }
    
    return QString();
}

QString AppStreamMetadata::extractIconFromDesktop(const QString& desktopFile) {
    QFile file(desktopFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    
    QString content = QString::fromUtf8(file.readAll());
    file.close();
    
    // Look for Icon= line
    QRegularExpression iconRegex("^Icon\\s*=\\s*(.+)$", QRegularExpression::MultilineOption);
    QRegularExpressionMatch match = iconRegex.match(content);
    if (match.hasMatch()) {
        QString iconName = match.captured(1).trimmed();
        // Remove % expansions
        iconName.remove(QRegularExpression("%[a-z]"));
        return iconName;
    }
    
    return QString();
}

QString AppStreamMetadata::extractNameFromDesktop(const QString& desktopFile) {
    QFile file(desktopFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    
    QString content = QString::fromUtf8(file.readAll());
    file.close();
    
    // Look for Name= line (preferred)
    QRegularExpression nameRegex("^Name\\s*=\\s*(.+)$", QRegularExpression::MultilineOption);
    QRegularExpressionMatch match = nameRegex.match(content);
    if (match.hasMatch()) {
        QString name = match.captured(1).trimmed();
        // Remove locale suffixes like [en], [ru], etc.
        name.remove(QRegularExpression("\\[.*\\]"));
        return name.trimmed();
    }
    
    // Fallback to Name[en]= or first Name[*]=
    QRegularExpression nameLocaleRegex("^Name\\[.*\\]\\s*=\\s*(.+)$", QRegularExpression::MultilineOption);
    QRegularExpressionMatch localeMatch = nameLocaleRegex.match(content);
    if (localeMatch.hasMatch()) {
        QString name = localeMatch.captured(1).trimmed();
        return name;
    }
    
    return QString();
}

QStringList AppStreamMetadata::extractCategoriesFromDesktop(const QString& desktopFile) {
    QFile file(desktopFile);
    if (!file.open(QIODevice::ReadOnly)) {
        return QStringList();
    }
    
    QString content = QString::fromUtf8(file.readAll());
    file.close();
    
    // Look for Categories= line
    QRegularExpression categoriesRegex("^Categories\\s*=\\s*(.+)$", QRegularExpression::MultilineOption);
    QRegularExpressionMatch match = categoriesRegex.match(content);
    if (match.hasMatch()) {
        QString categoriesStr = match.captured(1).trimmed();
        QStringList categories = categoriesStr.split(';', Qt::SkipEmptyParts);
        for (QString& category : categories) {
            category = category.trimmed();
        }
        return categories;
    }
    
    return QStringList();
}

QStringList AppStreamMetadata::categoriesFromSection(const QString& section) {
    QString sectionLower = section.toLower();
    QStringList categories;
    
    if (sectionLower.contains("game")) {
        categories << "Game";
    } else if (sectionLower.contains("graphics") || sectionLower.contains("image")) {
        categories << "Graphics";
    } else if (sectionLower.contains("audio") || sectionLower.contains("video") || sectionLower.contains("multimedia")) {
        categories << "AudioVideo";
    } else if (sectionLower.contains("net") || sectionLower.contains("web") || sectionLower.contains("network")) {
        categories << "Network";
    } else if (sectionLower.contains("office")) {
        categories << "Office";
    } else if (sectionLower.contains("science")) {
        categories << "Science";
    } else if (sectionLower.contains("utils") || sectionLower.contains("utility")) {
        categories << "Utility";
    } else if (sectionLower.contains("system")) {
        categories << "System";
    } else if (sectionLower.contains("editor") || sectionLower.contains("text")) {
        categories << "TextEditor";
    } else if (sectionLower.contains("devel") || sectionLower.contains("development")) {
        categories << "Development";
    }
    
    if (categories.isEmpty()) {
        categories << "Other";
    }
    
    return categories;
}

QList<QPixmap> AppStreamMetadata::getScreenshots(const QString& packageName) {
    AppInfo app = getAppInfo(packageName);
    QList<QPixmap> screenshots;
    
    for (const QString& path : app.screenshotPaths) {
        if (QFile::exists(path)) {
            QPixmap pixmap(path);
            if (!pixmap.isNull()) {
                screenshots.append(pixmap);
            }
        }
    }
    
    return screenshots;
}

QStringList AppStreamMetadata::getCategories(const QString& packageName) {
    AppInfo app = getAppInfo(packageName);
    return app.categories;
}

QString AppStreamMetadata::categoryDisplayName(const QString& category) {
    static QMap<QString, QString> categoryMap = {
        {"AudioVideo", "Multimedia"},
        {"Audio", "Audio"},
        {"Video", "Video"},
        {"Development", "Development"},
        {"Education", "Education"},
        {"Game", "Games"},
        {"Graphics", "Graphics"},
        {"Network", "Network"},
        {"Office", "Office"},
        {"Science", "Science"},
        {"System", "System"},
        {"Utility", "Utilities"},
        {"Settings", "Settings"},
        {"Accessibility", "Accessibility"},
        {"TextEditor", "Text Editors"},
        {"IDE", "IDEs"},
        {"WebBrowser", "Web Browsers"},
        {"Email", "Email"},
        {"InstantMessaging", "Messaging"},
        {"Chat", "Chat"},
        {"FileManager", "File Managers"},
        {"TerminalEmulator", "Terminals"},
        {"VideoPlayer", "Video Players"},
        {"AudioPlayer", "Audio Players"},
        {"Music", "Music"},
        {"Photo", "Photo"},
        {"Viewer", "Viewers"},
        {"Editor", "Editors"},
        {"3DGraphics", "3D Graphics"},
        {"VectorGraphics", "Vector Graphics"},
        {"RasterGraphics", "Raster Graphics"},
        {"Scanning", "Scanning"},
        {"OCR", "OCR"},
        {"Photography", "Photography"},
        {"Publishing", "Publishing"},
        {"Viewer", "Viewers"},
        {"Calendar", "Calendar"},
        {"ContactManagement", "Contacts"},
        {"Database", "Databases"},
        {"Dictionary", "Dictionaries"},
        {"Finance", "Finance"},
        {"FlowChart", "Flow Charts"},
        {"ProjectManagement", "Project Management"},
        {"Presentation", "Presentations"},
        {"Spreadsheet", "Spreadsheets"},
        {"WordProcessor", "Word Processors"},
        {"2DGraphics", "2D Graphics"},
        {"Construction", "Construction"},
        {"Electricity", "Electricity"},
        {"Electronics", "Electronics"},
        {"Engineering", "Engineering"},
        {"Aerospace", "Aerospace"},
        {"Astronomy", "Astronomy"},
        {"Biology", "Biology"},
        {"Chemistry", "Chemistry"},
        {"ComputerScience", "Computer Science"},
        {"DataVisualization", "Data Visualization"},
        {"Economy", "Economy"},
        {"Geography", "Geography"},
        {"Geology", "Geology"},
        {"Geoscience", "Geoscience"},
        {"History", "History"},
        {"Humanities", "Humanities"},
        {"ImageProcessing", "Image Processing"},
        {"Languages", "Languages"},
        {"Literature", "Literature"},
        {"Maps", "Maps"},
        {"Math", "Mathematics"},
        {"NumericalAnalysis", "Numerical Analysis"},
        {"MedicalSoftware", "Medical"},
        {"Physics", "Physics"},
        {"Robotics", "Robotics"},
        {"Spirituality", "Spirituality"},
        {"Sports", "Sports"},
        {"ParallelComputing", "Parallel Computing"},
        {"ArtificialIntelligence", "AI"},
        {"Art", "Art"},
        {"Languages", "Languages"},
        {"Religion", "Religion"},
        {"BoardGame", "Board Games"},
        {"CardGame", "Card Games"},
        {"ActionGame", "Action Games"},
        {"AdventureGame", "Adventure Games"},
        {"ArcadeGame", "Arcade Games"},
        {"BlocksGame", "Puzzle Games"},
        {"BoardGame", "Board Games"},
        {"CardGame", "Card Games"},
        {"KidsGame", "Kids Games"},
        {"LogicGame", "Logic Games"},
        {"RolePlaying", "RPG"},
        {"Shooter", "Shooters"},
        {"Simulation", "Simulation"},
        {"SportsGame", "Sports Games"},
        {"StrategyGame", "Strategy Games"},
        {"Amusement", "Amusement"},
        {"Archiving", "Archiving"},
        {"Compression", "Compression"},
        {"PackageManager", "Package Managers"},
        {"Monitor", "System Monitors"},
        {"Security", "Security"},
        {"Accessibility", "Accessibility"},
        {"Calculator", "Calculators"},
        {"Clock", "Clocks"},
        {"TextTools", "Text Tools"},
        {"DesktopSettings", "Desktop Settings"},
        {"HardwareSettings", "Hardware Settings"},
        {"Printing", "Printing"},
        {"PackageManager", "Package Managers"},
        {"Dialup", "Dial-up"},
        {"InstantMessaging", "Messaging"},
        {"IRCClient", "IRC Clients"},
        {"FileTransfer", "File Transfer"},
        {"HamRadio", "Ham Radio"},
        {"News", "News"},
        {"P2P", "P2P"},
        {"RemoteAccess", "Remote Access"},
        {"Telephony", "Telephony"},
        {"TelephonyTools", "Telephony Tools"},
        {"VideoConference", "Video Conference"},
        {"WebBrowser", "Web Browsers"},
        {"WebDevelopment", "Web Development"},
        {"Midi", "MIDI"},
        {"Mixer", "Mixers"},
        {"Sequencer", "Sequencers"},
        {"Tuner", "Tuners"},
        {"TV", "TV"},
        {"AudioVideoEditing", "Audio/Video Editing"},
        {"Player", "Players"},
        {"Recorder", "Recorders"},
        {"DiscBurning", "Disc Burning"},
        {"Presentation", "Presentations"},
        {"Scanning", "Scanning"},
        {"OCR", "OCR"},
        {"Photography", "Photography"},
        {"Publishing", "Publishing"},
        {"Viewer", "Viewers"},
        {"TextTools", "Text Tools"},
        {"DesktopSettings", "Desktop Settings"},
        {"HardwareSettings", "Hardware Settings"},
        {"Printing", "Printing"},
        {"PackageManager", "Package Managers"},
        {"Dialup", "Dial-up"},
        {"InstantMessaging", "Messaging"},
        {"IRCClient", "IRC Clients"},
        {"FileTransfer", "File Transfer"},
        {"HamRadio", "Ham Radio"},
        {"News", "News"},
        {"P2P", "P2P"},
        {"RemoteAccess", "Remote Access"},
        {"Telephony", "Telephony"},
        {"TelephonyTools", "Telephony Tools"},
        {"VideoConference", "Video Conference"},
        {"WebBrowser", "Web Browsers"},
        {"WebDevelopment", "Web Development"},
        {"Midi", "MIDI"},
        {"Mixer", "Mixers"},
        {"Sequencer", "Sequencers"},
        {"Tuner", "Tuners"},
        {"TV", "TV"},
        {"AudioVideoEditing", "Audio/Video Editing"},
        {"Player", "Players"},
        {"Recorder", "Recorders"},
        {"DiscBurning", "Disc Burning"}
    };
    
    if (categoryMap.contains(category)) {
        return categoryMap[category];
    }
    
    // Return capitalized version if not in map
    if (category.isEmpty()) return category;
    return category.at(0).toUpper() + category.mid(1).toLower();
}

QStringList AppStreamMetadata::getAllCategories() {
    return QStringList({
        "AudioVideo", "Audio", "Video", "Development", "Education", "Game",
        "Graphics", "Network", "Office", "Science", "System", "Utility",
        "Settings", "Accessibility", "TextEditor", "IDE", "WebBrowser",
        "Email", "InstantMessaging", "Chat", "FileManager", "TerminalEmulator"
    });
}

