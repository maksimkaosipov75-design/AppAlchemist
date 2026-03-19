#include "appstore_window.h"
#include <QInputDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QDir>
#include <QStringListModel>
#include <QScrollBar>
#include <QDebug>
#include <algorithm>
#include <cstdio>

AppStoreWindow::AppStoreWindow(QWidget* parent)
    : QDialog(parent)
    , m_model(new AppStoreModel(this))
    , m_browser(new RepositoryBrowser(this))
    , m_isSearching(false)
    , m_isDownloading(false)
    , m_modelInitialized(false)
    , m_currentCollection(CollectionType::All)
    , m_displayedAppsCount(0)
    , m_searchTimer(new QTimer(this))
{
    setWindowTitle("AppAlchemist Store");
    setMinimumSize(1000, 700);
    resize(1200, 800);
    
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(500); // 500ms delay for search
    
    setupUI();
    
    // Connect signals
    connect(m_model, &AppStoreModel::initialized, this, &AppStoreWindow::onModelInitialized);
    connect(m_model, &AppStoreModel::loadingProgress, this, &AppStoreWindow::onModelLoadingProgress);
    connect(m_model, &AppStoreModel::repositorySearchCompleted, this, &AppStoreWindow::onRepositorySearchResults);
    connect(m_browser, &RepositoryBrowser::downloadCompleted, this, &AppStoreWindow::onDownloadCompleted);
    connect(m_browser, &RepositoryBrowser::downloadError, this, &AppStoreWindow::onDownloadError);
    connect(m_browser, &RepositoryBrowser::downloadProgress, this, &AppStoreWindow::onDownloadProgress);
    connect(m_searchTimer, &QTimer::timeout, this, &AppStoreWindow::onSearchClicked);
    
    // Initialize model
    m_model->initialize();
    showLoadingIndicator(true);
}

AppStoreWindow::~AppStoreWindow() {
}

void AppStoreWindow::setupUI() {
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
    
    // Header with logo and search
    QHBoxLayout* headerLayout = new QHBoxLayout();
    headerLayout->setSpacing(15);
    
    QLabel* logoLabel = new QLabel("AppAlchemist Store", this);
    logoLabel->setStyleSheet(
        "QLabel {"
        "color: #ffffff;"
        "font-size: 24px;"
        "font-weight: bold;"
        "background: transparent;"
        "}"
    );
    headerLayout->addWidget(logoLabel);
    headerLayout->addStretch();
    
    setupSearchBar();
    headerLayout->addWidget(m_searchInput);
    headerLayout->addWidget(m_searchButton);
    
    mainLayout->addLayout(headerLayout);
    
    // Collections (All, Popular, New, Recommended)
    QHBoxLayout* collectionLayout = new QHBoxLayout();
    setupCollections(collectionLayout);
    mainLayout->addLayout(collectionLayout);
    
    // Categories
    setupCategories();
    QHBoxLayout* categoryLayout = new QHBoxLayout();
    categoryLayout->addWidget(new QLabel("Category:", this));
    categoryLayout->addWidget(m_categoryCombo);
    categoryLayout->addStretch();
    mainLayout->addLayout(categoryLayout);
    
    // App grid
    setupAppGrid();
    mainLayout->addWidget(m_scrollArea, 1);
    
    // Progress bar and status
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
    
    m_loadingLabel = new QLabel(this);
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    m_loadingLabel->setStyleSheet(
        "QLabel {"
        "color: #ffffff;"
        "font-size: 16px;"
        "background: transparent;"
        "}"
    );
    m_loadingLabel->setVisible(false);
    mainLayout->addWidget(m_loadingLabel);
    
    m_statusLabel = new QLabel("Ready", this);
    m_statusLabel->setStyleSheet("color: #a0a0a0; font-size: 12px; background: transparent;");
    mainLayout->addWidget(m_statusLabel);
    
    // Buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    
    m_loadMoreButton = new QPushButton("Load More", this);
    m_loadMoreButton->setVisible(false);
    m_loadMoreButton->setStyleSheet(
        "QPushButton {"
        "background: rgba(108, 92, 231, 0.6);"
        "color: white;"
        "font-weight: 500;"
        "border-radius: 8px;"
        "padding: 10px 20px;"
        "border: 1px solid rgba(108, 92, 231, 0.8);"
        "}"
        "QPushButton:hover {"
        "background: rgba(108, 92, 231, 0.8);"
        "}"
    );
    connect(m_loadMoreButton, &QPushButton::clicked, this, &AppStoreWindow::onLoadMoreClicked);
    buttonLayout->addWidget(m_loadMoreButton);
    
    buttonLayout->addStretch();
    
    m_cancelButton = new QPushButton("Close", this);
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
    buttonLayout->addWidget(m_cancelButton);
    
    mainLayout->addLayout(buttonLayout);
}

