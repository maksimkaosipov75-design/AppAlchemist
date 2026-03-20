#include "appdirbuilder.h"
#include "appdetector.h"
#include "compatibility_rules.h"
#include "utils.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFile>
#include <QSet>
#include <QTextStream>
#include <QDebug>
#include <QRegularExpression>
#include <algorithm>
#include <limits>

AppDirBuilder::AppDirBuilder() {
}

namespace {

const QSet<QString>& desktopEntryCategories() {
    static const QSet<QString> categories = {
        "AudioVideo", "Audio", "Video", "Development", "Education", "Game",
        "Graphics", "Network", "Office", "Science", "Settings", "System",
        "Utility", "Building", "Debugger", "IDE", "GUIDesigner", "Profiling",
        "RevisionControl", "Translation", "Calendar", "ContactManagement",
        "Database", "Dictionary", "Chart", "Email", "Finance", "FlowChart",
        "PDA", "ProjectManagement", "Presentation", "Spreadsheet",
        "WordProcessor", "2DGraphics", "VectorGraphics", "RasterGraphics",
        "3DGraphics", "Scanning", "OCR", "Photography", "Publishing",
        "Viewer", "TextTools", "DesktopSettings", "HardwareSettings",
        "Printing", "PackageManager", "Dialup", "InstantMessaging", "Chat",
        "IRCClient", "Feed", "FileTransfer", "HamRadio", "News", "P2P",
        "RemoteAccess", "Telephony", "TelephonyTools", "VideoConference",
        "WebBrowser", "WebDevelopment", "Midi", "Mixer", "Sequencer",
        "Tuner", "TV", "AudioVideoEditing", "Player", "Recorder",
        "DiscBurning", "ActionGame", "AdventureGame", "ArcadeGame",
        "BoardGame", "BlocksGame", "CardGame", "KidsGame", "LogicGame",
        "RolePlaying", "Shooter", "Simulation", "SportsGame", "StrategyGame",
        "Art", "Construction", "Music", "Languages", "ArtificialIntelligence",
        "Astronomy", "Biology", "Chemistry", "ComputerScience",
        "DataVisualization", "Economy", "Electricity", "Geography", "Geology",
        "Geoscience", "History", "Humanities", "ImageProcessing", "Literature",
        "Maps", "Math", "NumericalAnalysis", "MedicalSoftware", "Physics",
        "Robotics", "Spirituality", "Sports", "ParallelComputing",
        "Amusement", "Archiving", "Compression", "Electronics",
        "Emulator", "Engineering", "FileTools", "FileManager",
        "TerminalEmulator", "Filesystem", "Monitor", "Security",
        "Accessibility", "Calculator", "Clock", "TextEditor"
    };
    return categories;
}

QString sanitizeDesktopCategories(const QString& categoriesValue) {
    QStringList sanitizedCategories;
    const QStringList categories = categoriesValue.split(';', Qt::SkipEmptyParts);

    for (QString category : categories) {
        category = category.trimmed();
        if (category.isEmpty()) {
            continue;
        }

        if (category.startsWith("X-")) {
            sanitizedCategories << category;
            continue;
        }

        if (desktopEntryCategories().contains(category)) {
            sanitizedCategories << category;
            continue;
        }

        QString extensionCategory = category;
        extensionCategory.remove(QRegularExpression("[^A-Za-z0-9-]"));
        if (extensionCategory.isEmpty()) {
            continue;
        }

        sanitizedCategories << QString("X-%1").arg(extensionCategory);
    }

    sanitizedCategories.removeDuplicates();
    if (sanitizedCategories.isEmpty()) {
        sanitizedCategories << "Utility";
    }

    return QString("Categories=%1;").arg(sanitizedCategories.join(';'));
}

QString normalizeDesktopCandidateName(QString value) {
    value = value.toLower();
    value.replace(QRegularExpression("\\.desktop$"), "");
    value.replace(QRegularExpression("(-|_)(latest|current|stable|installer|linux|amd64|x86_64|all|appimage)$",
                                     QRegularExpression::CaseInsensitiveOption), "");
    return value;
}

int extractIconScore(const QString& iconPath) {
    const QString lowerPath = iconPath.toLower();
    if (lowerPath.contains("1x1") || lowerPath.contains("placeholder")) {
        return -1000;
    }

    if (lowerPath.endsWith(".png")) {
        QFile pngFile(iconPath);
        if (pngFile.open(QIODevice::ReadOnly)) {
            const QByteArray header = pngFile.read(24);
            pngFile.close();
            if (header.size() >= 24 && header.startsWith("\x89PNG\r\n\x1a\n")) {
                auto readBigEndian32 = [](const char* data) -> int {
                    return (static_cast<unsigned char>(data[0]) << 24) |
                           (static_cast<unsigned char>(data[1]) << 16) |
                           (static_cast<unsigned char>(data[2]) << 8) |
                           static_cast<unsigned char>(data[3]);
                };
                const int width = readBigEndian32(header.constData() + 16);
                const int height = readBigEndian32(header.constData() + 20);
                if (width <= 1 || height <= 1) {
                    return -1000;
                }
                return std::max(width, height);
            }
        }
    }

    if (lowerPath.endsWith(".svg")) {
        QFile svgFile(iconPath);
        if (svgFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString svgContent = QString::fromUtf8(svgFile.read(512));
            svgFile.close();
            if (svgContent.contains("width=\"1\"", Qt::CaseInsensitive) ||
                svgContent.contains("height=\"1\"", Qt::CaseInsensitive)) {
                return -1000;
            }
        }
    }

    QRegularExpression tinyIconRegex("(^|[^0-9])(1)x(1)([^0-9]|$)");
    if (tinyIconRegex.match(lowerPath).hasMatch()) {
        return -1000;
    }

    QRegularExpression sizedPathRegex("(\\d+)x(\\d+)");
    const QRegularExpressionMatch sizedPathMatch = sizedPathRegex.match(lowerPath);
    if (sizedPathMatch.hasMatch()) {
        const int width = sizedPathMatch.captured(1).toInt();
        const int height = sizedPathMatch.captured(2).toInt();
        if (width <= 1 || height <= 1) {
            return -1000;
        }
        return std::max(width, height);
    }

    QRegularExpression suffixedSizeRegex("_(\\d+)\\.(png|svg|xpm|ico)$");
    const QRegularExpressionMatch suffixedSizeMatch = suffixedSizeRegex.match(lowerPath);
    if (suffixedSizeMatch.hasMatch()) {
        return suffixedSizeMatch.captured(1).toInt();
    }

    if (lowerPath.endsWith(".svg")) {
        return 512;
    }

    return 64;
}

struct DesktopEntryCandidate {
    QString fileName;
    QString absolutePath;
    int score = 0;
};

DesktopEntryCandidate selectPreferredDesktopEntry(const QDir& applicationsDir, const QString& packageName) {
    DesktopEntryCandidate bestCandidate;
    const QStringList desktopFiles = applicationsDir.entryList({"*.desktop"}, QDir::Files);
    const QString normalizedPackageName = normalizeDesktopCandidateName(packageName);

    for (const QString& desktopFile : desktopFiles) {
        const QString absolutePath = applicationsDir.absoluteFilePath(desktopFile);
        QFile file(absolutePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }

        const QString content = file.readAll();
        const QString lowerFileName = desktopFile.toLower();
        const QString normalizedBaseName = normalizeDesktopCandidateName(QFileInfo(desktopFile).baseName());

        if (!content.contains("Type=Application", Qt::CaseInsensitive)) {
            continue;
        }

        if (lowerFileName.contains("url-handler") || lowerFileName.contains("handler")) {
            continue;
        }

        DesktopEntryCandidate candidate;
        candidate.fileName = desktopFile;
        candidate.absolutePath = absolutePath;

        if (!normalizedPackageName.isEmpty()) {
            if (normalizedBaseName == normalizedPackageName) {
                candidate.score += 120;
            } else if (normalizedBaseName.contains(normalizedPackageName) ||
                       normalizedPackageName.contains(normalizedBaseName)) {
                candidate.score += 75;
            }
        }

        if (!content.contains("NoDisplay=true", Qt::CaseInsensitive)) {
            candidate.score += 40;
        } else {
            candidate.score -= 120;
        }

        if (!content.contains("MimeType=", Qt::CaseInsensitive)) {
            candidate.score += 10;
        } else if (content.contains("x-scheme-handler/", Qt::CaseInsensitive)) {
            candidate.score -= 20;
        }

        if (content.contains("StartupWMClass=", Qt::CaseInsensitive)) {
            candidate.score += 15;
        }

        if (content.contains("Icon=", Qt::CaseInsensitive)) {
            candidate.score += 10;
        }

        if (bestCandidate.absolutePath.isEmpty() || candidate.score > bestCandidate.score) {
            bestCandidate = candidate;
        }
    }

    return bestCandidate;
}

QString selectBestIcon(const QStringList& candidates, const QStringList& preferredNames) {
    QString bestPath;
    int bestScore = std::numeric_limits<int>::min();

    for (const QString& candidatePath : candidates) {
        const QFileInfo info(candidatePath);
        if (!info.exists() || !info.isFile()) {
            continue;
        }

        int score = extractIconScore(candidatePath);
        if (score < 0) {
            continue;
        }

        const QString lowerFileName = info.fileName().toLower();
        for (int index = 0; index < preferredNames.size(); ++index) {
            const QString preferred = preferredNames.at(index).toLower();
            if (preferred.isEmpty()) {
                continue;
            }

            if (lowerFileName == QString("%1.%2").arg(preferred, info.suffix().toLower())) {
                score += 200 - (index * 10);
            } else if (lowerFileName.contains(preferred)) {
                score += 120 - (index * 10);
            }
        }

        if (bestPath.isEmpty() || score > bestScore) {
            bestPath = candidatePath;
            bestScore = score;
        }
    }

    return bestPath;
}

QStringList collectMatchingFilesRecursively(const QString& rootPath, const QStringList& nameFilters) {
    QStringList matches;
    QDir rootDir(rootPath);
    if (!rootDir.exists()) {
        return matches;
    }

    QDirIterator it(rootPath, nameFilters, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        matches.append(it.next());
    }

    return matches;
}

}

bool AppDirBuilder::createDirectoryStructure(const QString& appDirPath) {
    QStringList dirs = {
        QString("%1/usr/bin").arg(appDirPath),
        QString("%1/usr/lib").arg(appDirPath),
        QString("%1/usr/share/applications").arg(appDirPath),
        QString("%1/usr/share/icons/hicolor/256x256/apps").arg(appDirPath),
        QString("%1/usr/share/icons/hicolor/128x128/apps").arg(appDirPath),
        QString("%1/usr/share/icons/hicolor/64x64/apps").arg(appDirPath),
        QString("%1/usr/share/icons/hicolor/48x48/apps").arg(appDirPath),
        QString("%1/usr/share/icons/hicolor/32x32/apps").arg(appDirPath),
        QString("%1/usr/share/icons/hicolor/16x16/apps").arg(appDirPath)
    };
    
    for (const QString& dir : dirs) {
        if (!SubprocessWrapper::createDirectory(dir)) {
            return false;
        }
    }
    
    return true;
}

