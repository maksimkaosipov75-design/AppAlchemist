#ifndef RPM_REPOSITORY_H
#define RPM_REPOSITORY_H

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Resolves RPM dependencies against a distribution's own repository.
//
// Converting an .rpm on a machine that runs another distribution used to
// produce bundles missing most of their libraries: the packages that provide
// them are not installed here, and the host's package manager knows nothing
// about them. The repository metadata answers exactly the question that
// matters - which package provides this soname - and the packages can be
// downloaded straight from the mirror, whatever the host runs.
class RpmRepository : public QObject {
    Q_OBJECT

public:
    explicit RpmRepository(QObject* parent = nullptr);

    // Release as it appears in an RPM release tag ("41" from "22.fc41") and
    // the architecture the package was built for.
    void setRelease(const QString& release);
    void setArchitecture(const QString& architecture);

    QString release() const { return m_release; }
    bool isUsable() const { return !m_release.isEmpty(); }

    // Reads the release number out of an RPM release string such as
    // "22.fc41"; empty when the string carries no Fedora release.
    static QString releaseFromRpmTag(const QString& rpmRelease);

    // Downloads the repository index unless a current copy is already cached.
    bool ensureMetadata();

    // Maps each wanted soname or package name to the package that provides it.
    // A single pass over the index answers all of them at once.
    QHash<QString, QString> resolve(const QSet<QString>& sonames,
                                    const QSet<QString>& packageNames);

    // Downloads the given repository locations into targetDir and returns the
    // files that arrived.
    QStringList download(const QStringList& locations, const QString& targetDir);

signals:
    void log(const QString& message);

private:
    QString baseUrl() const;
    QString cacheDirectory() const;
    bool fetch(const QString& url, const QString& destination, int timeoutMs);

    QString m_release;
    QString m_architecture = "x86_64";
    QString m_primaryPath;      // location of the index inside the repository
    QString m_primaryFile;      // cached copy on disk
};

#endif // RPM_REPOSITORY_H