void AppStoreWindow::setupSearchBar() {
    m_searchInput = new QLineEdit(this);
    m_searchInput->setPlaceholderText("Search for applications...");
    m_searchInput->setStyleSheet(
        "QLineEdit {"
        "background-color: rgba(255, 255, 255, 0.1);"
        "color: white;"
        "border: 2px solid rgba(108, 92, 231, 0.5);"
        "border-radius: 8px;"
        "padding: 10px 15px;"
        "font-size: 14px;"
        "min-width: 300px;"
        "}"
        "QLineEdit:focus {"
        "border-color: #6c5ce7;"
        "}"
    );
    connect(m_searchInput, &QLineEdit::textChanged, this, &AppStoreWindow::onSearchTextChanged);
    connect(m_searchInput, &QLineEdit::returnPressed, this, &AppStoreWindow::onSearchClicked);
    
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
    );
    connect(m_searchButton, &QPushButton::clicked, this, &AppStoreWindow::onSearchClicked);
    
    // Setup completer for autocomplete
    m_searchCompleter = new QCompleter(this);
    m_searchCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_searchCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_searchCompleter->setMaxVisibleItems(10);
    m_searchInput->setCompleter(m_searchCompleter);
}

void AppStoreWindow::setupCollections(QHBoxLayout* collectionLayout) {
    m_collectionGroup = new QButtonGroup(this);
    
    collectionLayout->setSpacing(10);
    
    m_allButton = new QPushButton("All", this);
    m_popularButton = new QPushButton("Popular", this);
    m_newButton = new QPushButton("New", this);
    m_recommendedButton = new QPushButton("Recommended", this);
    
    QList<QPushButton*> buttons = {m_allButton, m_popularButton, m_newButton, m_recommendedButton};
    for (int i = 0; i < buttons.size(); ++i) {
        QPushButton* btn = buttons[i];
        m_collectionGroup->addButton(btn, i);
        btn->setCheckable(true);
        btn->setStyleSheet(
            "QPushButton {"
            "background: rgba(108, 92, 231, 0.3);"
            "color: white;"
            "font-weight: 500;"
            "border-radius: 8px;"
            "padding: 8px 20px;"
            "border: 2px solid rgba(108, 92, 231, 0.5);"
            "}"
            "QPushButton:checked {"
            "background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #6c5ce7, stop:1 #4a90e2);"
            "border-color: #6c5ce7;"
            "}"
            "QPushButton:hover {"
            "background: rgba(108, 92, 231, 0.5);"
            "}"
        );
        collectionLayout->addWidget(btn);
    }
    
    m_allButton->setChecked(true);
    connect(m_collectionGroup, &QButtonGroup::idClicked, 
            this, &AppStoreWindow::onCollectionChanged);
    
    collectionLayout->addStretch();
}