bool AppDirBuilder::buildAppDir(const QString& appDirPath,
                                const QString& extractedDebDir,
                                const PackageMetadata& metadata,
                                const QStringList& libraries) {
    qDebug() << "Building AppDir at:" << appDirPath;
    qDebug() << "Extracted deb dir:" << extractedDebDir;
    qDebug() << "Number of executables:" << metadata.executables.size();
    if (metadata.executables.size() > 50) {
        qDebug() << "WARNING: Too many executables found, filtering will be applied";
        qDebug() << "First 10 executables:" << metadata.executables.mid(0, 10);
    }
    
    // Create directory structure
    if (!createDirectoryStructure(appDirPath)) {
        qWarning() << "Failed to create directory structure";
        return false;
    }
    
    // Copy executables
    if (!copyExecutables(appDirPath, extractedDebDir, metadata.executables)) {
        qWarning() << "Failed to copy executables";
        return false;
    }
    qDebug() << "Executables copied successfully";
    
    // Copy libraries
    if (!copyLibraries(appDirPath, libraries)) {
        qWarning() << "Failed to copy libraries";
        return false;
    }
    qDebug() << "Libraries copied successfully";
    
    // Copy resources (but skip usr/share/applications to avoid overwriting .desktop)
    if (!copyResources(appDirPath, extractedDebDir)) {
        qWarning() << "Failed to copy resources";
        return false;
    }
    qDebug() << "Resources copied successfully";
    
    // UNIVERSAL: After copying resources, check for Python applications and ensure Python modules are copied
    QString mainExec = metadata.mainExecutable.isEmpty() ? 
                      (metadata.executables.isEmpty() ? "" : metadata.executables.first()) : 
                      metadata.mainExecutable;
    if (!mainExec.isEmpty() && AppDetector::isPython(mainExec)) {
        qDebug() << "Detected Python application, ensuring Python modules are copied...";
        
        // Copy Python modules from extracted package
        QStringList pythonSourceDirs = {
            QString("%1/data/usr/lib/python3").arg(extractedDebDir),
            QString("%1/usr/lib/python3").arg(extractedDebDir),
            QString("%1/data/usr/lib/python3/dist-packages").arg(extractedDebDir),
            QString("%1/usr/lib/python3/dist-packages").arg(extractedDebDir),
            QString("%1/data/usr/share/python3").arg(extractedDebDir),
            QString("%1/usr/share/python3").arg(extractedDebDir)
        };
        
        for (const QString& pythonSource : pythonSourceDirs) {
            if (QDir(pythonSource).exists()) {
                QString pythonTarget = pythonSource;
                if (pythonTarget.contains("/data/")) {
                    pythonTarget = pythonTarget.section("/data/", 1);
                }
                pythonTarget = QString("%1/%2").arg(appDirPath).arg(pythonTarget);
                
                qDebug() << "Copying Python modules from" << pythonSource << "to" << pythonTarget;
                QDir targetDir(pythonTarget);
                if (!targetDir.exists()) {
                    targetDir.mkpath(".");
                }
                if (!SubprocessWrapper::copyDirectory(pythonSource, pythonTarget)) {
                    qWarning() << "Failed to copy Python modules from" << pythonSource;
                } else {
                    qDebug() << "Successfully copied Python modules to" << pythonTarget;
                }
            }
        }
    }
    
    // UNIVERSAL: After copying resources, explicitly copy missing files for Electron apps
    // This is a workaround for copyDirectory potentially missing root-level files
    QString electronBaseDir = AppDetector::findElectronBaseDir(appDirPath);
    if (!electronBaseDir.isEmpty()) {
        qDebug() << "Post-processing Electron app directory:" << electronBaseDir;
        QString foundAppDir = QString("%1/%2").arg(appDirPath).arg(electronBaseDir);
        
        // Try to find source directory (could be in data/usr/share or usr/share or opt)
        QString sourceAppDir = QString("%1/data/%2").arg(extractedDebDir).arg(electronBaseDir);
        if (!QDir(sourceAppDir).exists()) {
            sourceAppDir = QString("%1/%2").arg(extractedDebDir).arg(electronBaseDir);
        }
        QDir sourceAppDirObj(sourceAppDir);
        if (sourceAppDirObj.exists()) {
            // Copy all root-level files that might be missing
            QStringList rootFiles = sourceAppDirObj.entryList(QDir::Files | QDir::NoSymLinks);
            for (const QString& file : rootFiles) {
                QString destFile = QDir(foundAppDir).absoluteFilePath(file);
                if (!QFile::exists(destFile)) {
                    QString srcFile = sourceAppDirObj.absoluteFilePath(file);
                    qDebug() << "Copying missing root file:" << file;
                    if (QFile::copy(srcFile, destFile)) {
                        qDebug() << "  ✓ Copied" << file;
                    } else {
                        qWarning() << "  ✗ Failed to copy" << file;
                    }
                }
            }
            // Copy resources directory if missing
            if (!QDir(foundAppDir).exists("resources")) {
                QString srcResources = sourceAppDirObj.absoluteFilePath("resources");
                if (QDir(srcResources).exists()) {
                    qDebug() << "Copying missing resources directory...";
                    QString destResources = QDir(foundAppDir).absoluteFilePath("resources");
                    if (SubprocessWrapper::copyDirectory(srcResources, destResources)) {
                        qDebug() << "  ✓ Copied resources directory";
                    } else {
                        qWarning() << "  ✗ Failed to copy resources directory";
                    }
                }
            }
        } else {
            qWarning() << "Source Electron app directory not found:" << sourceAppDir;
        }
    }
    
    // Try to copy .desktop file from extracted package first (AFTER resources to ensure directory exists)
    // For DEB: files are in data/usr/share/applications
    // For RPM: files may be directly in usr/share/applications (if extracted without data/ prefix)
    QString desktopSource = QString("%1/data/usr/share/applications").arg(extractedDebDir);
    QString desktopSourceAlt = QString("%1/usr/share/applications").arg(extractedDebDir);  // For RPM without data/
    QDir desktopSourceDir(desktopSource);
    bool desktopCopied = false;
    
    // Try data/usr/share/applications first (DEB), then usr/share/applications (RPM)
    if (!desktopSourceDir.exists() && QDir(desktopSourceAlt).exists()) {
        desktopSource = desktopSourceAlt;
        desktopSourceDir = QDir(desktopSource);
        qDebug() << "Using alternative desktop path (RPM format):" << desktopSource;
    }
    
    qDebug() << "Looking for .desktop file in:" << desktopSource;
    
    if (desktopSourceDir.exists()) {
        QStringList desktopFiles = desktopSourceDir.entryList({"*.desktop"}, QDir::Files);
        qDebug() << "Found .desktop files in package:" << desktopFiles;
        
        if (!desktopFiles.isEmpty()) {
            const DesktopEntryCandidate preferredDesktop = selectPreferredDesktopEntry(desktopSourceDir, metadata.package);
            const QString selectedDesktopFile = preferredDesktop.fileName.isEmpty() ? desktopFiles.first() : preferredDesktop.fileName;
            QString sourceDesktop = preferredDesktop.absolutePath.isEmpty()
                ? desktopSourceDir.absoluteFilePath(selectedDesktopFile)
                : preferredDesktop.absolutePath;
            QString targetDesktopDir = QString("%1/usr/share/applications").arg(appDirPath);
            QDir targetDesktopDirObj(targetDesktopDir);
            if (!targetDesktopDirObj.exists()) {
                targetDesktopDirObj.mkpath(".");
            }
            QString targetDesktop = QString("%1/%2").arg(targetDesktopDir).arg(selectedDesktopFile);
            
            qDebug() << "Copying .desktop from" << sourceDesktop << "to" << targetDesktop;
            
            if (SubprocessWrapper::copyFile(sourceDesktop, targetDesktop)) {
                qDebug() << "Successfully copied .desktop file:" << selectedDesktopFile;
                
                // Verify and fix .desktop file (ensure Categories= is present)
                if (QFileInfo::exists(targetDesktop)) {
                    qDebug() << "Verified .desktop file exists at:" << targetDesktop;
                    if (!fixDesktopFile(targetDesktop, metadata)) {
                        qWarning() << "Failed to fix .desktop file, will create new one";
                        QFile::remove(targetDesktop);
                        desktopCopied = false;
                    } else {
                        desktopCopied = true;
                    }
                } else {
                    qWarning() << "WARNING: .desktop file was copied but doesn't exist at:" << targetDesktop;
                    desktopCopied = false;
                }
            } else {
                qWarning() << "Failed to copy .desktop file from package";
            }
        }
    } else {
        qDebug() << "Desktop source directory does not exist:" << desktopSource;
    }
    
    // Create .desktop file if not copied from package
    if (!desktopCopied) {
        qDebug() << "Creating new .desktop file";
        if (!createDesktopFile(appDirPath, metadata)) {
            qWarning() << "Failed to create desktop file";
            return false;
        }
        qDebug() << "Desktop file created successfully";
    } else {
        qDebug() << "Desktop file copied from package";
    }
    
    // Final verification - list all .desktop files
    QString finalDesktopDir = QString("%1/usr/share/applications").arg(appDirPath);
    QDir finalDesktopDirObj(finalDesktopDir);
    if (finalDesktopDirObj.exists()) {
        QStringList finalDesktopFiles = finalDesktopDirObj.entryList({"*.desktop"}, QDir::Files);
        qDebug() << "Final .desktop files in AppDir:" << finalDesktopFiles;
        for (const QString& file : finalDesktopFiles) {
            QString fullPath = finalDesktopDirObj.absoluteFilePath(file);
            qDebug() << "  -" << fullPath << "(exists:" << QFileInfo::exists(fullPath) << ")";
            
            // CRITICAL: appimagetool requires .desktop file in AppDir root!
            // Copy .desktop file to AppDir root
            QString rootDesktopPath = QString("%1/%2").arg(appDirPath).arg(file);
            if (SubprocessWrapper::copyFile(fullPath, rootDesktopPath)) {
                qDebug() << "Copied .desktop file to AppDir root:" << rootDesktopPath;
                
                // Also ensure icon path in root .desktop file points to root icon
                // Read and fix icon path in root .desktop
                QFile rootDesktopFile(rootDesktopPath);
                if (rootDesktopFile.open(QIODevice::ReadWrite | QIODevice::Text)) {
                    QString content = rootDesktopFile.readAll();
                    rootDesktopFile.close();
                    
                    // Fix Icon= path to point to root icon (without path, just name)
                    QRegularExpression iconRegex("(?i)^Icon=(.+)$", QRegularExpression::MultilineOption);
                    QRegularExpressionMatch match = iconRegex.match(content);
                    if (match.hasMatch()) {
                        QString iconValue = match.captured(1).trimmed();
                        // Remove path, keep only name
                        QString iconName = QFileInfo(iconValue).baseName();
                        if (!iconName.isEmpty()) {
                            content.replace(iconRegex, QString("Icon=%1").arg(iconName));
                            
                            if (rootDesktopFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                                QTextStream out(&rootDesktopFile);
                                out << content;
                                rootDesktopFile.close();
                            }
                            qDebug() << "Fixed Icon= path in root .desktop file:" << iconName;
                        }
                    }
                }
            } else {
                qWarning() << "Failed to copy .desktop file to AppDir root:" << rootDesktopPath;
            }
        }
    } else {
        qWarning() << "ERROR: Desktop directory does not exist:" << finalDesktopDir;
    }
    
    // Determine icon name from .desktop file first
    QString iconNameFromDesktop;
    if (finalDesktopDirObj.exists()) {
        QStringList finalDesktopFiles = finalDesktopDirObj.entryList({"*.desktop"}, QDir::Files);
        if (!finalDesktopFiles.isEmpty()) {
            QString desktopPath = finalDesktopDirObj.absoluteFilePath(finalDesktopFiles.first());
            QFile desktopFile(desktopPath);
            if (desktopFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&desktopFile);
                while (!in.atEnd()) {
                    QString line = in.readLine().trimmed();
                    if (line.startsWith("Icon=", Qt::CaseInsensitive)) {
                        iconNameFromDesktop = line.mid(5).trimmed();
                        // Remove path, keep only name
                        iconNameFromDesktop = QFileInfo(iconNameFromDesktop).baseName();
                        qDebug() << "Found Icon= in .desktop file:" << iconNameFromDesktop;
                        break;
                    }
                }
                desktopFile.close();
            }
        }
    }
    
    // Copy icon (CRITICAL: appimagetool requires icon in AppDir root)
    QString rootIconName;
    QString iconNameToUse = iconNameFromDesktop.isEmpty() ? metadata.package : iconNameFromDesktop;
    
    if (!metadata.iconPath.isEmpty()) {
        if (!copyIcon(appDirPath, metadata.iconPath, metadata)) {
            qWarning() << "Failed to copy icon, continuing anyway";
        }
        
        // Also copy icon to AppDir root (required by appimagetool)
        QFileInfo iconInfo(metadata.iconPath);
        QString iconExt = iconInfo.suffix();
        if (iconExt.isEmpty()) {
            // Try to determine extension from file name
            QString fileName = iconInfo.fileName();
            int lastDot = fileName.lastIndexOf('.');
            if (lastDot > 0) {
                iconExt = fileName.mid(lastDot + 1);
            } else {
                iconExt = "png"; // Default
            }
        }
        
        // Try different icon names: from desktop, then package name
        QStringList iconNamesToTry = {iconNameToUse, metadata.package};
        bool iconCopied = false;
        
        for (const QString& name : iconNamesToTry) {
            rootIconName = QString("%1.%2").arg(name).arg(iconExt);
            QString rootIconPath = QString("%1/%2").arg(appDirPath).arg(rootIconName);
            
            if (SubprocessWrapper::copyFile(metadata.iconPath, rootIconPath)) {
                qDebug() << "Copied icon to AppDir root:" << rootIconPath;
                iconCopied = true;
                break;
            }
        }
        
        if (!iconCopied) {
            qWarning() << "Failed to copy icon to AppDir root";
            rootIconName.clear();
        }
    } else {
        // Try to find icon in standard locations
        QStringList iconSearchPaths = {
            QString("%1/data/usr/share/pixmaps").arg(extractedDebDir),
            QString("%1/data/usr/share/icons/hicolor/256x256/apps").arg(extractedDebDir),
            QString("%1/data/usr/share/icons/hicolor/128x128/apps").arg(extractedDebDir),
            QString("%1/data/usr/share/icons/hicolor/64x64/apps").arg(extractedDebDir),
            QString("%1/data/usr/share/icons/hicolor/48x48/apps").arg(extractedDebDir),
            QString("%1/data/usr/share/icons/hicolor/32x32/apps").arg(extractedDebDir),
            QString("%1/data/usr/share/icons/hicolor/16x16/apps").arg(extractedDebDir),
            QString("%1/data/usr/share/icons").arg(extractedDebDir),
            QString("%1/data/opt").arg(extractedDebDir)
        };
        
        QStringList iconExtensions = {"png", "svg", "xpm", "ico"};
        bool iconFound = false;
        QStringList preferredIconNames = {iconNameToUse, metadata.package};
        QStringList iconCandidates;
        
        qDebug() << "Searching for icon with name:" << iconNameToUse;

        for (const QString& searchPath : iconSearchPaths) {
            for (const QString& extension : iconExtensions) {
                iconCandidates.append(collectMatchingFilesRecursively(searchPath, {QString("*.%1").arg(extension)}));
            }
        }

        const QString selectedIconPath = selectBestIcon(iconCandidates, preferredIconNames);
        if (!selectedIconPath.isEmpty()) {
            QFileInfo foundIconInfo(selectedIconPath);
            const QString iconExt = foundIconInfo.suffix().isEmpty() ? "png" : foundIconInfo.suffix();
            rootIconName = QString("%1.%2").arg(iconNameToUse).arg(iconExt);
            QString rootIconPath = QString("%1/%2").arg(appDirPath).arg(rootIconName);
            if (SubprocessWrapper::copyFile(selectedIconPath, rootIconPath)) {
                qDebug() << "Found and copied best icon:" << rootIconPath << "from" << selectedIconPath;
                iconFound = true;
            }
        }
        
        if (!iconFound) {
            qWarning() << "WARNING: No icon found for" << iconNameToUse;
            qWarning() << "Searched in:" << iconSearchPaths;
            qWarning() << "Creating a placeholder icon to satisfy appimagetool requirements";
            
            // Create a minimal 1x1 PNG as placeholder
            // This is a minimal valid PNG file (1x1 transparent pixel)
            QByteArray placeholderPng = QByteArray::fromBase64(
                "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
            );
            
            rootIconName = QString("%1.png").arg(iconNameToUse);
            QString rootIconPath = QString("%1/%2").arg(appDirPath).arg(rootIconName);
            
            QFile placeholderFile(rootIconPath);
            if (placeholderFile.open(QIODevice::WriteOnly)) {
                placeholderFile.write(placeholderPng);
                placeholderFile.close();
                qDebug() << "Created placeholder icon:" << rootIconPath;
                iconFound = true;
            } else {
                qWarning() << "Failed to create placeholder icon:" << rootIconPath;
            }
        }
    }
    
    // Update .desktop file to use correct icon name (if we have one)
    if (!rootIconName.isEmpty() && finalDesktopDirObj.exists()) {
        QStringList finalDesktopFiles = finalDesktopDirObj.entryList({"*.desktop"}, QDir::Files);
        for (const QString& file : finalDesktopFiles) {
            QString desktopPath = finalDesktopDirObj.absoluteFilePath(file);
            QString rootDesktopPath = QString("%1/%2").arg(appDirPath).arg(file);
            
            // Update Icon= in both .desktop files
            for (const QString& path : {desktopPath, rootDesktopPath}) {
                if (QFileInfo::exists(path)) {
                    QFile desktopFile(path);
                    if (desktopFile.open(QIODevice::ReadWrite | QIODevice::Text)) {
                        QString content = desktopFile.readAll();
                        desktopFile.close();
                        
                        // Replace Icon= with just the icon name (no path, no extension in Icon=)
                        QString iconNameOnly = QFileInfo(rootIconName).baseName();
                        QRegularExpression iconRegex("(?i)^Icon=(.+)$", QRegularExpression::MultilineOption);
                        content.replace(iconRegex, QString("Icon=%1").arg(iconNameOnly));
                        
                        if (desktopFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                            QTextStream out(&desktopFile);
                            out << content;
                            desktopFile.close();
                            qDebug() << "Updated Icon= in .desktop file:" << path << "to" << iconNameOnly;
                        }
                    }
                }
            }
        }
    }
    
    // Create AppRun
    if (!createAppRun(appDirPath, metadata)) {
        qWarning() << "Failed to create AppRun";
        return false;
    }
    qDebug() << "AppRun created successfully";
    
    // Create symlink to main executable (skip for jar files, they're handled by AppRun)
    // Wrap in try-catch to prevent crashes
    try {
        if (!metadata.mainExecutable.isEmpty()) {
            QFileInfo execInfo(metadata.mainExecutable);
            if (execInfo.exists()) {
                QString execName = execInfo.fileName();
                
                // Safety check: ensure execName is not empty and doesn't contain dangerous characters
                if (!execName.isEmpty() && !execName.contains("..") && !execName.contains("/")) {
                    // Don't create symlink for jar files
                    if (!execName.endsWith(".jar")) {
                        QString symlinkPath = QString("%1/%2").arg(appDirPath).arg(execName);
                        QString targetPath;
                        
                        // Determine correct target path based on where executable was placed
                        if (metadata.mainExecutable.contains("/usr/games/")) {
                            targetPath = QString("usr/games/%1").arg(execName);
                        } else if (metadata.mainExecutable.contains("/opt/")) {
                            targetPath = QString("opt/%1").arg(execName);
                        } else {
                            targetPath = QString("usr/bin/%1").arg(execName);
                        }
                        
                        // Verify target exists before creating symlink
                        QString fullTargetPath = QString("%1/%2").arg(appDirPath).arg(targetPath);
                        if (QFileInfo::exists(fullTargetPath)) {
                            QFile::remove(symlinkPath);
                            if (!QFile::link(targetPath, symlinkPath)) {
                                // Fallback: copy instead of symlink
                                QString sourcePath = QString("%1/%2").arg(appDirPath).arg(targetPath);
                                if (QFileInfo::exists(sourcePath)) {
                                    SubprocessWrapper::copyFile(sourcePath, symlinkPath);
                                    SubprocessWrapper::setExecutable(symlinkPath);
                                }
                            }
                        } else {
                            qWarning() << "Target path does not exist for symlink:" << fullTargetPath;
                        }
                    }
                } else {
                    qWarning() << "Invalid executable name for symlink:" << execName;
                }
            } else {
                qWarning() << "Main executable does not exist:" << metadata.mainExecutable;
            }
        }
    } catch (...) {
        qWarning() << "Exception occurred while creating symlink, continuing...";
        // Don't fail the entire build if symlink creation fails
    }
    
    return true;
}

