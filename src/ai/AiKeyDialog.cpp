#include "AiKeyDialog.h"

#include "AiEnv.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

namespace {
const QString kKeyPage = QStringLiteral("https://aistudio.google.com/apikey");
}

AiKeyDialog::AiKeyDialog(QWidget *parent)
    : QDialog(parent)
    , m_net(new QNetworkAccessManager(this))
{
    setWindowTitle(tr("Gemini API key"));
    setModal(true);
    setMinimumWidth(460);

    auto *icon = new QLabel(this);
    icon->setObjectName(QStringLiteral("keyBadge"));
    icon->setFixedSize(44, 44);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/key.svg"), Theme::byName(SettingsManager::instance().theme()).accent, 22));
    auto *title = new QLabel(tr("Use your own Gemini API key"), this);
    title->setObjectName(QStringLiteral("keyTitle"));
    auto *sub = new QLabel(tr("QODE ships without a key. Create a free one in Google AI Studio and paste it here."), this);
    sub->setObjectName(QStringLiteral("keySub"));
    sub->setWordWrap(true);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(title);
    titles->addWidget(sub);
    auto *head = new QHBoxLayout;
    head->setSpacing(14);
    head->addWidget(icon, 0, Qt::AlignTop);
    head->addLayout(titles, 1);

    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("keyEdit"));
    m_edit->setEchoMode(QLineEdit::Password);
    m_edit->setPlaceholderText(tr("AIza…"));
    m_edit->setClearButtonEnabled(true);
    m_edit->setMinimumHeight(38);
    m_edit->setAttribute(Qt::WA_InputMethodEnabled, false);
    m_show = new QPushButton(tr("Show"), this);
    m_show->setObjectName(QStringLiteral("keyGhost"));
    m_show->setCheckable(true);
    m_show->setMinimumHeight(38);
    auto *field = new QHBoxLayout;
    field->setSpacing(8);
    field->addWidget(m_edit, 1);
    field->addWidget(m_show);

    auto *link = new QLabel(tr("<a href=\"%1\">Get a key from Google AI Studio →</a>").arg(kKeyPage), this);
    link->setObjectName(QStringLiteral("keyLink"));
    link->setTextFormat(Qt::RichText);
    link->setOpenExternalLinks(false);
    connect(link, &QLabel::linkActivated, this, [](const QString &u) { QDesktopServices::openUrl(QUrl(u)); });

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("keyStatus"));
    m_status->setWordWrap(true);

    auto *note = new QLabel(tr("The key is stored only on this computer, in a file only you can read (%1). It is sent to Google over HTTPS "
                               "and nowhere else. Files such as .env, private keys and credentials are never attached to a prompt.")
                                .arg(AiEnv::storedKeyPath()),
                            this);
    note->setObjectName(QStringLiteral("keyNote"));
    note->setWordWrap(true);
    note->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_remove = new QPushButton(tr("Remove key"), this);
    m_remove->setObjectName(QStringLiteral("keyGhost"));
    m_test = new QPushButton(tr("Test"), this);
    m_test->setObjectName(QStringLiteral("keyGhost"));
    auto *cancel = new QPushButton(tr("Cancel"), this);
    cancel->setObjectName(QStringLiteral("keyGhost"));
    m_save = new QPushButton(tr("Save"), this);
    m_save->setObjectName(QStringLiteral("keyPrimary"));
    m_save->setDefault(true);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addWidget(m_remove);
    buttons->addStretch(1);
    buttons->addWidget(m_test);
    buttons->addWidget(cancel);
    buttons->addWidget(m_save);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 20);
    root->setSpacing(14);
    root->addLayout(head);
    root->addLayout(field);
    root->addWidget(link);
    root->addWidget(m_status);
    root->addWidget(note);
    root->addSpacing(2);
    root->addLayout(buttons);

    connect(m_show, &QPushButton::toggled, this, [this](bool on) {
        m_edit->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        m_show->setText(on ? tr("Hide") : tr("Show"));
    });
    connect(m_edit, &QLineEdit::textChanged, this, [this] {
        setStatus({}, true);
        updateState();
    });
    connect(m_edit, &QLineEdit::returnPressed, this, &AiKeyDialog::save);
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_save, &QPushButton::clicked, this, &AiKeyDialog::save);
    connect(m_test, &QPushButton::clicked, this, &AiKeyDialog::test);
    connect(m_remove, &QPushButton::clicked, this, [this] {
        AiEnv::clearKey();
        m_edit->clear();
        setStatus(tr("The saved key was removed."), true);
        updateState();
        accept();
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AiKeyDialog::applyTheme);

    applyTheme();
    updateState();
}

