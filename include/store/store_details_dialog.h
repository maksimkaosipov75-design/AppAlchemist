#ifndef STORE_DETAILS_DIALOG_H
#define STORE_DETAILS_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <QTextEdit>
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <QVBoxLayout>
#include "store_appentry.h"
#include "store_ratings.h"

class StoreDetailsDialog : public QDialog {
    Q_OBJECT

public:
    explicit StoreDetailsDialog(const StoreAppEntry& app,
                                const QPixmap& icon,
                                StoreRatings* ratings,
                                QWidget* parent = nullptr);

signals:
    void downloadRequested(const StoreAppEntry& app);

private slots:
    void onSubmitRating();

private:
    void refreshRatings();

    StoreAppEntry m_app;
    StoreRatings* m_ratings;
    QLabel* m_titleLabel;
    QLabel* m_summaryLabel;
    QTextEdit* m_description;
    QListWidget* m_ratingsList;
    QComboBox* m_ratingCombo;
    QTextEdit* m_commentInput;
    QPushButton* m_submitButton;
    QPushButton* m_downloadButton;
};

#endif // STORE_DETAILS_DIALOG_H