void AppStoreWindow::setupCategories() {
    m_categoryCombo = new QComboBox(this);
    m_categoryCombo->addItem("All Categories", "");
    
    m_categoryCombo->setStyleSheet(
        "QComboBox {"
        "background: rgba(255, 255, 255, 0.1);"
        "color: white;"
        "border: 2px solid rgba(108, 92, 231, 0.5);"
        "border-radius: 8px;"
        "padding: 8px 15px;"
        "min-width: 200px;"
        "}"
        "QComboBox:hover {"
        "border-color: #6c5ce7;"
        "}"
        "QComboBox::drop-down {"
        "border: none;"
        "}"
        "QComboBox QAbstractItemView {"
        "background: rgba(30, 30, 50, 0.95);"
        "border: 1px solid rgba(108, 92, 231, 0.5);"
        "border-radius: 8px;"
        "selection-background-color: #6c5ce7;"
        "selection-color: white;"
        "color: #ffffff;"
        "}"
    );
    
    connect(m_categoryCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            [this](int index) {
                QString category = m_categoryCombo->itemData(index).toString();
                onCategoryChanged(category);
            });
}

void AppStoreWindow::updateCategoryCombo() {
    if (!m_modelInitialized) {
        return;
    }
    
    QMap<QString, int> categoryCounts = m_model->getCategoriesWithCounts();
    QString currentCategory = m_categoryCombo->currentData().toString();
    
    m_categoryCombo->blockSignals(true);
    m_categoryCombo->clear();
    m_categoryCombo->addItem("All Categories", "");
    
    // Sort categories by count descending
    QList<QPair<QString, int>> sortedCategories;
    for (auto it = categoryCounts.begin(); it != categoryCounts.end(); ++it) {
        if (it.value() > 0) {
            sortedCategories.append(qMakePair(it.key(), it.value()));
        }
    }
    std::sort(sortedCategories.begin(), sortedCategories.end(),
              [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
                  return a.second > b.second;
              });
    
    for (const auto& pair : sortedCategories) {
        QString displayName = AppStreamMetadata::categoryDisplayName(pair.first);
        m_categoryCombo->addItem(QString("%1 (%2)").arg(displayName).arg(pair.second), pair.first);
    }
    
    // Restore selection if possible
    int index = m_categoryCombo->findData(currentCategory);
    if (index >= 0) {
        m_categoryCombo->setCurrentIndex(index);
    }
    
    m_categoryCombo->blockSignals(false);
}

void AppStoreWindow::setupAppGrid() {
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setStyleSheet(
        "QScrollArea {"
        "background: transparent;"
        "border: none;"
        "}"
        "QScrollBar:vertical {"
        "background: rgba(0, 0, 0, 0.3);"
        "width: 12px;"
        "border-radius: 6px;"
        "}"
        "QScrollBar::handle:vertical {"
        "background: rgba(108, 92, 231, 0.6);"
        "border-radius: 6px;"
        "min-height: 30px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "background: rgba(108, 92, 231, 0.8);"
        "}"
    );
    
    m_scrollContent = new QWidget();
    m_gridLayout = new QGridLayout(m_scrollContent);
    m_gridLayout->setSpacing(20);
    m_gridLayout->setContentsMargins(10, 10, 10, 10);
    
    m_scrollArea->setWidget(m_scrollContent);
}

