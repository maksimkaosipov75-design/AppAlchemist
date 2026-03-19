#ifndef SEARCH_DIALOG_H
#define SEARCH_DIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QProgressBar>
#include <QLabel>
#include <QVBoxLayout>
#include "repository_browser.h"

class SearchDialog : public QDialog {
    Q_OBJECT

public:
    explicit SearchDialog(QWidget* parent = nullptr);
    ~SearchDialog();
    
    // Get the selected package path after download
    QString selectedPackagePath() const { return m_downloadedPackagePath; }

signals:
    void packageSelected(const QString& packagePath);

private slots:
    void onSearchClicked();
    void onDownloadClicked();
    void onSearchCompleted(const QList<PackageInfo>& results);
    void onSearchError(const QString& errorMessage);
    void onDownloadCompleted(const QString& packagePath);
    void onDownloadError(const QString& errorMessage);
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onSelectionChanged();
    void onCellDoubleClicked(int row, int column);

private:
    void setupUI();
    void updateButtonStates();
    QString requestSudoPassword();
    
    QLineEdit* m_searchInput;
    QPushButton* m_searchButton;
    QPushButton* m_downloadButton;
    QPushButton* m_cancelButton;
    QTableWidget* m_resultsTable;
    QProgressBar* m_progressBar;
    QLabel* m_statusLabel;
    
    RepositoryBrowser* m_browser;
    QList<PackageInfo> m_searchResults;
    QString m_downloadedPackagePath;
    bool m_isSearching;
    bool m_isDownloading;
};

#endif // SEARCH_DIALOG_H