bool AppDirBuilder::copyExecutables(const QString& appDirPath,
                                    const QString& extractedDebDir,
                                    const QStringList& executables) {
    qDebug() << "Copying" << executables.size() << "executables";
    
    int copied = 0;
    int failed = 0;
    
    for (const QString& exec : executables) {
        QFileInfo execInfo(exec);
        if (!execInfo.exists()) {
            qWarning() << "Executable does not exist:" << exec;
            failed++;
            continue;
        }
        
        qDebug() << "Copying executable:" << exec;
        
        QString relativePath = exec;
        
        // Remove extractDir prefix (may be /extracted or /extracted/data)
        if (relativePath.startsWith(extractedDebDir)) {
            relativePath = relativePath.mid(extractedDebDir.length());
            if (relativePath.startsWith("/")) {
                relativePath = relativePath.mid(1);
            }
            // Remove "data/" prefix if present
            if (relativePath.startsWith("data/")) {
                relativePath = relativePath.mid(5);
            }
        } else if (relativePath.contains("/data/")) {
            // Extract path after /data/
            int dataPos = relativePath.indexOf("/data/");
            relativePath = relativePath.mid(dataPos + 6);
        }
        
        // Map to AppDir structure - preserve original directory structure
        QString targetPath;
        if (relativePath.startsWith("usr/bin/") || relativePath.startsWith("usr/sbin/")) {
            targetPath = QString("%1/%2").arg(appDirPath).arg(relativePath);
        } else if (relativePath.startsWith("usr/games/")) {
            targetPath = QString("%1/%2").arg(appDirPath).arg(relativePath);
        } else if (relativePath.startsWith("opt/")) {
            // Preserve opt/ structure (important for Chrome, Steam, etc.)
            targetPath = QString("%1/%2").arg(appDirPath).arg(relativePath);
        } else if (relativePath.startsWith("usr/lib/")) {
            targetPath = QString("%1/%2").arg(appDirPath).arg(relativePath);
        } else {
            // Put in usr/bin (or usr/games for jar files)
            if (execInfo.suffix().toLower() == "jar") {
                targetPath = QString("%1/usr/games/%2").arg(appDirPath).arg(execInfo.fileName());
            } else {
                targetPath = QString("%1/usr/bin/%2").arg(appDirPath).arg(execInfo.fileName());
            }
        }
        
        // Create target directory if needed
        QFileInfo targetInfo(targetPath);
        QDir targetDir = targetInfo.dir();
        if (!targetDir.exists()) {
            if (!targetDir.mkpath(".")) {
                qWarning() << "Failed to create directory:" << targetDir.absolutePath();
                return false;
            }
        }
        
        // UNIVERSAL: Check if it's a shell script that needs path modification
        QString execName = execInfo.fileName();
        bool isShellScript = AppDetector::isScript(exec);
        
        if (isShellScript) {
            // Read script, modify paths, write to target
            QFile sourceFile(exec);
            if (!sourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                qWarning() << "Failed to open script for reading:" << exec;
                failed++;
                continue;
            }
            
            QString content = sourceFile.readAll();
            sourceFile.close();
            
            // UNIVERSAL: Replace absolute paths with relative ones for AppImage
            // Determine app base directory from executable path
            QString appBaseDir;
            if (exec.contains("/usr/share/")) {
                // Extract usr/share/appname
                int sharePos = exec.indexOf("/usr/share/");
                if (sharePos >= 0) {
                    QString afterShare = exec.mid(sharePos + 1);
                    appBaseDir = afterShare.section('/', 0, 1); // e.g., "usr/share/code"
                }
            } else if (exec.contains("/opt/")) {
                appBaseDir = exec.section("/opt/", 1).section('/', 0, 0);
                appBaseDir = "opt/" + appBaseDir;
            } else if (exec.contains("/usr/lib/")) {
                int libPos = exec.indexOf("/usr/lib/");
                if (libPos >= 0) {
                    QString afterLib = exec.mid(libPos + 1);
                    appBaseDir = afterLib.section('/', 0, 1); // e.g., "usr/lib/yandex-music"
                }
            }
            
            // Use universal path replacement
            content = AppDetector::replaceScriptPaths(content, appBaseDir);
            
            // Fix shebang lines that contain ${HERE} - variables don't work in shebang
            // Replace #!${HERE}/usr/bin/env with #!/usr/bin/env
            // Replace #!${HERE}/bin/bash with #!/bin/bash
            // Replace #!${HERE}/usr/bin/bash with #!/usr/bin/bash
            content.replace(QRegularExpression(R"(^#!\$\{HERE\}/usr/bin/env\s+)"), "#!/usr/bin/env ");
            content.replace(QRegularExpression(R"(^#!\$\{HERE\}/bin/bash\s*)"), "#!/bin/bash");
            content.replace(QRegularExpression(R"(^#!\$\{HERE\}/usr/bin/bash\s*)"), "#!/usr/bin/bash");
            content.replace(QRegularExpression(R"(^#!\$\{HERE\}/bin/sh\s*)"), "#!/bin/sh");
            content.replace(QRegularExpression(R"(^#!\$\{HERE\}/usr/bin/sh\s*)"), "#!/usr/bin/sh");
            
            QFile targetFile(targetPath);
            if (!targetFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                qWarning() << "Failed to open script for writing:" << targetPath;
                failed++;
                continue;
            }
            
            QTextStream out(&targetFile);
            out << content;
            targetFile.close();
        } else {
            // Regular file copy
            // Use hardlink if source is in /tmp (RAM-optimized), otherwise copy
            bool success = false;
            if (exec.startsWith("/tmp/")) {
                success = SubprocessWrapper::createHardLink(exec, targetPath);
            } else {
                success = SubprocessWrapper::copyFile(exec, targetPath);
            }
            if (!success) {
                qWarning() << "Failed to copy/link executable from" << exec << "to" << targetPath;
                failed++;
                // Don't fail completely, continue with other files
                continue;
            }
        }
        
        copied++;
        
        // Set executable only for non-jar files
        if (!execInfo.suffix().toLower().endsWith("jar")) {
            if (!SubprocessWrapper::setExecutable(targetPath)) {
                qWarning() << "Failed to set executable permissions:" << targetPath;
            }
        }
    }
    
    qDebug() << "Copied" << copied << "executables," << failed << "failed";
    
    // Return true if at least some executables were copied
    return copied > 0;
}

