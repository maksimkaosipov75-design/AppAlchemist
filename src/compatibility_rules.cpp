#include "compatibility_rules.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

QString rulesPath() {
#ifdef APPALCHEMIST_COMPATIBILITY_RULES_PATH
    return QStringLiteral(APPALCHEMIST_COMPATIBILITY_RULES_PATH);
#else
    return QString();
#endif
}

QString appTypeToRuleName(AppType type) {
    switch (type) {
    case AppType::Electron:
        return "electron";
    case AppType::Java:
        return "java";
    case AppType::Python:
        return "python";
    case AppType::Script:
        return "script";
    case AppType::Native:
        return "native";
    case AppType::Chrome:
        return "chrome";
    case AppType::Unknown:
        return "unknown";
    }

    return "unknown";
}

QString expandTemplate(QString value, const AppInfo& appInfo, const PackageMetadata& metadata) {
    value.replace("{baseDir}", appInfo.baseDir);
    value.replace("{workingDir}", appInfo.workingDir);
    value.replace("{package}", metadata.package);
    return value;
}

bool matchesRule(const QJsonObject& rule, const AppInfo& appInfo, const PackageMetadata& metadata) {
    const QString profile = rule.value("application_profile").toString();
    if (!profile.isEmpty() && profile != appTypeToRuleName(appInfo.type)) {
        return false;
    }

    const QString packageRegex = rule.value("package_regex").toString();
    if (!packageRegex.isEmpty()) {
        const QRegularExpression regex(packageRegex, QRegularExpression::CaseInsensitiveOption);
        if (!regex.isValid() || !regex.match(metadata.package).hasMatch()) {
            return false;
        }
    }

    const QString baseDirRegex = rule.value("base_dir_regex").toString();
    if (!baseDirRegex.isEmpty()) {
        const QRegularExpression regex(baseDirRegex, QRegularExpression::CaseInsensitiveOption);
        if (!regex.isValid() || !regex.match(appInfo.baseDir).hasMatch()) {
            return false;
        }
    }

    return true;
}

void appendUnique(QStringList& list, const QString& value) {
    if (!value.isEmpty() && !list.contains(value)) {
        list << value;
    }
}

}

bool CompatibilityFixes::isEmpty() const {
    return exportStatements.isEmpty() && unsetVariables.isEmpty() && preLaunchCommands.isEmpty();
}

CompatibilityFixes CompatibilityRuleEngine::resolve(const AppInfo& appInfo, const PackageMetadata& metadata) {
    CompatibilityFixes fixes;

    const QString path = rulesPath();
    if (path.isEmpty()) {
        return fixes;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return fixes;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (doc.isNull() || !doc.isObject()) {
        return fixes;
    }

    const QJsonArray rules = doc.object().value("rules").toArray();
    for (const QJsonValue& ruleValue : rules) {
        if (!ruleValue.isObject()) {
            continue;
        }

        const QJsonObject rule = ruleValue.toObject();
        if (!matchesRule(rule, appInfo, metadata)) {
            continue;
        }

        const QJsonObject exports = rule.value("exports").toObject();
        for (auto it = exports.begin(); it != exports.end(); ++it) {
            const QString key = it.key().trimmed();
            const QString value = expandTemplate(it.value().toString(), appInfo, metadata);
            if (!key.isEmpty() && !value.isEmpty()) {
                appendUnique(fixes.exportStatements, QString("export %1=\"%2\"").arg(key, value));
            }
        }

        const QJsonArray unsetValues = rule.value("unset").toArray();
        for (const QJsonValue& unsetValue : unsetValues) {
            appendUnique(fixes.unsetVariables, unsetValue.toString().trimmed());
        }

        const QJsonArray preLaunch = rule.value("prelaunch").toArray();
        for (const QJsonValue& commandValue : preLaunch) {
            appendUnique(fixes.preLaunchCommands,
                         expandTemplate(commandValue.toString(), appInfo, metadata));
        }
    }

    return fixes;
}
