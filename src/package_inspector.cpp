#include "package_inspector.h"
#include "utils.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

PackageInspectionResult PackageInspector::inspect(const QString& packagePath,
                                                  PackageFormat packageType,
                                                  const QString& extractedDir,
                                                  const PackageMetadata& metadata) const {
    PackageInspectionResult result;
    result.profile = PackageClassifier::classify(packagePath, packageType, extractedDir, metadata);
    result.plan = PackageClassifier::buildPlan(result.profile);

    result.logs << QString("Package classification: %1").arg(result.profile.summary());
    result.logs << QString("Selected conversion plan: %1").arg(result.plan.summary());

    if (result.profile.likelyUnsupported) {
        result.logs << "WARNING: Package classification indicates a partial-support or service-style package.";
    }

    if (!metadata.scripts.isEmpty()) {
        result.logs << "WARNING: Package contains installation scripts (postinst/prerm). These will NOT be executed.";
        for (const QString& script : metadata.scripts) {
            result.logs << QString("  - %1").arg(QFileInfo(script).fileName());
        }
    }

    if (metadata.executables.isEmpty()) {
        result.error = "No executables found in package";
        result.logs << "ERROR: No executables found in package";
        appendMissingExecutableDiagnostics(result, packagePath, packageType, metadata, extractedDir);
        return result;
    }

    result.logs << QString("Found %1 executable(s)").arg(metadata.executables.size());
    for (const QString& exec : metadata.executables) {
        result.logs << QString("  - %1").arg(exec);
    }

    result.success = true;
    return result;
}

void PackageInspector::appendMissingExecutableDiagnostics(PackageInspectionResult& result,
                                                          const QString& packagePath,
                                                          PackageFormat packageType,
                                                          const PackageMetadata& metadata,
                                                          const QString& extractedDir) const {
    result.logs << "Searched in: /usr/bin, /usr/sbin, /opt, /bin, /sbin, /usr/games, /usr/lib, /usr/libexec, /usr/share";

    if (packageType == PackageFormat::Rpm) {
        result.logs << "Attempting to list files in RPM package...";
        const ProcessResult rpmList = SubprocessWrapper::execute("rpm", {"-qpl", packagePath});
        if (rpmList.success && !rpmList.stdoutOutput.isEmpty()) {
            const QStringList files = rpmList.stdoutOutput.split('\n', Qt::SkipEmptyParts);
            result.logs << QString("RPM contains %1 files. Looking for executables...").arg(files.size());
            int execCount = 0;
            for (const QString& file : files) {
                if (file.contains("/bin/") || file.contains("/sbin/") ||
                    file.contains("/opt/") || file.contains("/usr/libexec/") ||
                    (file.startsWith("/usr/") && (file.endsWith(".sh") || file.endsWith(".jar")))) {
                    result.logs << QString("  Found potential executable: %1").arg(file);
                    ++execCount;
                    if (execCount > 10) {
                        result.logs << "  ... (showing first 10)";
                        break;
                    }
                }
            }
        } else {
            result.logs << "rpm command not available, listing extracted files...";
            const QDir extractedDirObj(extractedDir);
            if (extractedDirObj.exists()) {
                const QStringList entries = extractedDirObj.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                result.logs << QString("Extracted directory structure: %1").arg(entries.join(", "));

                const QDir dataDir(QString("%1/data").arg(extractedDir));
                if (dataDir.exists()) {
                    const QStringList dataEntries = dataDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
                    result.logs << QString("Data directory structure: %1")
                                       .arg(dataEntries.isEmpty() ? "(empty)" : dataEntries.join(", "));

                    const QStringList dataFiles = dataDir.entryList(QDir::Files, QDir::Name);
                    if (!dataFiles.isEmpty()) {
                        result.logs << QString("Files directly in data/: %1").arg(dataFiles.mid(0, 10).join(", "));
                    }

                    const QStringList checkDirs = {"usr/bin", "usr/sbin", "opt", "bin", "sbin", "usr/lib", "usr/libexec"};
                    for (const QString& checkDir : checkDirs) {
                        const QDir dir(dataDir.absoluteFilePath(checkDir));
                        if (dir.exists()) {
                            const QStringList files = dir.entryList(QDir::Files, QDir::Name);
                            if (!files.isEmpty()) {
                                result.logs << QString("  Files in %1: %2").arg(checkDir, files.mid(0, 10).join(", "));
                            }
                        }
                    }

                    result.logs << "Recursively searching for executables...";
                    QDirIterator it(dataDir.absolutePath(), QDirIterator::Subdirectories);
                    int execCount = 0;
                    while (it.hasNext() && execCount < 20) {
                        const QString filePath = it.next();
                        const QFileInfo fileInfo(filePath);
                        if (fileInfo.isFile() && fileInfo.isExecutable()) {
                            result.logs << QString("  Found executable: %1")
                                               .arg(filePath.mid(dataDir.absolutePath().length() + 1));
                            ++execCount;
                        } else if (fileInfo.isFile() &&
                                   (fileInfo.suffix() == "jar" || fileInfo.suffix() == "sh")) {
                            result.logs << QString("  Found potential executable: %1")
                                               .arg(filePath.mid(dataDir.absolutePath().length() + 1));
                            ++execCount;
                        }
                    }
                }
            }
        }
    }

    const QString packageName = metadata.package.toLower();
    if (packageName.contains("-data") || packageName.contains("-common") ||
        packageName.contains("-dev") || packageName.contains("-doc")) {
        result.logs << "";
        result.logs << "TIP: This appears to be a data/common package, not the main application package.";
        result.logs << "     Data packages contain resources (icons, translations, data files) but no executables.";
        result.logs << "     You need to convert the MAIN package instead:";
        result.logs << "     - For GIMP: use 'gimp' or 'gimp-bin' package, not 'gimp-data'";
        result.logs << "     - For other apps: look for packages without '-data', '-common', '-dev', '-doc' suffixes";
    } else {
        result.logs << "Tip: The package might contain only libraries or data files, not executables";
    }
}