bool AppDirBuilder::copyLibraries(const QString& appDirPath, const QStringList& libraries) {
    for (const QString& lib : libraries) {
        QFileInfo libInfo(lib);
        if (!libInfo.exists()) {
            qWarning() << "Library does not exist:" << lib;
            continue;
        }
        
        QString libName = libInfo.fileName();
        QString libPath = lib;
        
        // Determine target path - preserve directory structure for opt/ libraries
        QString targetPath;
        if (libPath.contains("/opt/google/chrome/")) {
            // Preserve opt/google/chrome structure
            int optPos = libPath.indexOf("/opt/");
            QString afterOpt = libPath.mid(optPos + 1);
            targetPath = QString("%1/%2").arg(appDirPath).arg(afterOpt);
        } else if (libPath.contains("/opt/")) {
            // Preserve other opt/ structures
            int optPos = libPath.indexOf("/opt/");
            QString afterOpt = libPath.mid(optPos + 1);
            if (afterOpt.contains("/data/")) {
                afterOpt = afterOpt.section("/data/", 1);
            }
            targetPath = QString("%1/%2").arg(appDirPath).arg(afterOpt);
        } else {
            // Standard library location
            targetPath = QString("%1/usr/lib/%2").arg(appDirPath).arg(libName);
        }
        
        // Create target directory if needed
        QFileInfo targetInfo(targetPath);
        QDir targetDir = targetInfo.dir();
        if (!targetDir.exists()) {
            targetDir.mkpath(".");
        }
        
        // Use hardlink if source is in /tmp (RAM-optimized), otherwise copy
        bool success = false;
        if (lib.startsWith("/tmp/")) {
            success = SubprocessWrapper::createHardLink(lib, targetPath);
        } else {
            success = SubprocessWrapper::copyFile(lib, targetPath);
        }
        if (!success) {
            qWarning() << "Failed to copy/link library:" << lib << "to" << targetPath;
            continue;
        }
    }
    
    return true;
}

bool AppDirBuilder::copyResources(const QString& appDirPath, const QString& extractedDebDir) {
    QStringList resourceDirs = {
        "usr/lib",
        "usr/games",
        // Note: usr/games is handled separately to preserve structure for Java apps
        "opt"
    };
    
    // Copy usr/share but exclude applications directory (we'll handle .desktop separately)
    // For DEB: files are in data/usr/share
    // For RPM: files may be directly in usr/share (if extracted without data/ prefix)
    QString shareSource = QString("%1/data/usr/share").arg(extractedDebDir);
    QString shareSourceAlt = QString("%1/usr/share").arg(extractedDebDir);  // For RPM without data/
    QString shareTarget = QString("%1/usr/share").arg(appDirPath);
    
    // Try data/usr/share first (DEB), then usr/share (RPM)
    if (!QDir(shareSource).exists() && QDir(shareSourceAlt).exists()) {
        shareSource = shareSourceAlt;
        qDebug() << "Using alternative share path (RPM format):" << shareSource;
    }
    
    if (QDir(shareSource).exists()) {
        QDir shareTargetDir(shareTarget);
        if (!shareTargetDir.exists()) {
            shareTargetDir.mkpath(".");
        }
        
        // Copy usr/share subdirectories except applications
        // Use copyDirectory which copies both files and subdirectories recursively
        QDir shareSourceDir(shareSource);
        QStringList shareSubdirs = shareSourceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        qDebug() << "Found usr/share subdirectories:" << shareSubdirs;
        qDebug() << "Share source directory exists:" << shareSourceDir.exists() << "at:" << shareSource;
        
        // First, verify source directory structure
        if (shareSourceDir.exists()) {
            QStringList allEntries = shareSourceDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
            qDebug() << "All entries in share source:" << allEntries.size() << "items";
            if (allEntries.size() < 10) {
                qDebug() << "Entries:" << allEntries;
            }
        }
        
        for (const QString& subdir : shareSubdirs) {
            if (subdir == "applications") {
                // Skip applications, we'll handle .desktop separately
                continue;
            }
            QString srcPath = shareSourceDir.absoluteFilePath(subdir);
            QString destPath = shareTargetDir.absoluteFilePath(subdir);
            
            qDebug() << "Copying share resource directory:" << srcPath << "->" << destPath;
            
            // Verify source exists and has content
            QDir srcDir(srcPath);
            if (!srcDir.exists()) {
                qWarning() << "Source directory does not exist:" << srcPath;
                continue;
            }
            
            // Count files in source before copying
            int srcFileCount = srcDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size();
            qDebug() << "Source directory has" << srcFileCount << "items";
            
            // copyDirectory will copy all files and subdirectories recursively
            // This includes files in the root of the directory (e.g., codium/snapshot_blob.bin)
            if (!SubprocessWrapper::copyDirectory(srcPath, destPath)) {
                qWarning() << "Failed to copy share resource:" << srcPath;
            } else {
                // Verify copy was successful
                QDir verifyDir(destPath);
                if (!verifyDir.exists()) {
                    qWarning() << "Destination directory was not created:" << destPath;
                    continue;
                }
                int fileCount = verifyDir.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name).size();
                qDebug() << "Successfully copied" << subdir << "-" << fileCount << "items (files + dirs)";
                
                if (fileCount == 0) {
                    qWarning() << "WARNING: Directory copied but is empty! Source had" << srcFileCount << "items";
                } else if (fileCount < srcFileCount / 2) {
                    qWarning() << "WARNING: Only copied" << fileCount << "of" << srcFileCount << "items from source!";
                }
                
                // Also verify that key files are present (for Electron apps)
                if (subdir.contains("codium", Qt::CaseInsensitive) || 
                    subdir.contains("code", Qt::CaseInsensitive) ||
                    subdir.contains("code-oss", Qt::CaseInsensitive)) {
                    // CRITICAL: copyDirectory may not copy files in the root of the directory
                    // We need to explicitly copy files from the root of srcDir to verifyDir
                    QStringList rootFiles = srcDir.entryList(QDir::Files | QDir::NoSymLinks);
                    qDebug() << "  Checking root files in source:" << rootFiles.size() << "files";
                    
                    // Copy all root files that are not already copied
                    for (const QString& rootFile : rootFiles) {
                        QString destRootFile = verifyDir.absoluteFilePath(rootFile);
                        if (!QFile::exists(destRootFile)) {
                            QString srcRootFile = srcDir.absoluteFilePath(rootFile);
                            qDebug() << "  Copying root file:" << rootFile;
                            if (QFile::copy(srcRootFile, destRootFile)) {
                                qDebug() << "    ✓ Copied" << rootFile;
                            } else {
                                qWarning() << "    ✗ Failed to copy" << rootFile;
                            }
                        }
                    }
                    
                    // Verify key files
                    QStringList keyFiles = {"snapshot_blob.bin", "v8_context_snapshot.bin", "resources.pak"};
                    for (const QString& keyFile : keyFiles) {
                        QString keyFilePath = verifyDir.absoluteFilePath(keyFile);
                        if (QFile::exists(keyFilePath)) {
                            qDebug() << "  ✓ Key file found:" << keyFile;
                        } else {
                            qWarning() << "  ✗ Key file missing:" << keyFile;
                        }
                    }
                    if (verifyDir.exists("resources")) {
                        qDebug() << "  ✓ Resources directory found";
                    } else {
                        qWarning() << "  ✗ Resources directory missing";
                    }
                }
            }
        }
    }
    
    // Copy other resource directories
    for (const QString& resourceDir : resourceDirs) {
        // Try data/ prefix first (DEB), then direct path (RPM)
        QString sourcePath = QString("%1/data/%2").arg(extractedDebDir).arg(resourceDir);
        QString sourcePathAlt = QString("%1/%2").arg(extractedDebDir).arg(resourceDir);
        QString targetPath = QString("%1/%2").arg(appDirPath).arg(resourceDir);
        
        if (!QDir(sourcePath).exists() && QDir(sourcePathAlt).exists()) {
            sourcePath = sourcePathAlt;
            qDebug() << "Using alternative resource path (RPM format):" << sourcePath;
        }
        
        if (QDir(sourcePath).exists()) {
            // Create target directory
            QDir targetDir(targetPath);
            if (!targetDir.exists()) {
                targetDir.mkpath(".");
            }
            
            // For usr/games, copy only subdirectories that don't already exist
            // This prevents overwriting jar files and other executables already copied
            if (resourceDir == "usr/games") {
                QDir sourceDir(sourcePath);
                QStringList subdirs = sourceDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                for (const QString& subdir : subdirs) {
                    QString subdirSource = sourceDir.absoluteFilePath(subdir);
                    QString subdirTarget = targetDir.absoluteFilePath(subdir);
                    
                    qDebug() << "Merging usr/games subdirectory:" << subdir;
                    if (!copyMissingDirectoryContents(subdirSource, subdirTarget)) {
                        qWarning() << "Failed to merge usr/games subdirectory:" << subdir;
                    }
                }
            } else {
                // For other directories, copy normally
                if (!SubprocessWrapper::copyDirectory(sourcePath, targetPath)) {
                    qWarning() << "Failed to copy resources from:" << sourcePath << "to" << targetPath;
                    // Don't fail completely, just warn
                }
            }
        }
    }
    
    return true;
}

