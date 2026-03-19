#ifndef APP_DETAILS_DIALOG_H
#define APP_DETAILS_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTextEdit>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QListWidget>
#include "appstream_metadata.h"

class AppDetailsDialog : public QDialog {
    Q_OBJECT

public:
    explicit AppDetailsDialog(const AppInfo& appInfo, QWidget* parent = nullptr);
    ~AppDetailsDialog();

signals:
    void downloadClicked(const AppInfo& appInfo);

private slots:
    void onDownloadButtonClicked();
    void onViewInRepositoryClicked();

private:
    void setupUI();
    void loadScreenshots();
    
    AppInfo m_appInfo;
    
    QLabel* m_iconLabel;
    QLabel* m_nameLabel;
    QLabel* m_versionLabel;
    QLabel* m_developerLabel;
    QTextEdit* m_descriptionText;
    QTextEdit* m_longDescriptionText;
    QListWidget* m_screenshotsList;
    QLabel* m_sizeLabel;
    QLabel* m_licenseLabel;
    QLabel* m_categoryLabel;
    QLabel* m_repositoryLabel;
    QPushButton* m_downloadButton;
    QPushButton* m_viewInRepositoryButton;
};

#endif // APP_DETAILS_DIALOG_H

