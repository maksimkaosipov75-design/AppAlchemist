#ifndef DEPENDENCYANALYZER_H
#define DEPENDENCYANALYZER_H

#include <QString>
#include <QStringList>
#include <QSet>

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
    
private:
    QSet<QString> m_systemLibraryPatterns;
    QSet<QString> m_systemPackagePatterns;
    void initializeSystemPatterns();
    QStringList runLdd(const QString& executablePath);
};

#endif // DEPENDENCYANALYZER_H














