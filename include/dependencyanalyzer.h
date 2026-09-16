#ifndef DEPENDENCYANALYZER_H
#define DEPENDENCYANALYZER_H

#include <QString>
#include <QStringList>
#include <QSet>
#include <QProcessEnvironment>

struct LibraryInfo {
    QString path;
    QString name;
    bool isSystemLibrary;
};

class DependencyAnalyzer {
public:
    DependencyAnalyzer();
    
    QList<LibraryInfo> analyzeExecutable(const QString& executablePath);
    QStringList collectLibraries(const QStringList& executables);
    QStringList filterSystemLibraries(const QStringList& libraries);
    bool isSystemLibrary(const QString& libraryPath);
    QStringList checkSystemDependencies(const QStringList& packageDepends);
    
    // Safely runs ldd under bwrap sandbox or passively parses via readelf -d DT_NEEDED
    static QStringList runLdd(const QString& executablePath, const QProcessEnvironment& env = QProcessEnvironment());

private:
    QSet<QString> m_systemLibraryPatterns;
    QSet<QString> m_systemPackagePatterns;
    void initializeSystemPatterns();
};

#endif // DEPENDENCYANALYZER_H