bool AppDirBuilder::copyMissingDirectoryContents(const QString& sourcePath, const QString& targetPath) {
    QDir sourceDir(sourcePath);
    if (!sourceDir.exists()) {
        return false;
    }

    QDir targetDir(targetPath);
    if (!targetDir.exists() && !targetDir.mkpath(".")) {
        return false;
    }

    const QFileInfoList entries = sourceDir.entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot
    );

    for (const QFileInfo& entry : entries) {
        const QString sourceEntryPath = entry.absoluteFilePath();
        const QString targetEntryPath = targetDir.absoluteFilePath(entry.fileName());

        if (entry.isDir() && !entry.isSymLink()) {
            if (!copyMissingDirectoryContents(sourceEntryPath, targetEntryPath)) {
                return false;
            }
            continue;
        }

        if (QFileInfo::exists(targetEntryPath) || QFileInfo(targetEntryPath).isSymLink()) {
            continue;
        }

        if (!SubprocessWrapper::copyFile(sourceEntryPath, targetEntryPath)) {
            qWarning() << "Failed to copy missing resource:" << sourceEntryPath << "to" << targetEntryPath;
            return false;
        }
    }

    return true;
}

bool AppDirBuilder::createDesktopFile(const QString& appDirPath, const PackageMetadata& metadata) {
    QString desktopDir = QString("%1/usr/share/applications").arg(appDirPath);
    QDir dir(desktopDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    
    QString desktopPath = QString("%1/%2.desktop")
        .arg(desktopDir)
        .arg(metadata.package);
    
    qDebug() << "Creating .desktop file at:" << desktopPath;
    
    QFile file(desktopPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Failed to open .desktop file for writing:" << desktopPath;
        return false;
    }
    
    QTextStream out(&file);
    out << "[Desktop Entry]\n";
    out << "Type=Application\n";
    out << "Name=" << metadata.package << "\n";
    
    if (!metadata.description.isEmpty()) {
        // Take first line of description
        QString desc = metadata.description.split('\n').first();
        // Escape special characters
        desc.replace("&", "&amp;");
        out << "Comment=" << desc << "\n";
    }
    
    out << "Exec=AppRun\n";
    
    out << "Icon=" << metadata.package << "\n";
    
    // Categories is REQUIRED by appimagetool
    out << "Categories=Utility;\n";
    
    out << "Terminal=false\n";
    
    file.close();
    
    // Verify file was created
    if (!QFileInfo::exists(desktopPath)) {
        qWarning() << ".desktop file was not created:" << desktopPath;
        return false;
    }
    
    qDebug() << ".desktop file created successfully:" << desktopPath;
    return true;
}

bool AppDirBuilder::fixDesktopFile(const QString& desktopPath, const PackageMetadata& metadata) {
    QFile file(desktopPath);
    if (!file.open(QIODevice::ReadWrite | QIODevice::Text)) {
        qWarning() << "Failed to open .desktop file for fixing:" << desktopPath;
        return false;
    }
    
    QTextStream in(&file);
    QString content = in.readAll();
    file.close();
    
    bool hasCategories = content.contains("Categories=", Qt::CaseInsensitive);
    bool modified = false;
    const QString normalizedExec = "Exec=AppRun";
    const QString normalizedName = metadata.package.isEmpty() ? QFileInfo(desktopPath).baseName() : metadata.package;

    QRegularExpression execRegex("(?im)^Exec=.*$");
    if (content.contains(execRegex)) {
        content.replace(execRegex, normalizedExec);
        modified = true;
    } else if (content.contains("[Desktop Entry]", Qt::CaseInsensitive)) {
        content.replace(QRegularExpression("(?i)(\\[Desktop Entry\\])"), QString("\\1\n%1").arg(normalizedExec));
        modified = true;
    }

    if (!normalizedName.isEmpty()) {
        QRegularExpression nameRegex("(?im)^Name=.*$");
        if (content.contains(nameRegex)) {
            content.replace(nameRegex, QString("Name=%1").arg(normalizedName));
        } else if (content.contains("[Desktop Entry]", Qt::CaseInsensitive)) {
            content.replace(QRegularExpression("(?i)(\\[Desktop Entry\\])"), QString("\\1\nName=%1").arg(normalizedName));
        }
        modified = true;
    }
    
    if (!hasCategories) {
        qDebug() << "Adding missing Categories= to .desktop file";
        // Find the last line before [Desktop Entry] section ends or before another section
        // Add Categories= before Terminal= if present, or at the end
        if (content.contains("Terminal=", Qt::CaseInsensitive)) {
            content.replace(QRegularExpression("(?i)(Terminal=.*)"), "Categories=Utility;\n\\1");
        } else {
            // Add at the end of [Desktop Entry] section
            if (content.contains("[Desktop Entry]", Qt::CaseInsensitive)) {
                // Find end of Desktop Entry section or end of file
                int entryEnd = content.indexOf("\n[", content.indexOf("[Desktop Entry]"));
                if (entryEnd == -1) {
                    entryEnd = content.length();
                }
                content.insert(entryEnd, "Categories=Utility;\n");
            } else {
                // No Desktop Entry section, add at end
                content += "\nCategories=Utility;\n";
            }
        }
        modified = true;
    } else {
        QRegularExpression categoriesRegex("(?im)^Categories\\s*=\\s*(.*)$");
        QRegularExpressionMatch match = categoriesRegex.match(content);
        if (match.hasMatch()) {
            const QString sanitizedCategories = sanitizeDesktopCategories(match.captured(1).trimmed());
            if (match.captured(0).trimmed() != sanitizedCategories) {
                qDebug() << "Sanitized Categories= in .desktop file to:" << sanitizedCategories;
                content.replace(categoriesRegex, sanitizedCategories);
                modified = true;
            }
        }
    }
    
    // Also ensure Type=Application is present
    if (!content.contains("Type=Application", Qt::CaseInsensitive) && 
        content.contains("[Desktop Entry]", Qt::CaseInsensitive)) {
        qDebug() << "Adding missing Type=Application to .desktop file";
        content.replace(QRegularExpression("(?i)(\\[Desktop Entry\\])"), "\\1\nType=Application");
        modified = true;
    }

    QRegularExpression noDisplayRegex("(?im)^NoDisplay\\s*=\\s*true\\s*$");
    if (content.contains(noDisplayRegex)) {
        content.replace(noDisplayRegex, "NoDisplay=false");
        modified = true;
    }
    
    if (modified) {
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            qWarning() << "Failed to write fixed .desktop file:" << desktopPath;
            return false;
        }
        QTextStream out(&file);
        out << content;
        file.close();
        qDebug() << "Fixed .desktop file:" << desktopPath;
    }
    
    return true;
}

bool AppDirBuilder::copyIcon(const QString& appDirPath, const QString& iconPath, const PackageMetadata& metadata) {
    QFileInfo iconInfo(iconPath);
    QString iconExt = iconInfo.suffix();
    QString iconName = QString("%1.%2").arg(metadata.package).arg(iconExt);
    
    // Copy to all icon sizes
    QStringList iconSizes = {"256x256", "128x128", "64x64", "48x48", "32x32", "16x16"};
    
    for (const QString& size : iconSizes) {
        QString targetPath = QString("%1/usr/share/icons/hicolor/%2/apps/%3")
            .arg(appDirPath)
            .arg(size)
            .arg(iconName);
        
        SubprocessWrapper::copyFile(iconPath, targetPath);
    }
    
    return true;
}