void AppStoreWindow::updateAppGrid() {
    // Clear existing cards
    QLayoutItem* item;
    while ((item = m_gridLayout->takeAt(0)) != nullptr) {
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    
    if (!m_modelInitialized) {
        showLoadingIndicator(true);
        m_statusLabel->setText("Initializing...");
        return;
    }
    
    showLoadingIndicator(false);
    
    // Get apps to display
    if (!m_currentSearchQuery.isEmpty()) {
        m_currentApps = m_model->searchApps(m_currentSearchQuery);
        
        // If no results in AppStream, trigger repository search
        if (m_currentApps.isEmpty()) {
            m_model->searchAppsInRepositories(m_currentSearchQuery);
            m_statusLabel->setText("Searching in repositories...");
            return; // Will update when results arrive
        }
    } else if (!m_currentCategory.isEmpty()) {
        m_currentApps = m_model->getAppsByCategory(m_currentCategory);
    } else {
        m_currentApps = m_model->getApps(m_currentCollection);
    }
    
    // Display apps (paginated)
    int appsToShow = qMin(m_displayedAppsCount + APPS_PER_PAGE, m_currentApps.size());
    
    int row = 0;
    int col = 0;
    const int colsPerRow = 4;
    
    for (int i = m_displayedAppsCount; i < appsToShow; ++i) {
        if (i >= m_currentApps.size()) break;
        
        // Debug: log app data before creating card
        const AppInfo& appData = m_currentApps[i];
        fprintf(stderr, "AppStoreWindow: Creating card for app[%d]: name='%s' pkgName='%s' displayName='%s'\n",
                i, appData.name.toUtf8().constData(), 
                appData.packageName.toUtf8().constData(),
                appData.displayName.toUtf8().constData());
        fprintf(stderr, "  name hex: ");
        for (int j = 0; j < appData.name.length() && j < 30; j++) {
            fprintf(stderr, "%04x ", appData.name[j].unicode());
        }
        fprintf(stderr, "\n");
        fflush(stderr);
        
        AppCardWidget* card = new AppCardWidget(m_currentApps[i], m_scrollContent);
        connect(card, &AppCardWidget::downloadClicked, this, &AppStoreWindow::onAppDownloadClicked);
        connect(card, &AppCardWidget::detailsClicked, this, &AppStoreWindow::onAppDetailsClicked);
        
        m_gridLayout->addWidget(card, row, col);
        
        col++;
        if (col >= colsPerRow) {
            col = 0;
            row++;
        }
    }
    
    m_displayedAppsCount = appsToShow;
    
    // Show/hide "Load More" button
    bool hasMore = appsToShow < m_currentApps.size();
    m_loadMoreButton->setVisible(hasMore);
    
    if (m_currentApps.isEmpty()) {
        m_statusLabel->setText("No applications found");
    } else {
        m_statusLabel->setText(QString("Showing %1 of %2 applications").arg(appsToShow).arg(m_currentApps.size()));
    }
}

void AppStoreWindow::onSearchTextChanged(const QString& text) {
    m_currentSearchQuery = text;
    
    // Update completer model
    if (m_modelInitialized) {
        QStringList appNames;
        for (const AppInfo& app : m_model->getAllApps()) {
            appNames.append(app.name);
        }
        QStringListModel* model = qobject_cast<QStringListModel*>(m_searchCompleter->model());
        if (!model) {
            model = new QStringListModel(appNames, m_searchCompleter);
            m_searchCompleter->setModel(model);
        } else {
            model->setStringList(appNames);
        }
    }
    
    // Reset pagination
    m_displayedAppsCount = 0;
    
    // Trigger search with delay
    m_searchTimer->start();
}

void AppStoreWindow::onSearchClicked() {
    m_searchTimer->stop();
    m_displayedAppsCount = 0;
    updateAppGrid();
}

void AppStoreWindow::onCollectionChanged(int id) {
    m_currentCollection = static_cast<CollectionType>(id);
    m_currentSearchQuery.clear();
    m_currentCategory.clear();
    m_categoryCombo->setCurrentIndex(0);
    m_searchInput->clear();
    m_displayedAppsCount = 0;
    updateAppGrid();
}

void AppStoreWindow::onCategoryChanged(const QString& category) {
    if (!m_modelInitialized) {
        // Wait for initialization
        m_statusLabel->setText("Please wait for applications to load...");
        return;
    }
    
    m_currentCategory = category;
    m_currentSearchQuery.clear();
    m_searchInput->clear();
    m_displayedAppsCount = 0;
    showLoadingIndicator(false);
    updateAppGrid();
}

void AppStoreWindow::onAppDownloadClicked(const AppInfo& appInfo) {
    // Check if sudo password is needed
    if (appInfo.source == PackageManager::PACMAN) {
        QString password = requestSudoPassword();
        if (password.isEmpty()) {
            QMessageBox::information(this, "Password Required", 
                "Sudo password is required for downloading packages via pacman.\n"
                "Operation cancelled.");
            return;
        }
        m_browser->setSudoPassword(password);
    }
    
    m_isDownloading = true;
    m_progressBar->setVisible(true);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_statusLabel->setText(QString("Downloading %1...").arg(appInfo.name));
    updateButtonStates();
    
    // Convert AppInfo to PackageInfo
    PackageInfo pkg;
    pkg.name = appInfo.packageName.isEmpty() ? appInfo.name : appInfo.packageName;
    pkg.version = appInfo.version;
    pkg.description = appInfo.description;
    pkg.size = appInfo.size;
    pkg.architecture = appInfo.architecture;
    pkg.repository = appInfo.repository;
    pkg.downloadUrl = appInfo.downloadUrl;
    pkg.source = appInfo.source;
    
    // Download to temp directory
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/appalchemist-downloads";
    QDir dir(tempDir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
    
    m_browser->downloadPackage(pkg, tempDir);
}

void AppStoreWindow::onAppDetailsClicked(const AppInfo& appInfo) {
    AppDetailsDialog dialog(appInfo, this);
    connect(&dialog, &AppDetailsDialog::downloadClicked, this, &AppStoreWindow::onAppDownloadClicked);
    dialog.exec();
}

void AppStoreWindow::onDownloadCompleted(const QString& packagePath) {
    m_isDownloading = false;
    m_downloadedPackagePath = packagePath;
    m_progressBar->setVisible(false);
    m_statusLabel->setText(QString("Downloaded: %1").arg(packagePath));
    updateButtonStates();
    
    emit packageSelected(packagePath);
    accept();
}

void AppStoreWindow::onDownloadError(const QString& errorMessage) {
    m_isDownloading = false;
    m_progressBar->setVisible(false);
    m_statusLabel->setText(QString("Error: %1").arg(errorMessage));
    updateButtonStates();
    
    QMessageBox::warning(this, "Download Error", errorMessage);
}

void AppStoreWindow::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal) {
    if (bytesTotal > 0) {
        int progress = (bytesReceived * 100) / bytesTotal;
        m_progressBar->setValue(progress);
    }
}

void AppStoreWindow::onModelInitialized() {
    m_modelInitialized = true;
    showLoadingIndicator(false);
    
    // Update completer
    QStringList appNames;
    for (const AppInfo& app : m_model->getAllApps()) {
        appNames.append(app.name);
    }
    QStringListModel* model = new QStringListModel(appNames, m_searchCompleter);
    m_searchCompleter->setModel(model);
    
    // Update category list based on actual app categories
    updateCategoryCombo();
    
    // Initial display
    updateAppGrid();
}

void AppStoreWindow::onModelLoadingProgress(int current, int total) {
    m_progressBar->setRange(0, total);
    m_progressBar->setValue(current);
    m_statusLabel->setText(QString("Loading applications: %1/%2").arg(current).arg(total));
}

void AppStoreWindow::onRepositorySearchResults(QList<AppInfo> results) {
    // Update app grid with repository search results
    if (!results.isEmpty()) {
        // Add to current apps if searching
        if (!m_currentSearchQuery.isEmpty()) {
            m_currentApps.append(results);
            m_displayedAppsCount = 0; // Reset pagination
            updateAppGrid();
        } else {
            // Refresh grid if not searching
            updateAppGrid();
        }
        // Update categories when new apps are added
        updateCategoryCombo();
    }
}

void AppStoreWindow::onLoadMoreClicked() {
    updateAppGrid();
}

void AppStoreWindow::updateButtonStates() {
    // Update button states based on current operation
}

QString AppStoreWindow::requestSudoPassword() {
    bool ok;
    QString password = QInputDialog::getText(this, tr("Sudo Password Required"),
                                             tr("Sudo password is required for downloading packages via pacman.\n"
                                                "Please enter your password:"),
                                             QLineEdit::Password, QString(), &ok);
    if (ok && !password.isEmpty()) {
        return password;
    }
    return QString();
}

void AppStoreWindow::showLoadingIndicator(bool show) {
    m_loadingLabel->setVisible(show);
    m_progressBar->setVisible(show);
    if (show) {
        m_loadingLabel->setText("Loading applications...");
        m_progressBar->setRange(0, 0); // Indeterminate
    } else {
        m_loadingLabel->setText("");
    }
}

