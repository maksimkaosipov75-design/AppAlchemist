#include "search_dialog.h"
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QInputDialog>
#include <QStandardPaths>
#include <QDir>

SearchDialog::SearchDialog(QWidget* parent)
    : QDialog(parent)
    , m_browser(new RepositoryBrowser(this))
    , m_isSearching(false)
    , m_isDownloading(false)
{
    setWindowTitle("Search Packages");
    setMinimumSize(700, 500);
    resize(800, 600);
    
    setupUI();
    
    // Connect signals
    connect(m_browser, &RepositoryBrowser::searchCompleted, this, &SearchDialog::onSearchCompleted);
    connect(m_browser, &RepositoryBrowser::searchError, this, &SearchDialog::onSearchError);
    connect(m_browser, &RepositoryBrowser::downloadCompleted, this, &SearchDialog::onDownloadCompleted);
    connect(m_browser, &RepositoryBrowser::downloadError, this, &SearchDialog::onDownloadError);
    connect(m_browser, &RepositoryBrowser::downloadProgress, this, &SearchDialog::onDownloadProgress);
    
    // Detect package manager
    PackageManager pm = RepositoryBrowser::detectPackageManager();
    m_statusLabel->setText(QString("Using %1 package manager").arg(RepositoryBrowser::packageManagerName(pm)));
}

SearchDialog::~SearchDialog() {
}

void SearchDialog::setupUI() {
    setStyleSheet(
        "QDialog {"
        "background: qlineargradient(x1:0, y1:0, x2:0, y2:1, "
        "stop:0 #1a1a2e, stop:0.5 #16213e, stop:1 #0f3460);"
        "color: #e0e0e0;"
        "}"
    );
    
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    
    // Search bar
    QHBoxLayout* searchLayout = new QHBoxLayout();
    
    m_searchInput = new QLineEdit(this);
    m_searchInput->setPlaceholderText("Search for packages (e.g., discord, vscode, firefox)...");
    m_searchInput->setStyleSheet(
        "QLineEdit {"
        "background-color: rgba(255, 255, 255, 0.1);"
        "color: white;"
        "border: 2px solid rgba(108, 92, 231, 0.5);"
        "border-radius: 8px;"
        "padding: 10px 15px;"
        "font-size: 14px;"
        "}"
        "QLineEdit:focus {"
        "border-color: #6c5ce7;"
        "}"
    );
    connect(m_searchInput, &QLineEdit::returnPressed, this, &SearchDialog::onSearchClicked);
    
    m_searchButton = new QPushButton("Search", this);
    m_searchButton->setStyleSheet(
        "QPushButton {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #6c5ce7, stop:1 #4a90e2);"
        "color: white;"
        "font-weight: bold;"
        "border-radius: 8px;"
        "padding: 10px 25px;"
        "border: none;"
        "}"
        "QPushButton:hover {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #7d6ff0, stop:1 #5ba0f2);"
        "}"
        "QPushButton:disabled {"
        "background-color: rgba(100, 100, 100, 0.3);"
        "color: rgba(200, 200, 200, 0.5);"
        "}"
    );
    connect(m_searchButton, &QPushButton::clicked, this, &SearchDialog::onSearchClicked);
    
    searchLayout->addWidget(m_searchInput, 1);
    searchLayout->addWidget(m_searchButton);
    mainLayout->addLayout(searchLayout);
    
    // Results table
    m_resultsTable = new QTableWidget(this);
    m_resultsTable->setColumnCount(4);
    m_resultsTable->setHorizontalHeaderLabels({"Name", "Version", "Size", "Description"});
    m_resultsTable->horizontalHeader()->setStretchLastSection(true);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_resultsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_resultsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_resultsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_resultsTable->setAlternatingRowColors(true);
    m_resultsTable->setStyleSheet(
        "QTableWidget {"
        "background-color: rgba(0, 0, 0, 0.3);"
        "color: #e0e0e0;"
        "border: 2px solid rgba(255, 255, 255, 0.1);"
        "border-radius: 8px;"
        "gridline-color: rgba(255, 255, 255, 0.1);"
        "}"
        "QTableWidget::item {"
        "padding: 8px;"
        "}"
        "QTableWidget::item:selected {"
        "background-color: rgba(108, 92, 231, 0.5);"
        "}"
        "QTableWidget::item:alternate {"
        "background-color: rgba(255, 255, 255, 0.05);"
        "}"
        "QHeaderView::section {"
        "background-color: rgba(108, 92, 231, 0.3);"
        "color: white;"
        "font-weight: bold;"
        "padding: 8px;"
        "border: none;"
        "border-bottom: 1px solid rgba(255, 255, 255, 0.2);"
        "}"
    );
    connect(m_resultsTable, &QTableWidget::itemSelectionChanged, this, &SearchDialog::onSelectionChanged);
    connect(m_resultsTable, &QTableWidget::cellDoubleClicked, this, &SearchDialog::onCellDoubleClicked);
    mainLayout->addWidget(m_resultsTable, 1);
    
    // Progress bar
    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    m_progressBar->setStyleSheet(
        "QProgressBar {"
        "border: 2px solid rgba(255, 255, 255, 0.2);"
        "border-radius: 8px;"
        "text-align: center;"
        "color: #ffffff;"
        "background-color: rgba(0, 0, 0, 0.3);"
        "}"
        "QProgressBar::chunk {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #6c5ce7, stop:1 #4a90e2);"
        "border-radius: 6px;"
        "}"
    );
    mainLayout->addWidget(m_progressBar);
    
    // Status label
    m_statusLabel = new QLabel("Ready", this);
    m_statusLabel->setStyleSheet("color: #a0a0a0; font-size: 12px; background: transparent;");
    mainLayout->addWidget(m_statusLabel);
    
    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    
    m_downloadButton = new QPushButton("Download && Convert", this);
    m_downloadButton->setEnabled(false);
    m_downloadButton->setStyleSheet(
        "QPushButton {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #27ae60, stop:1 #2ecc71);"
        "color: white;"
        "font-weight: bold;"
        "border-radius: 8px;"
        "padding: 12px 30px;"
        "border: none;"
        "}"
        "QPushButton:hover {"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2ecc71, stop:1 #3dd884);"
        "}"
        "QPushButton:disabled {"
        "background-color: rgba(100, 100, 100, 0.3);"
        "color: rgba(200, 200, 200, 0.5);"
        "}"
    );
    connect(m_downloadButton, &QPushButton::clicked, this, &SearchDialog::onDownloadClicked);
    
    m_cancelButton = new QPushButton("Cancel", this);
    m_cancelButton->setStyleSheet(
        "QPushButton {"
        "background-color: rgba(231, 76, 60, 0.6);"
        "color: white;"
        "font-weight: 500;"
        "border-radius: 8px;"
        "padding: 12px 30px;"
        "border: none;"
        "}"
        "QPushButton:hover {"
        "background-color: rgba(231, 76, 60, 0.8);"
        "}"
    );
    connect(m_cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_downloadButton);
    buttonLayout->addWidget(m_cancelButton);
    mainLayout->addLayout(buttonLayout);
}

