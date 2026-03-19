#ifndef COMPATIBILITY_RULES_H
#define COMPATIBILITY_RULES_H

#include "appdetector.h"
#include "debparser.h"
#include <QString>
#include <QStringList>

struct CompatibilityFixes {
    QStringList exportStatements;
    QStringList unsetVariables;
    QStringList preLaunchCommands;

    bool isEmpty() const;
};

class CompatibilityRuleEngine {
public:
    static CompatibilityFixes resolve(const AppInfo& appInfo, const PackageMetadata& metadata);
};

#endif // COMPATIBILITY_RULES_H