void AiKeyDialog::setStatus(const QString &text, bool good)
{
    m_status->setProperty("good", good);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void AiKeyDialog::updateState()
{
    const bool has = !m_edit->text().trimmed().isEmpty();
    m_save->setEnabled(has);
    m_test->setEnabled(has && !m_reply);
    m_remove->setVisible(AiEnv::hasStoredKey());
    if (!has && m_status->text().isEmpty())
        m_status->setVisible(false);
}

void AiKeyDialog::test()
{
    const QString key = m_edit->text().trimmed();
    if (key.isEmpty() || m_reply)
        return;
    QNetworkRequest req(QUrl(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models?pageSize=1")));
    req.setRawHeader("x-goog-api-key", key.toUtf8()); // header, never the URL
    req.setTransferTimeout(15000);
    m_reply = m_net->get(req);
    setStatus(tr("Checking the key…"), true);
    updateState();
    connect(m_reply, &QNetworkReply::finished, this, [this, r = m_reply] {
        r->deleteLater();
        m_reply = nullptr;
        const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (code == 200)
            setStatus(tr("✓ The key works."), true);
        else if (code == 400 || code == 401 || code == 403)
            setStatus(tr("✗ Google rejected this key. Check that you copied all of it."), false);
        else if (code == 0)
            setStatus(tr("✗ Could not reach Google: %1").arg(r->errorString()), false);
        else
            setStatus(tr("✗ Google answered with HTTP %1.").arg(code), false);
        updateState();
    });
}

void AiKeyDialog::save()
{
    const QString key = m_edit->text().trimmed();
    if (key.isEmpty())
        return;
    QString error;
    if (!AiEnv::saveKey(key, &error)) {
        setStatus(tr("✗ Could not save the key: %1").arg(error), false);
        return;
    }
    accept();
}

void AiKeyDialog::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    setStyleSheet(QStringLiteral(
                      "AiKeyDialog { background: %1; }"
                      "QLabel { background: transparent; color: %2; }"
                      "QLabel#keyBadge { background: %3; border: 1px solid %4; border-radius: 12px; }"
                      "QLabel#keyTitle { font-size: 13pt; font-weight: 600; }"
                      "QLabel#keySub, QLabel#keyNote { color: %5; }"
                      "QLabel#keyNote { font-size: 8pt; }"
                      "QLabel#keyLink { color: %6; }"
                      "QLabel#keyStatus { padding: 8px 10px; border-radius: 8px; background: %3; }"
                      "QLabel#keyStatus[good=\"true\"] { color: %2; }"
                      "QLabel#keyStatus[good=\"false\"] { color: %7; border: 1px solid %7; }"
                      "QLineEdit#keyEdit { background: %3; color: %2; border: 1px solid %4; border-radius: 9px; padding: 0 12px; "
                      "selection-background-color: %6; }"
                      "QLineEdit#keyEdit:focus { border-color: %6; }"
                      "QPushButton { border-radius: 9px; padding: 8px 18px; border: 1px solid %4; background: %3; color: %2; }"
                      "QPushButton:hover { border-color: %6; }"
                      "QPushButton:disabled { color: %5; border-color: %4; }"
                      "QPushButton#keyPrimary { background: %6; border-color: %6; color: %8; font-weight: 600; }"
                      "QPushButton#keyPrimary:hover { border-color: %2; }"
                      "QPushButton#keyPrimary:disabled { background: %3; border-color: %4; color: %5; }"
                      "QPushButton:checked { border-color: %6; }")
                      .arg(t.window.name(), t.editorFg.name(), t.panel.name(), t.border.name(), t.textMuted.name(), t.accent.name(), t.danger.name(),
                           t.onAccent().name()));
}
