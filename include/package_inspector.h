#ifndef PACKAGE_INSPECTOR_H
#define PACKAGE_INSPECTOR_H

#include "debparser.h"
#include "package_profile.h"

struct PackageInspectionResult {
    bool success = false;
    QString error;
    PackageProfile profile;
    ConversionPlan plan;
    QStringList logs;
};

class PackageInspector {
public:
    PackageInspectionResult inspect(const QString& packagePath,
                                    PackageFormat packageType,
                                    const QString& extractedDir,
                                    const PackageMetadata& metadata) const;

private:
    void appendMissingExecutableDiagnostics(PackageInspectionResult& result,
                                            const QString& packagePath,
                                            PackageFormat packageType,
                                            const PackageMetadata& metadata,
                                            const QString& extractedDir) const;
};

#endif // PACKAGE_INSPECTOR_H