void SearchDialog::onSearchClicked() {
    QString query = m_searchInput->text().trimmed();
    if (query.isEmpty()) {
        QMessageBox::warning(this, "Search", "Please enter a search term.");
        return;
    }
    
    m_isSearching = true;
    m_searchResults.clear();
    m_resultsTable->setRowCount(0);
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, 0);  // Indeterminate
    m_statusLabel->setText("Searching...");
    updateButtonStates();
    
    m_browser->searchPackages(query);
}

void SearchDialog::onDownloadClicked() {
    int row = m_resultsTable->currentRow();
    if (row < 0 || row >= m_searchResults.size()) {
        QMessageBox::warning(this, "Download", "Please select a package to download.");
        return;
    }
    
    PackageInfo package = m_searchResults[row];
    
    // Check if sudo password is needed (for pacman)
    if (package.source == PackageManager::PACMAN) {
        QString password = requestSudoPassword();
        if (password.isEmpty()) {
            QMessageBox::information(this, "Пароль не введен", 
                "Пароль sudo необходим для загрузки пакетов через pacman.\n"
                "Операция отменена.");
            return;
        }
        m_browser->setSudoPassword(password);
    }
    
    m_isDownloading = true;
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_statusLabel->setText(QString("Downloading %1...").arg(package.name));
    updateButtonStates();
    
    // Download to temp directory
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/appalchemist-download";
    QDir dir(tempDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    
    m_browser->downloadPackage(package, tempDir);
}

void SearchDialog::onSearchCompleted(const QList<PackageInfo>& results) {
    m_isSearching = false;
    m_searchResults = results;
    m_progressBar->setVisible(false);
    
    m_resultsTable->setRowCount(results.size());
    
    for (int i = 0; i < results.size(); ++i) {
        const PackageInfo& pkg = results[i];
        
        m_resultsTable->setItem(i, 0, new QTableWidgetItem(pkg.name));
        m_resultsTable->setItem(i, 1, new QTableWidgetItem(pkg.version));
        m_resultsTable->setItem(i, 2, new QTableWidgetItem(pkg.sizeFormatted()));
        m_resultsTable->setItem(i, 3, new QTableWidgetItem(pkg.description));
    }
    
    m_statusLabel->setText(QString("Found %1 packages").arg(results.size()));
    updateButtonStates();
}

void SearchDialog::onSearchError(const QString& errorMessage) {
    m_isSearching = false;
    m_progressBar->setVisible(false);
    m_statusLabel->setText(QString("Error: %1").arg(errorMessage));
    updateButtonStates();
    
    QMessageBox::warning(this, "Search Error", errorMessage);
}

void SearchDialog::onDownloadCompleted(const QString& packagePath) {
    m_isDownloading = false;
    m_downloadedPackagePath = packagePath;
    m_progressBar->setVisible(false);
    m_statusLabel->setText(QString("Downloaded: %1").arg(packagePath));
    updateButtonStates();
    
    emit packageSelected(packagePath);
    accept();
}

void SearchDialog::onDownloadError(const QString& errorMessage) {
    m_isDownloading = false;
    m_progressBar->setVisible(false);
    m_statusLabel->setText(QString("Download error: %1").arg(errorMessage));
    updateButtonStates();
    
    // Create a more detailed error message
    QString detailedMessage = errorMessage;
    
    // Add helpful suggestions based on error
    if (errorMessage.contains("apt-get download failed") || errorMessage.contains("apt download")) {
        detailedMessage += "\n\nВозможные решения:";
        detailedMessage += "\n1. Убедитесь, что пакет существует: apt-cache search <имя>";
        detailedMessage += "\n2. Обновите кэш пакетов: sudo apt-get update";
        detailedMessage += "\n3. Проверьте подключение к интернету";
        detailedMessage += "\n4. Убедитесь, что репозитории настроены правильно";
    } else if (errorMessage.contains("dnf download failed")) {
        detailedMessage += "\n\nВозможные решения:";
        detailedMessage += "\n1. Убедитесь, что пакет существует: dnf search <имя>";
        detailedMessage += "\n2. Обновите метаданные: sudo dnf makecache";
        detailedMessage += "\n3. Проверьте подключение к интернету";
    } else if (errorMessage.contains("Package manager mismatch")) {
        detailedMessage += "\n\nПакет из другого репозитория. Попробуйте найти пакет для вашей системы.";
    }
    
    QMessageBox::warning(this, "Ошибка загрузки", detailedMessage);
}

void SearchDialog::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    if (bytesTotal > 0) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(static_cast<int>(bytesReceived * 100 / bytesTotal));
        m_statusLabel->setText(QString("Downloading... %1/%2 MB")
            .arg(bytesReceived / 1024.0 / 1024.0, 0, 'f', 1)
            .arg(bytesTotal / 1024.0 / 1024.0, 0, 'f', 1));
    }
}

void SearchDialog::onSelectionChanged() {
    updateButtonStates();
}

void SearchDialog::onCellDoubleClicked(int row, int column) {
    Q_UNUSED(column);
    if (row >= 0 && row < m_searchResults.size() && !m_isDownloading) {
        onDownloadClicked();
    }
}

void SearchDialog::updateButtonStates() {
    bool hasSelection = m_resultsTable->currentRow() >= 0;
    m_searchButton->setEnabled(!m_isSearching && !m_isDownloading);
    m_downloadButton->setEnabled(hasSelection && !m_isSearching && !m_isDownloading);
    m_searchInput->setEnabled(!m_isSearching && !m_isDownloading);
}

QString SearchDialog::requestSudoPassword() {
    bool ok;
    QString password = QInputDialog::getText(
        this,
        "Требуется пароль sudo",
        "Введите пароль для sudo:",
        QLineEdit::Password,
        "",
        &ok
    );
    
    if (ok && !password.isEmpty()) {
        return password;
    }
    
    return QString();
}

