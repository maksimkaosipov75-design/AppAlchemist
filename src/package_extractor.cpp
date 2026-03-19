#include "package_extractor.h"
#include "debparser.h"
#include "rpmparser.h"
#include "tarballparser.h"
#include "utils.h"
#include <QFileInfo>
#include <QRegularExpression>

PackageExtractor::PackageExtractor(DebParser* debParser,
                                   RpmParser* rpmParser,
                                   TarballParser* tarballParser)
    : m_debParser(debParser)
    , m_rpmParser(rpmParser)
    , m_tarballParser(tarballParser) {
}

PackageValidationResult PackageExtractor::validate(const QString& packagePath, PackageFormat packageType) const {
    PackageValidationResult result;

    if (packagePath.isEmpty() || !QFileInfo::exists(packagePath)) {
        result.error = "Package file not found";
        result.logs << "ERROR: Package file not found";
        return result;
    }

    switch (packageType) {
    case PackageFormat::Deb:
        if (!m_debParser->validateDebFile(packagePath)) {
            result.error = "Invalid .deb file";
            result.logs << "ERROR: Invalid .deb file format";
            return result;
        }
        result.logs << QString("Valid .deb file: %1").arg(packagePath);
        break;
    case PackageFormat::Rpm:
        if (!m_rpmParser->validateRpmFile(packagePath)) {
            result.error = "Invalid .rpm file";
            result.logs << "ERROR: Invalid .rpm file format";
            return result;
        }
        result.logs << QString("Valid .rpm file: %1").arg(packagePath);
        break;
    case PackageFormat::Tarball:
        if (!m_tarballParser->validateTarball(packagePath)) {
            result.error = "Invalid tarball file";
            result.logs << "ERROR: Invalid tarball file format";
            return result;
        }
        result.logs << QString("Valid tarball file: %1").arg(packagePath);
        break;
    case PackageFormat::Unknown:
        result.error = "Unknown package type";
        result.logs << "ERROR: Unknown package type";
        return result;
    }

    result.success = true;
    return result;
}

PackageExtractionResult PackageExtractor::extract(const QString& packagePath,
                                                  PackageFormat packageType,
                                                  const QString& tempDir) const {
    PackageExtractionResult result;
    result.extractedDir = QString("%1/extracted").arg(tempDir);

    switch (packageType) {
    case PackageFormat::Deb:
        if (!m_debParser->extractDeb(packagePath, result.extractedDir)) {
            result.error = "Failed to extract .deb package";
            result.logs << "ERROR: Failed to extract .deb package";
            return result;
        }
        result.logs << "Successfully extracted .deb package";
        result.metadata = m_debParser->parseMetadata(result.extractedDir);
        break;
    case PackageFormat::Tarball:
        if (!m_tarballParser->extractTarball(packagePath, result.extractedDir)) {
            result.error = "Failed to extract tarball";
            result.logs << "ERROR: Failed to extract tarball";
            return result;
        }
        result.logs << "Successfully extracted tarball";
        result.metadata = m_tarballParser->parseMetadata(result.extractedDir);
        if (result.metadata.package.isEmpty()) {
            QFileInfo info(packagePath);
            QString baseName = info.baseName();
            baseName.remove(QRegularExpression(R"(\.(tar|linux|x86_64|amd64)$)", QRegularExpression::CaseInsensitiveOption));
            result.metadata.package = baseName;
        }
        result.logs << QString("Detected package: %1, Version: %2")
                           .arg(result.metadata.package, result.metadata.version);
        break;
    case PackageFormat::Rpm:
        if (!m_rpmParser->extractRpm(packagePath, result.extractedDir)) {
            result.error = "Failed to extract .rpm package";
            result.logs << "ERROR: Failed to extract .rpm package";
            return result;
        }
        result.logs << "Successfully extracted .rpm package";
        result.metadata = extractRpmMetadata(packagePath, result.extractedDir);
        break;
    case PackageFormat::Unknown:
        result.error = "Unknown package type";
        result.logs << "ERROR: Unknown package type";
        return result;
    }

    result.logs << QString("Package: %1, Version: %2").arg(result.metadata.package, result.metadata.version);
    result.success = true;
    return result;
}