bool AppDirBuilder::createAppRun(const QString& appDirPath, const PackageMetadata& metadata) {
    QString appRunPath = QString("%1/AppRun").arg(appDirPath);
    
    // Safety check: ensure appDirPath exists
    QDir appDir(appDirPath);
    if (!appDir.exists()) {
        qWarning() << "AppDir does not exist:" << appDirPath;
        return false;
    }
    
    QFile file(appRunPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Failed to open AppRun for writing:" << appRunPath;
        return false;
    }
    
    QTextStream out(&file);
    // Qt6 uses UTF-8 by default for QTextStream
    out << "#!/bin/bash\n";
    out << "HERE=\"$(dirname \"$(readlink -f \"${0}\")\")\"\n";
    out << "\n";
    
    // UNIVERSAL: Detect application type
    // Wrap in try-catch to prevent crashes during detection
    AppInfo appInfo;
    try {
        QString execPath = metadata.mainExecutable.isEmpty() ? 
                          (metadata.executables.isEmpty() ? "" : metadata.executables.first()) : 
                          metadata.mainExecutable;
        appInfo = AppDetector::detectApp(appDirPath, "", execPath, metadata);
    } catch (...) {
        qWarning() << "Exception during app detection, using defaults";
        // Use default AppInfo (will be Native type)
        appInfo.type = AppType::Native;
        appInfo.baseDir = "";
        appInfo.workingDir = "${HERE}/usr/bin";
    }
    
    const CompatibilityFixes compatibilityFixes = CompatibilityRuleEngine::resolve(appInfo, metadata);

    // UNIVERSAL: Set up environment paths dynamically
    QStringList pathDirs = {"usr/bin", "usr/sbin", "usr/games"};
    QString arch = detectSystemArchitecture();
    QStringList libDirs = {"usr/lib"};
    
    // Add architecture-specific lib directory
    if (arch == "aarch64") {
        libDirs << "usr/lib/aarch64-linux-gnu";
    } else if (arch == "x86_64") {
        libDirs << "usr/lib/x86_64-linux-gnu";
    }
    
    // Add opt directories if they exist
    QDir optDir(QString("%1/opt").arg(appDirPath));
    if (optDir.exists()) {
        pathDirs << "opt";
        libDirs << "opt";
        // Add specific opt subdirectories
        QStringList optSubdirs = optDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& subdir : optSubdirs) {
            pathDirs << QString("opt/%1").arg(subdir);
            libDirs << QString("opt/%1").arg(subdir);
        }
    }
    
    // Add Electron base directory if found
    if (!appInfo.baseDir.isEmpty()) {
        libDirs << appInfo.baseDir;
        // For Electron apps, also add electron subdirectory if it exists
        QString electronSubdir = QString("%1/electron").arg(appInfo.baseDir);
        QDir electronDir(QString("%1/%2").arg(appDirPath).arg(electronSubdir));
        if (electronDir.exists()) {
            libDirs << electronSubdir;
            pathDirs << electronSubdir;  // Also add to PATH for electron binary
        }
    }
    
    out << "# Set up environment\n";
    QString pathStr = "${HERE}/" + pathDirs.join(":${HERE}/") + ":${PATH}";
    out << "export PATH=\"" << pathStr << "\"\n";
    out << "\n";
    
    out << "# Set library paths (order matters - bundled libs first)\n";
    QString libPathStr = "${HERE}/" + libDirs.join(":${HERE}/") + ":${LD_LIBRARY_PATH}";
    out << "export LD_LIBRARY_PATH=\"" << libPathStr << "\"\n";
    out << "\n";
    
    out << "# Set XDG directories\n";
    out << "export XDG_DATA_DIRS=\"${HERE}/usr/share:${XDG_DATA_DIRS}\"\n";
    out << "export XDG_CONFIG_DIRS=\"${HERE}/etc/xdg:${XDG_CONFIG_DIRS}\"\n";
    out << "\n";
    
    // Set application-specific environment variables
    for (const QString& envVar : appInfo.envVars) {
        out << "export " << envVar << "\n";
    }

    for (const QString& exportStatement : compatibilityFixes.exportStatements) {
        out << exportStatement << "\n";
    }

    for (const QString& unsetVariable : compatibilityFixes.unsetVariables) {
        out << "unset " << unsetVariable << "\n";
    }

    for (const QString& preLaunchCommand : compatibilityFixes.preLaunchCommands) {
        out << preLaunchCommand << "\n";
    }

    if (!compatibilityFixes.isEmpty()) {
        out << "\n";
    }
    
    if (!metadata.mainExecutable.isEmpty()) {
        QFileInfo execInfo(metadata.mainExecutable);
        QString execName = execInfo.fileName();
        QString execPath = metadata.mainExecutable;
        
        // UNIVERSAL: Use detected app type
        if (appInfo.type == AppType::Java) {
            // Find Java in the bundled JRE or use system Java
            // Try common JRE locations, but test if they work first
            out << "JAVA=\"\"\n";
            out << "JRE_LIB_DIR=\"\"\n";
            out << "for jre_path in \"${HERE}/usr/games\"/*/lib/jvm/jre/bin/java \"${HERE}/usr/games\"/*/*/lib/jvm/jre/bin/java \"${HERE}/usr/games\"/*/*/*/lib/jvm/jre/bin/java \"${HERE}/usr/lib\"/*/jre/bin/java \"${HERE}/usr/lib\"/*/*/jre/bin/java \"${HERE}/opt\"/*/jre/bin/java \"${HERE}/opt\"/*/*/jre/bin/java; do\n";
            out << "    if [ -f \"$jre_path\" ]; then\n";
            out << "        # Test if this Java works (check version)\n";
            out << "        if \"$jre_path\" -version >/dev/null 2>&1; then\n";
            out << "            JAVA=\"$jre_path\"\n";
            out << "            JRE_LIB_DIR=\"$(dirname \"$(dirname \"$(dirname \"$jre_path\")\")\")/lib\"\n";
            out << "            break\n";
            out << "        fi\n";
            out << "    fi\n";
            out << "done\n";
            out << "if [ -z \"$JAVA\" ]; then\n";
            out << "    # Use system Java as fallback\n";
            out << "    JAVA=\"java\"\n";
            out << "fi\n";
            out << "# Add JRE library paths to LD_LIBRARY_PATH only if using bundled JRE\n";
            out << "if [ -n \"$JRE_LIB_DIR\" ] && [ \"$JAVA\" != \"java\" ]; then\n";
            out << "    export LD_LIBRARY_PATH=\"${JRE_LIB_DIR}:${JRE_LIB_DIR}/server:${LD_LIBRARY_PATH}\"\n";
            out << "fi\n";
            // Determine jar location
            QString jarRelativePath = execPath;
            if (jarRelativePath.contains("/data/")) {
                jarRelativePath = jarRelativePath.section("/data/", 1);
            }
            // Ensure path is relative (remove leading /)
            if (jarRelativePath.startsWith("/")) {
                if (jarRelativePath.contains("/usr/games/")) {
                    jarRelativePath = jarRelativePath.section("/usr/games/", 1);
                    jarRelativePath = "usr/games/" + jarRelativePath;
                } else if (jarRelativePath.contains("/usr/lib/")) {
                    jarRelativePath = jarRelativePath.section("/usr/lib/", 1);
                    jarRelativePath = "usr/lib/" + jarRelativePath;
                } else if (jarRelativePath.contains("/opt/")) {
                    jarRelativePath = jarRelativePath.section("/opt/", 1);
                    jarRelativePath = "opt/" + jarRelativePath;
                }
            }
            // Set working directory to jar file directory for Java apps
            QString jarWorkingDir = jarRelativePath;
            QFileInfo jarInfo(jarWorkingDir);
            jarWorkingDir = jarInfo.path();
            if (jarWorkingDir == ".") {
                jarWorkingDir = jarRelativePath.section("/", 0, -2); // Remove filename, keep directory
            }
            if (!jarWorkingDir.isEmpty() && jarWorkingDir != ".") {
                out << "cd \"${HERE}/" << jarWorkingDir << "\"\n";
            }
            out << "exec \"$JAVA\" -jar \"${HERE}/" << jarRelativePath << "\" \"$@\"\n";
        } else {
            // UNIVERSAL: Determine relative path from executable path
            QString relativePath = execPath;
            // Remove /data/ prefix if present (DEB packages)
            if (relativePath.contains("/data/")) {
                relativePath = relativePath.section("/data/", 1);
            }
            // Remove absolute path prefix, keep only relative path from root
            if (relativePath.startsWith("/")) {
                // Extract path after /usr, /opt, etc.
                // Check usr/games FIRST before general /usr/ check
                if (relativePath.contains("/usr/games/")) {
                    relativePath = relativePath.section("/usr/games/", 1);
                    relativePath = "usr/games/" + relativePath;
                    qDebug() << "Determined relative path for game:" << relativePath;
                } else if (relativePath.contains("/usr/bin/")) {
                    relativePath = relativePath.section("/usr/bin/", 1);
                    relativePath = "usr/bin/" + relativePath;
                } else if (relativePath.contains("/usr/sbin/")) {
                    relativePath = relativePath.section("/usr/sbin/", 1);
                    relativePath = "usr/sbin/" + relativePath;
                } else if (relativePath.contains("/usr/")) {
                    relativePath = relativePath.section("/usr/", 1);
                    relativePath = "usr/" + relativePath;
                } else if (relativePath.contains("/opt/")) {
                    relativePath = relativePath.section("/opt/", 1);
                    relativePath = "opt/" + relativePath;
                } else {
                    relativePath = QString("usr/bin/%1").arg(execName);
                }
            } else {
                // Already relative - ensure it starts with usr/ or opt/
                if (!relativePath.startsWith("usr/") && !relativePath.startsWith("opt/")) {
                    // Try to determine from execPath
                    if (execPath.contains("/usr/games/")) {
                        relativePath = "usr/games/" + execName;
                    } else {
                        relativePath = "usr/bin/" + execName;
                    }
                }
            }
            
            // UNIVERSAL: Handle different application types
            if (appInfo.type == AppType::Chrome) {
                out << "# Chrome wrapper - set up sandbox and run from correct directory\n";
                out << "cd \"${HERE}/opt/google/chrome\"\n";
                out << "exec \"${HERE}/" << relativePath << "\" \"$@\"\n";
            } else if (appInfo.type == AppType::Electron) {
                // Electron application
                out << "# Electron application\n";
                
                // For VS Code/Codium style apps, check if there's a bin/ subdirectory with the launcher
                QFileInfo mainExecInfo(metadata.mainExecutable);
                QString execName = mainExecInfo.fileName();
                QString binLauncherPath;
                
                // Check for bin/ subdirectory launcher (VS Code/Codium style)
                if (appInfo.baseDir.startsWith("usr/share/")) {
                    QString binPath = QString("%1/%2/bin/%3").arg(appDirPath).arg(appInfo.baseDir).arg(execName);
                    QFileInfo binInfo(binPath);
                    if (binInfo.exists() && binInfo.isExecutable()) {
                        binLauncherPath = QString("%1/bin/%2").arg(appInfo.baseDir).arg(execName);
                        qDebug() << "Found bin launcher for Electron app:" << binLauncherPath;
                    }
                }
                
                // For Electron apps, try to find the actual Electron binary
                QString electronBinary;
                QString electronBinaryPath;
                if (!appInfo.baseDir.isEmpty()) {
                    // Check if baseDir is a valid path in appDirPath
                    QString fullBaseDirPath = QString("%1/%2").arg(appDirPath).arg(appInfo.baseDir);
                    QDir baseDirCheck(fullBaseDirPath);
                    if (baseDirCheck.exists()) {
                        // Only call findElectronBinary if baseDir is valid and exists
                        // Pass FULL path to findElectronBinary
                        try {
                            electronBinary = AppDetector::findElectronBinary(fullBaseDirPath);
                            if (!electronBinary.isEmpty()) {
                                electronBinaryPath = QString("%1/%2").arg(appInfo.baseDir).arg(electronBinary);
                                QString fullElectronPath = QString("%1/%2").arg(appDirPath).arg(electronBinaryPath);
                                QFileInfo electronInfo(fullElectronPath);
                                qDebug() << "Checking Electron binary:" << fullElectronPath 
                                         << "exists:" << electronInfo.exists() 
                                         << "executable:" << electronInfo.isExecutable();
                                if (!electronInfo.exists() || !electronInfo.isExecutable()) {
                                    electronBinary.clear();
                                    electronBinaryPath.clear();
                                }
                            }
                        } catch (...) {
                            // If findElectronBinary throws, just clear and continue
                            electronBinary.clear();
                            electronBinaryPath.clear();
                        }
                    }
                }
                
                // Determine working directory - replace ${HERE} with actual path
                QString workingDir = appInfo.workingDir;
                if (workingDir.contains("${HERE}")) {
                    // Extract path after ${HERE}/
                    QString pathAfterHere = workingDir;
                    pathAfterHere.replace("${HERE}/", "");
                    if (!pathAfterHere.isEmpty()) {
                        workingDir = QString("\"${HERE}/%1\"").arg(pathAfterHere);
                    } else {
                        workingDir = "\"${HERE}\"";
                    }
                } else if (!workingDir.isEmpty()) {
                    workingDir = QString("\"%1\"").arg(workingDir);
                }
                
                if (!electronBinary.isEmpty() && !electronBinaryPath.isEmpty()) {
                    // Found Electron binary (e.g., Discord, Slack, etc.)
                    out << "# Found Electron binary: " << electronBinaryPath << "\n";
                    
                    // Add Electron base directory to LD_LIBRARY_PATH for bundled libs
                    out << "export LD_LIBRARY_PATH=\"${HERE}/" << appInfo.baseDir << ":${LD_LIBRARY_PATH}\"\n";
                    
                    // Change to the Electron app directory (important for proper execution)
                    out << "cd \"${HERE}/" << appInfo.baseDir << "\"\n";
                    
                    // Find .asar file in base directory
                    bool usedAsar = false;
                    if (!appInfo.baseDir.isEmpty()) {
                        QString fullBaseDirPath = QString("%1/%2").arg(appDirPath).arg(appInfo.baseDir);
                        QDir baseDir(fullBaseDirPath);
                        if (baseDir.exists()) {
                            QStringList asarFiles = baseDir.entryList({"*.asar"}, QDir::Files);
                            if (!asarFiles.isEmpty()) {
                                QString asarFile = QString("%1/%2").arg(appInfo.baseDir).arg(asarFiles.first());
                                out << "# Using .asar file: " << asarFile << "\n";
                                out << "exec \"${HERE}/" << electronBinaryPath << "\" \"${HERE}/" << asarFile << "\" \"$@\"\n";
                                usedAsar = true;
                            }
                        }
                    }
                    
                    if (!usedAsar) {
                        if (!binLauncherPath.isEmpty()) {
                            // Use bin launcher for VS Code/Codium style apps
                            out << "# Using bin launcher: " << binLauncherPath << "\n";
                            out << "exec \"${HERE}/" << binLauncherPath << "\" \"$@\"\n";
                        } else {
                            // No .asar file and no bin launcher - run Electron binary directly
                            // This is common for Discord, Slack, etc.
                            out << "# Running Electron binary directly (no .asar)\n";
                            out << "exec \"${HERE}/" << electronBinaryPath << "\" \"$@\"\n";
                        }
                    }
                } else if (!binLauncherPath.isEmpty()) {
                    // Electron binary not found but bin launcher exists (VS Code/Codium)
                    out << "# Using bin launcher: " << binLauncherPath << "\n";
                    if (!workingDir.isEmpty()) {
                        out << "cd " << workingDir << "\n";
                    }
                    out << "exec \"${HERE}/" << binLauncherPath << "\" \"$@\"\n";
                } else {
                    // Electron binary not found, use wrapper script
                    out << "# Electron binary not found, using wrapper script\n";
                    out << "export HERE=\"${HERE}\"\n";
                    if (!workingDir.isEmpty()) {
                        out << "cd " << workingDir << "\n";
                    }
                    out << "exec \"${HERE}/" << relativePath << "\" \"$@\"\n";
                }
            } else if (appInfo.type == AppType::Python) {
                // Python application
                out << "# Python application\n";
                QString pythonInterpreter = AppDetector::findPythonInterpreter(appDirPath);
                
                // Build comprehensive PYTHONPATH
                QStringList pythonLibDirs;
                
                // Standard Python paths
                pythonLibDirs << "${HERE}/usr/lib/python3";
                pythonLibDirs << "${HERE}/usr/lib/python3/dist-packages";
                pythonLibDirs << "${HERE}/usr/lib/python3/site-packages";
                pythonLibDirs << "${HERE}/usr/share/python3";
                
                // Find all Python version directories in AppDir
                QDir usrLibDir(QString("%1/usr/lib").arg(appDirPath));
                if (usrLibDir.exists()) {
                    QStringList entries = usrLibDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const QString& entry : entries) {
                        if (entry.startsWith("python")) {
                            pythonLibDirs << QString("${HERE}/usr/lib/%1").arg(entry);
                            pythonLibDirs << QString("${HERE}/usr/lib/%1/dist-packages").arg(entry);
                            pythonLibDirs << QString("${HERE}/usr/lib/%1/site-packages").arg(entry);
                        }
                    }
                }
                
                // Also check usr/share for Python modules
                QDir usrShareDir(QString("%1/usr/share").arg(appDirPath));
                if (usrShareDir.exists()) {
                    QStringList entries = usrShareDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const QString& entry : entries) {
                        if (entry.startsWith("python")) {
                            pythonLibDirs << QString("${HERE}/usr/share/%1").arg(entry);
                        }
                    }
                }
                
                // Set PYTHONPATH
                out << "export PYTHONPATH=\"" << pythonLibDirs.join(":") << ":${PYTHONPATH}\"\n";
                
                // Also set PYTHONHOME if Python is bundled
                if (pythonInterpreter.startsWith("usr/") || pythonInterpreter.startsWith("opt/")) {
                    QString pythonHome = pythonInterpreter.section('/', 0, -2);
                    out << "export PYTHONHOME=\"${HERE}/" << pythonHome << "\"\n";
                }
                
                // Change to working directory (important for relative imports)
                if (!appInfo.workingDir.isEmpty()) {
                    out << "cd \"" << appInfo.workingDir << "\"\n";
                }
                
                // Determine Python script path
                QString pythonScriptPath = relativePath;
                // If the executable is a launcher script, use the actual Python file
                if (AppDetector::isPythonLauncherScript(metadata.mainExecutable)) {
                    QString actualPythonFile = AppDetector::findPythonFileInScript(metadata.mainExecutable);
                    if (!actualPythonFile.isEmpty()) {
                        // Convert to relative path from AppDir
                        if (actualPythonFile.startsWith("/")) {
                            if (actualPythonFile.contains("/usr/share/")) {
                                pythonScriptPath = actualPythonFile.section("/usr/share/", 1);
                                pythonScriptPath = "usr/share/" + pythonScriptPath;
                            } else if (actualPythonFile.contains("/usr/games/")) {
                                pythonScriptPath = actualPythonFile.section("/usr/games/", 1);
                                pythonScriptPath = "usr/games/" + pythonScriptPath;
                            } else if (actualPythonFile.contains("/usr/bin/")) {
                                pythonScriptPath = actualPythonFile.section("/usr/bin/", 1);
                                pythonScriptPath = "usr/bin/" + pythonScriptPath;
                            }
                        } else {
                            // Relative path - combine with working directory
                            if (appInfo.workingDir.contains("usr/share/")) {
                                QString baseDir = appInfo.workingDir.section("usr/share/", 1);
                                pythonScriptPath = QString("usr/share/%1/%2").arg(baseDir).arg(actualPythonFile);
                            } else if (appInfo.workingDir.contains("usr/games/")) {
                                pythonScriptPath = QString("usr/games/%1").arg(actualPythonFile);
                            } else {
                                pythonScriptPath = actualPythonFile;
                            }
                        }
                        qDebug() << "Python launcher script detected, using Python file:" << pythonScriptPath;
                    }
                }
                
                // Run Python script with unbuffered output for better error messages
                if (pythonInterpreter.startsWith("usr/") || pythonInterpreter.startsWith("opt/")) {
                    out << "exec \"${HERE}/" << pythonInterpreter << "\" -u \"${HERE}/" << pythonScriptPath << "\" \"$@\"\n";
                } else {
                    out << "exec " << pythonInterpreter << " -u \"${HERE}/" << pythonScriptPath << "\" \"$@\"\n";
                }
            } else if (appInfo.type == AppType::Script) {
                // Shell script - check if it's a wrapper that needs to launch system binary
                // For krita and similar apps, if script is just a test/debug script, try to find system binary
                QString execBaseName = QFileInfo(relativePath).baseName();
                
                // Special handling for Steam - run through bash to avoid shebang issues
                bool isSteam = (execBaseName.toLower() == "steam" || metadata.package.toLower().contains("steam"));
                
                out << "# Shell script wrapper\n";
                out << "export HERE=\"${HERE}\"\n";
                out << "export APPDIR=\"${HERE}\"\n";
                // Check if script exists and is not just a test script
                out << "if [ -f \"${HERE}/" << relativePath << "\" ]; then\n";
                out << "    # Check if script actually does something useful (not just echo/debug)\n";
                out << "    # Count non-echo, non-comment, non-empty lines\n";
                out << "    USEFUL_LINES=$(grep -vE '^echo |^#|^$|^#!/' \"${HERE}/" << relativePath << "\" 2>/dev/null | wc -l)\n";
                out << "    if [ \"$USEFUL_LINES\" -gt 0 ]; then\n";
                out << "        # Script has useful content, try to run it\n";
                if (isSteam) {
                    // For Steam, run through bash to avoid shebang issues with ${HERE}
                    out << "        exec /bin/bash \"${HERE}/" << relativePath << "\" \"$@\"\n";
                } else {
                    out << "        exec \"${HERE}/" << relativePath << "\" \"$@\"\n";
                }
                out << "    else\n";
                out << "        # Script is test/debug only, try system binary (but not from AppImage PATH)\n";
                out << "        # Save current PATH and remove AppImage paths\n";
                out << "        OLD_PATH=\"$PATH\"\n";
                out << "        export PATH=$(echo \"$PATH\" | tr ':' '\\n' | grep -v \"${HERE}\" | tr '\\n' ':' | sed 's/:$//')\n";
                out << "        if command -v " << execBaseName << " >/dev/null 2>&1; then\n";
                out << "            export PATH=\"$OLD_PATH\"\n";
                out << "            exec " << execBaseName << " \"$@\"\n";
                out << "        else\n";
                out << "            export PATH=\"$OLD_PATH\"\n";
                out << "            echo \"Error: This AppImage contains source code, not a ready application.\" >&2\n";
                out << "            echo \"The archive '" << metadata.package << "' appears to be source code.\" >&2\n";
                out << "            echo \"Please either:\" >&2\n";
                out << "            echo \"  1. Install " << execBaseName << " in your system (e.g., sudo pacman -S " << execBaseName << ")\" >&2\n";
                out << "            echo \"  2. Use a pre-built AppImage or package instead\" >&2\n";
                out << "            exit 1\n";
                out << "        fi\n";
                out << "    fi\n";
                out << "else\n";
                out << "    # Script not found, try system binary\n";
                out << "    if command -v " << execBaseName << " >/dev/null 2>&1; then\n";
                out << "        exec " << execBaseName << " \"$@\"\n";
                out << "    else\n";
                out << "        echo \"Error: " << execBaseName << " not found. Please install " << execBaseName << " or provide a proper executable.\" >&2\n";
                out << "        exit 1\n";
                out << "    fi\n";
                out << "fi\n";
            } else {
                // Native application - change to working directory if specified
                // Always change directory for games and opt applications
                if (!appInfo.workingDir.isEmpty()) {
                    QString workingDir = appInfo.workingDir;
                    if (workingDir.contains("${HERE}")) {
                        QString pathAfterHere = workingDir;
                        pathAfterHere.replace("${HERE}/", "");
                        if (!pathAfterHere.isEmpty()) {
                            workingDir = QString("\"${HERE}/%1\"").arg(pathAfterHere);
                        } else {
                            workingDir = "\"${HERE}\"";
                        }
                    } else {
                        workingDir = QString("\"%1\"").arg(workingDir);
                    }
                    out << "cd " << workingDir << "\n";
                } else if (relativePath.startsWith("opt/")) {
                    const QString nativeWorkingDir = QFileInfo(relativePath).path();
                    if (!nativeWorkingDir.isEmpty() && nativeWorkingDir != ".") {
                        out << "cd \"${HERE}/" << nativeWorkingDir << "\"\n";
                    }
                }
                // Use absolute path from HERE for reliability
                out << "exec \"${HERE}/" << relativePath << "\" \"$@\"\n";
            }
        }
    } else if (!metadata.executables.isEmpty()) {
        // UNIVERSAL: Use same logic as mainExecutable
        QString firstExec = metadata.executables.first();
        AppInfo fallbackAppInfo = AppDetector::detectApp(appDirPath, "", firstExec, metadata);
        
        QFileInfo execInfo(firstExec);
        QString execName = execInfo.fileName();
        QString execPath = firstExec;
        
        if (fallbackAppInfo.type == AppType::Java) {
            out << "JAVA=\"\"\n";
            out << "for jre_path in \"${HERE}/usr/games\"/*/lib/jvm/jre/bin/java \"${HERE}/usr/games\"/*/*/lib/jvm/jre/bin/java \"${HERE}/usr/games\"/*/*/*/lib/jvm/jre/bin/java \"${HERE}/usr/lib\"/*/jre/bin/java \"${HERE}/usr/lib\"/*/*/jre/bin/java \"${HERE}/opt\"/*/jre/bin/java \"${HERE}/opt\"/*/*/jre/bin/java; do\n";
            out << "    if [ -f \"$jre_path\" ]; then\n";
            out << "        JAVA=\"$jre_path\"\n";
            out << "        break\n";
            out << "    fi\n";
            out << "done\n";
            out << "if [ -z \"$JAVA\" ]; then\n";
            out << "    JAVA=\"java\"\n";
            out << "fi\n";
            QString jarRelativePath = execPath;
            if (jarRelativePath.contains("/data/")) {
                jarRelativePath = jarRelativePath.section("/data/", 1);
            }
            // Set working directory to jar file directory for Java apps
            QString jarWorkingDir = jarRelativePath;
            QFileInfo jarInfo(jarWorkingDir);
            jarWorkingDir = jarInfo.path();
            if (jarWorkingDir == ".") {
                jarWorkingDir = jarRelativePath.section("/", 0, -2); // Remove filename, keep directory
            }
            if (!jarWorkingDir.isEmpty() && jarWorkingDir != ".") {
                out << "cd \"${HERE}/" << jarWorkingDir << "\"\n";
            }
            out << "exec \"$JAVA\" -jar \"${HERE}/" << jarRelativePath << "\" \"$@\"\n";
        } else {
            // UNIVERSAL: Determine relative path
            QString relativePath = execPath;
            if (relativePath.contains("/data/")) {
                relativePath = relativePath.section("/data/", 1);
            }
            if (relativePath.startsWith("/")) {
                if (relativePath.contains("/usr/")) {
                    relativePath = relativePath.section("/usr/", 1);
                    relativePath = "usr/" + relativePath;
                } else if (relativePath.contains("/opt/")) {
                    relativePath = relativePath.section("/opt/", 1);
                    relativePath = "opt/" + relativePath;
                } else {
                    relativePath = QString("usr/bin/%1").arg(execName);
                }
            }
            
            // UNIVERSAL: Handle different application types
            if (fallbackAppInfo.type == AppType::Chrome) {
                out << "# Chrome wrapper\n";
                out << "cd \"${HERE}/opt/google/chrome\"\n";
                out << "exec \"${HERE}/" << relativePath << "\" \"$@\"\n";
            } else if (fallbackAppInfo.type == AppType::Electron) {
                out << "# Electron application\n";
                // Determine working directory - replace ${HERE} with actual path
                QString workingDir = fallbackAppInfo.workingDir;
                if (workingDir.contains("${HERE}")) {
                    QString pathAfterHere = workingDir;
                    pathAfterHere.replace("${HERE}/", "");
                    if (!pathAfterHere.isEmpty()) {
                        workingDir = QString("\"${HERE}/%1\"").arg(pathAfterHere);
                    } else {
                        workingDir = "\"${HERE}\"";
                    }
                } else if (!workingDir.isEmpty()) {
                    workingDir = QString("\"%1\"").arg(workingDir);
                }
                if (!workingDir.isEmpty()) {
                    out << "cd " << workingDir << "\n";
                }
                out << "exec \"${HERE}/" << relativePath << "\" \"$@\"\n";
            } else if (fallbackAppInfo.type == AppType::Python) {
                // Python application
                out << "# Python application\n";
                QString pythonInterpreter = AppDetector::findPythonInterpreter(appDirPath);
                
                // Build comprehensive PYTHONPATH
                QStringList pythonLibDirs;
                
                // Standard Python paths
                pythonLibDirs << "${HERE}/usr/lib/python3";
                pythonLibDirs << "${HERE}/usr/lib/python3/dist-packages";
                pythonLibDirs << "${HERE}/usr/lib/python3/site-packages";
                pythonLibDirs << "${HERE}/usr/share/python3";
                
                // Find all Python version directories in AppDir
                QDir usrLibDir(QString("%1/usr/lib").arg(appDirPath));
                if (usrLibDir.exists()) {
                    QStringList entries = usrLibDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const QString& entry : entries) {
                        if (entry.startsWith("python")) {
                            pythonLibDirs << QString("${HERE}/usr/lib/%1").arg(entry);
                            pythonLibDirs << QString("${HERE}/usr/lib/%1/dist-packages").arg(entry);
                            pythonLibDirs << QString("${HERE}/usr/lib/%1/site-packages").arg(entry);
                        }
                    }
                }
                
                // Also check usr/share for Python modules
                QDir usrShareDir(QString("%1/usr/share").arg(appDirPath));
                if (usrShareDir.exists()) {
                    QStringList entries = usrShareDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                    for (const QString& entry : entries) {
                        if (entry.startsWith("python")) {
                            pythonLibDirs << QString("${HERE}/usr/share/%1").arg(entry);
                        }
                    }
                }
                
                // Set PYTHONPATH
                out << "export PYTHONPATH=\"" << pythonLibDirs.join(":") << ":${PYTHONPATH}\"\n";
                
                // Also set PYTHONHOME if Python is bundled
                if (pythonInterpreter.startsWith("usr/") || pythonInterpreter.startsWith("opt/")) {
                    QString pythonHome = pythonInterpreter.section('/', 0, -2);
                    out << "export PYTHONHOME=\"${HERE}/" << pythonHome << "\"\n";
                }
                
                // Change to working directory (important for relative imports)
                if (!fallbackAppInfo.workingDir.isEmpty()) {
                    QString workingDir = fallbackAppInfo.workingDir;
                    if (workingDir.contains("${HERE}")) {
                        QString pathAfterHere = workingDir;
                        pathAfterHere.replace("${HERE}/", "");
                        if (!pathAfterHere.isEmpty()) {
                            workingDir = QString("\"${HERE}/%1\"").arg(pathAfterHere);
                        } else {
                            workingDir = "\"${HERE}\"";
                        }
                    } else {
                        workingDir = QString("\"%1\"").arg(workingDir);
                    }
                    out << "cd " << workingDir << "\n";
                }
                
                // Determine Python script path
                QString pythonScriptPath = relativePath;
                // If the executable is a launcher script, use the actual Python file
                if (AppDetector::isPythonLauncherScript(firstExec)) {
                    QString actualPythonFile = AppDetector::findPythonFileInScript(firstExec);
                    if (!actualPythonFile.isEmpty()) {
                        // Convert to relative path from AppDir
                        if (actualPythonFile.startsWith("/")) {
                            if (actualPythonFile.contains("/usr/share/")) {
                                pythonScriptPath = actualPythonFile.section("/usr/share/", 1);
                                pythonScriptPath = "usr/share/" + pythonScriptPath;
                            } else if (actualPythonFile.contains("/usr/games/")) {
                                pythonScriptPath = actualPythonFile.section("/usr/games/", 1);
                                pythonScriptPath = "usr/games/" + pythonScriptPath;
                            } else if (actualPythonFile.contains("/usr/bin/")) {
                                pythonScriptPath = actualPythonFile.section("/usr/bin/", 1);
                                pythonScriptPath = "usr/bin/" + pythonScriptPath;
                            } else {
                                // Keep original path but make relative
                                pythonScriptPath = actualPythonFile;
                            }
                        } else {
                            // Relative path - combine with working directory
                            if (fallbackAppInfo.workingDir.contains("usr/share/")) {
                                QString baseDir = fallbackAppInfo.workingDir.section("usr/share/", 1);
                                pythonScriptPath = QString("usr/share/%1/%2").arg(baseDir).arg(actualPythonFile);
                            } else if (fallbackAppInfo.workingDir.contains("usr/games/")) {
                                pythonScriptPath = QString("usr/games/%1").arg(actualPythonFile);
                            } else {
                                pythonScriptPath = actualPythonFile;
                            }
                        }
                        qDebug() << "Python launcher script detected (fallback), using Python file:" << pythonScriptPath;
                    }
                }
                
                // Run Python script with unbuffered output for better error messages
                if (pythonInterpreter.startsWith("usr/") || pythonInterpreter.startsWith("opt/")) {
                    out << "exec \"${HERE}/" << pythonInterpreter << "\" -u \"${HERE}/" << pythonScriptPath << "\" \"$@\"\n";
                } else {
                    out << "exec " << pythonInterpreter << " -u \"${HERE}/" << pythonScriptPath << "\" \"$@\"\n";
                }
            } else if (fallbackAppInfo.type == AppType::Script) {
                out << "# Shell script wrapper\n";
                out << "export HERE=\"${HERE}\"\n";
                out << "export APPDIR=\"${HERE}\"\n";
                out << "exec \"${HERE}/" << relativePath << "\" \"$@\"\n";
            } else {
                // Always change directory for games and opt applications
                if (!fallbackAppInfo.workingDir.isEmpty()) {
                    QString workingDir = fallbackAppInfo.workingDir;
                    if (workingDir.contains("${HERE}")) {
                        QString pathAfterHere = workingDir;
                        pathAfterHere.replace("${HERE}/", "");
                        if (!pathAfterHere.isEmpty()) {
                            workingDir = QString("\"${HERE}/%1\"").arg(pathAfterHere);
                        } else {
                            workingDir = "\"${HERE}\"";
                        }
                    } else {
                        workingDir = QString("\"%1\"").arg(workingDir);
                    }
                    out << "cd " << workingDir << "\n";
                } else if (relativePath.startsWith("opt/")) {
                    const QString nativeWorkingDir = QFileInfo(relativePath).path();
                    if (!nativeWorkingDir.isEmpty() && nativeWorkingDir != ".") {
                        out << "cd \"${HERE}/" << nativeWorkingDir << "\"\n";
                    }
                }
                out << "exec \"${HERE}/" << relativePath << "\" \"$@\"\n";
            }
        }
    }
    
    // Flush and close the file explicitly before setting executable
    out.flush();
    file.close();
    
    // Verify file was written successfully
    if (!QFileInfo::exists(appRunPath)) {
        qWarning() << "AppRun file was not created:" << appRunPath;
        return false;
    }
    
    // Set executable permissions
    bool result = SubprocessWrapper::setExecutable(appRunPath);
    if (!result) {
        qWarning() << "Failed to set executable permissions on AppRun:" << appRunPath;
    }
    
    return result;
}
