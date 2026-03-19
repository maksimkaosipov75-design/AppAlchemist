#ifndef PACKAGE_EXTRACTOR_H
#define PACKAGE_EXTRACTOR_H

#include "debparser.h"
#include "package_profile.h"

class DebParser;
class RpmParser;
class TarballParser;

struct PackageValidationResult {
    bool success = false;
    QString error;
    QStringList logs;
};

struct PackageExtractionResult {
    bool success = false;
    QString error;
    QString extractedDir;
    PackageMetadata metadata;
    QStringList logs;
};

class PackageExtractor {
public:
    PackageExtractor(DebParser* debParser,
                     RpmParser* rpmParser,
                     TarballParser* tarballParser);

    PackageValidationResult validate(const QString& packagePath, PackageFormat packageType) const;
    PackageExtractionResult extract(const QString& packagePath,
                                    PackageFormat packageType,
                                    const QString& tempDir) const;

private:
    PackageMetadata extractRpmMetadata(const QString& packagePath, const QString& extractedDir) const;

    DebParser* m_debParser;
    RpmParser* m_rpmParser;
    TarballParser* m_tarballParser;
};

#endif // PACKAGE_EXTRACTOR_H