PackageMetadata PackageExtractor::extractRpmMetadata(const QString& packagePath, const QString& extractedDir) const {
    PackageMetadata metadata;

    const ProcessResult rpmInfo = SubprocessWrapper::execute("rpm", {
        "-qp", "--queryformat", "%{NAME}\n%{VERSION}\n%{SUMMARY}\n", packagePath
    });

    if (rpmInfo.success && !rpmInfo.stdoutOutput.isEmpty()) {
        const QStringList lines = rpmInfo.stdoutOutput.trimmed().split('\n');
        if (lines.size() >= 2) {
            metadata.package = lines[0].trimmed();
            metadata.version = lines[1].trimmed();
            if (lines.size() >= 3) {
                metadata.description = lines[2].trimmed();
            }
        }
    } else {
        QFileInfo packageInfo(packagePath);
        QString baseName = packageInfo.baseName();

        QString nameWithoutExt = baseName;
        if (nameWithoutExt.endsWith(".rpm", Qt::CaseInsensitive)) {
            nameWithoutExt.chop(4);
        }

        const QRegularExpression archRegex(R"((\.(x86_64|i686|i386|aarch64|arm64|noarch))$)");
        const QRegularExpressionMatch archMatch = archRegex.match(nameWithoutExt);
        QString nameWithoutArch = nameWithoutExt;
        if (archMatch.hasMatch()) {
            nameWithoutArch = nameWithoutExt.left(archMatch.capturedStart());
        }

        const QRegularExpression releaseRegex(R"(-(el\d+|fc\d+|rhel\d+|sles\d+)$)");
        const QRegularExpressionMatch releaseMatch = releaseRegex.match(nameWithoutArch);
        QString nameWithoutRelease = nameWithoutArch;
        if (releaseMatch.hasMatch()) {
            nameWithoutRelease = nameWithoutArch.left(releaseMatch.capturedStart());
        }

        int lastDash = -1;
        for (int i = nameWithoutRelease.length() - 1; i >= 0; --i) {
            if (nameWithoutRelease[i] == '-') {
                const QString afterDash = nameWithoutRelease.mid(i + 1);
                if (!afterDash.isEmpty() && afterDash[0].isDigit()) {
                    lastDash = i;
                    break;
                }
            }
        }

        if (lastDash > 0) {
            metadata.package = nameWithoutRelease.left(lastDash);
            metadata.version = nameWithoutRelease.mid(lastDash + 1);
            if (releaseMatch.hasMatch()) {
                metadata.version += "-" + releaseMatch.captured(1);
            }
        } else {
            const int firstDash = baseName.indexOf('-');
            if (firstDash > 0) {
                metadata.package = baseName.left(firstDash);
                QString rest = baseName.mid(firstDash + 1);
                rest.remove(QRegularExpression(R"(\.[^.]+\.rpm?$)"));
                metadata.version = rest;
            }
        }
    }

    const PackageMetadata extractedMetadata = m_rpmParser->parseMetadata(extractedDir);
    if (metadata.package.isEmpty()) {
        return extractedMetadata;
    }

    metadata.executables = extractedMetadata.executables;
    metadata.scripts = extractedMetadata.scripts;
    metadata.iconPath = extractedMetadata.iconPath;
    metadata.mainExecutable = extractedMetadata.mainExecutable;
    metadata.desktopExecCommand = extractedMetadata.desktopExecCommand;
    metadata.desktopFileContent = extractedMetadata.desktopFileContent;
    metadata.javaMainClass = extractedMetadata.javaMainClass;
    metadata.javaJarPath = extractedMetadata.javaJarPath;
    if (metadata.description.isEmpty()) {
        metadata.description = extractedMetadata.description;
    }
    if (metadata.depends.isEmpty()) {
        metadata.depends = extractedMetadata.depends;
    }

    return metadata;
}
