#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;

// Where the user pastes their own Google AI Studio key. The key is stored in an owner-only file (AiEnv::saveKey).
class AiKeyDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AiKeyDialog(QWidget *parent = nullptr);

private:
    void applyTheme();
    void updateState();
    void test();
    void save();
    void setStatus(const QString &text, bool good);

    QLineEdit *m_edit;
    QPushButton *m_show;
    QPushButton *m_test;
    QPushButton *m_save;
    QPushButton *m_remove;
    QLabel *m_status;
    QNetworkAccessManager *m_net;
    QNetworkReply *m_reply = nullptr;
};
