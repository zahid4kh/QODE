#include "ClaudeCodePanel.h"
#include "platform/Platform.h"

#include "agent/AgentTools.h"
#include "agent/AgentTurnView.h"
#include "ai/AiChangeCard.h"
#include "ai/AiEdits.h"
#include "ai/AiEnv.h"
#include "ai/AiInput.h"
#include "ai/AiKeyDialog.h"
#include "ai/AiQuote.h"
#include "app/DesktopNotify.h"
#include "claudecode/ClaudeCodeCli.h"
#include "claudecode/ClaudeCodeHistory.h"
#include "claudecode/ClaudeCodeInfo.h"
#include "claudecode/ClaudeCodeSession.h"
#include "claudecode/ClaudeCodeTasks.h"
#include "claudecode/ContextMeter.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "media/CopyButton.h"
#include "media/MarkdownView.h"
#include "palette/FuzzyMatcher.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "editor/TextEncoding.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <algorithm>
#include <memory>

namespace claudecode {

namespace {
const QString kName = QStringLiteral("Claude Code");

struct Choice {
    const char *id, *label, *hint;
};
// Aliases the CLI resolves to the newest model of each family; "" leaves the account's default.
const Choice kModels[] = {{"", "Default", "the default model of your account"},
                          {"opus", "Opus", "most capable for coding"},
                          {"sonnet", "Sonnet", "fast and capable"},
                          {"haiku", "Haiku", "fastest, lowest cost"},
                          {"fable", "Fable", "most capable, highest cost"}};
const Choice kModes[] = {{"default", "Ask before edits", "asks before editing files or running commands"},
                         {"acceptEdits", "Accept edits", "edits files without asking, still asks before commands"},
                         {"plan", "Plan mode", "explores and plans, changes nothing until you approve the plan"},
                         {"bypassPermissions", "Bypass permissions", "never asks: edits and runs anything"}};

QString str(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

// "claude-opus-5-5" -> "Opus 5.5"; anything else as it is.
QString prettyModel(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^claude-([a-z]+)-(\\d+)(?:-(\\d{1,2}))?(?:-\\d{8})?(\\[1m\\])?$"));
    const QRegularExpressionMatch m = re.match(id);
    if (!m.hasMatch())
        return id;
    QString name = m.captured(1);
    name[0] = name.at(0).toUpper();
    return m.captured(3).isEmpty() ? QStringLiteral("%1 %2").arg(name, m.captured(2)) : QStringLiteral("%1 %2.%3").arg(name, m.captured(2), m.captured(3));
}

QString modeLabel(const QString &id)
{
    for (const Choice &c : kModes)
        if (id == QLatin1String(c.id))
            return QString::fromLatin1(c.label);
    return id;
}

// The text of a tool_result block (a string, or text blocks).
QString resultText(const QJsonObject &block)
{
    const QJsonValue c = block.value(QStringLiteral("content"));
    if (c.isString())
        return c.toString();
    QString out;
    for (const QJsonValue &v : c.toArray())
        if (v.toObject().value(QStringLiteral("type")).toString() == QLatin1String("text"))
            out += v.toObject().value(QStringLiteral("text")).toString();
    return out;
}

// A tool call in full, for the IN part of its folded-out row: a Bash command as it is, anything else one "key: value"
// per input field (multi-line values indented under their key).
QString inputText(const QString &name, const QJsonObject &in)
{
    if (name == QLatin1String("Bash"))
        return in.value(QStringLiteral("command")).toString();
    QStringList lines;
    for (auto it = in.begin(); it != in.end(); ++it) {
        const QJsonValue v = it.value();
        QString value;
        if (v.isString())
            value = v.toString();
        else if (v.isObject())
            value = QString::fromUtf8(QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented)).trimmed();
        else if (v.isArray())
            value = QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented)).trimmed();
        else
            value = v.toVariant().toString();
        if (value.contains(QLatin1Char('\n')))
            lines << it.key() + QLatin1Char(':') << QStringLiteral("  ") + value.replace(QLatin1Char('\n'), QStringLiteral("\n  "));
        else
            lines << it.key() + QStringLiteral(": ") + value;
    }
    return lines.join(QLatin1Char('\n'));
}

QString firstLine(const QString &text, int max = 160)
{
    for (const QString &l : text.split(QLatin1Char('\n'))) {
        const QString t = l.trimmed();
        if (!t.isEmpty())
            return t.size() > max ? t.left(max) + QStringLiteral("…") : t;
    }
    return {};
}

int lineCount(const QString &text)
{
    int n = 0;
    for (const QString &l : text.split(QLatin1Char('\n')))
        if (!l.trimmed().isEmpty())
            ++n;
    return n;
}

// Lines of a text as an editor counts them ("" has none, a final newline does not start another).
int textLines(const QString &text)
{
    if (text.isEmpty())
        return 0;
    return int(text.count(QLatin1Char('\n'))) + (text.endsWith(QLatin1Char('\n')) ? 0 : 1);
}

bool editsFiles(const QString &tool)
{
    return tool == QLatin1String("Edit") || tool == QLatin1String("MultiEdit") || tool == QLatin1String("Write") || tool == QLatin1String("NotebookEdit");
}

// Error text that means the CLI is not signed in (any more).
bool authProblem(const QString &text)
{
    for (const char *marker : {"/login", "Invalid API key", "authentication_error", "OAuth token", "Not logged in", "not logged in", "401"})
        if (text.contains(QLatin1String(marker)))
            return true;
    return false;
}

QString when(const QDateTime &t)
{
    const qint64 s = t.secsTo(QDateTime::currentDateTime());
    if (s < 60)
        return ClaudeCodePanel::tr("just now");
    if (s < 3600)
        return ClaudeCodePanel::tr("%n min ago", nullptr, int(s / 60));
    if (s < 86400)
        return ClaudeCodePanel::tr("%n h ago", nullptr, int(s / 3600));
    return QLocale().toString(t.date(), QLocale::ShortFormat);
}

constexpr int kColumn = 920; // the transcript's and input's width on a wide window

QLabel *wrapLabel(const QString &text, QWidget *parent, const char *name = nullptr)
{
    auto *l = new QLabel(text, parent);
    l->setWordWrap(true);
    if (name)
        l->setObjectName(QLatin1String(name));
    return l;
}
} // namespace

ClaudeCodePanel::ClaudeCodePanel(QWidget *parent)
    : QWidget(parent)
    , m_auth(new Auth(this))
    , m_session(new Session(this))
    , m_statsTimer(new QTimer(this))
    , m_stopTimer(new QTimer(this))
    , m_changedTimer(new QTimer(this))
    , m_selectionTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("agentRoot"));
    setAttribute(Qt::WA_StyledBackground);
    m_statsTimer->setInterval(250);
    connect(m_statsTimer, &QTimer::timeout, this, &ClaudeCodePanel::updateStats);
    m_stopTimer->setSingleShot(true);
    m_stopTimer->setInterval(6000);
    connect(m_stopTimer, &QTimer::timeout, this, [this] {
        // The CLI did not end the turn after the interrupt: end the process (the conversation can still be resumed).
        m_session->stop();
        addNote(tr("Stopped."));
        finishTurn();
        afterTurn(false);
    });
    m_changedTimer->setSingleShot(true);
    m_changedTimer->setInterval(400);
    connect(m_changedTimer, &QTimer::timeout, this, &ClaudeCodePanel::projectChanged);
    m_selectionTimer->setSingleShot(true);
    m_selectionTimer->setInterval(120);
    connect(m_selectionTimer, &QTimer::timeout, this, &ClaudeCodePanel::updateSelection);

    // --- header -------------------------------------------------------------------------------------------------
    auto *logo = new QLabel(this);
    logo->setObjectName(QStringLiteral("ccLogo"));
    logo->setFixedSize(16, 16);
    auto *title = new QLabel(kName, this);
    title->setObjectName(QStringLiteral("panelTitle"));
    m_accountBtn = new QToolButton(this);
    Icons::bind(m_accountBtn, QStringLiteral(":/new-icons/key.svg"));
    m_accountBtn->setToolTip(tr("Claude Code sign-in"));
    m_accountBtn->setAutoRaise(true);
    m_accountBtn->setPopupMode(QToolButton::InstantPopup);
    auto *accountMenu = new QMenu(m_accountBtn);
    m_accountBtn->setMenu(accountMenu);
    connect(accountMenu, &QMenu::aboutToShow, this, &ClaudeCodePanel::rebuildAccountMenu);
    m_historyBtn = new QToolButton(this);
    Icons::bind(m_historyBtn, QStringLiteral(":/new-icons/scroll-text.svg"));
    m_historyBtn->setToolTip(tr("Claude Code conversations of this project"));
    m_historyBtn->setAutoRaise(true);
    m_historyBtn->setPopupMode(QToolButton::InstantPopup);
    auto *historyMenu = new QMenu(m_historyBtn);
    m_historyBtn->setMenu(historyMenu);
    connect(historyMenu, &QMenu::aboutToShow, this, &ClaudeCodePanel::rebuildHistoryMenu);
    m_newBtn = new QToolButton(this);
    Icons::bind(m_newBtn, QStringLiteral(":/new-icons/plus.svg"));
    m_newBtn->setToolTip(tr("New conversation"));
    m_newBtn->setAutoRaise(true);
    connect(m_newBtn, &QToolButton::clicked, this, &ClaudeCodePanel::newConversation);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("agentHeader"));
    header->setAttribute(Qt::WA_StyledBackground);
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 0, 4, 0);
    hl->setSpacing(6);
    hl->addWidget(logo);
    hl->addWidget(title, 1);
    hl->addWidget(m_accountBtn);
    hl->addWidget(m_historyBtn);
    hl->addWidget(m_newBtn);

    m_pages = new QStackedWidget(this);
    buildSetupPage();
    buildChatPage();

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(header);
    lay->addWidget(m_pages, 1);

    // --- wiring -------------------------------------------------------------------------------------------------
    connect(m_auth, &Auth::statusChanged, this, &ClaudeCodePanel::updatePage);
    connect(m_auth, &Auth::loginUrlReady, this, [this] { showSetup(Setup::SigningIn); });
    connect(m_auth, &Auth::loginWaitingForCode, this, [this] {
        showSetup(Setup::SigningIn, m_codeSubmitted ? tr("That code was not accepted. Copy the whole code again from the browser and paste it here.") : QString());
        m_codeSubmitted = false;
    });
    connect(m_auth, &Auth::loginFinished, this, [this](bool ok, const QString &message) {
        m_codeSubmitted = false;
        if (ok)
            showSetup(Setup::Checking); // statusChanged follows
        else
            showSetup(Setup::SignIn, message.isEmpty() ? QString() : tr("Sign-in failed: %1\nStart again and paste the newest code.").arg(message));
    });
    connect(m_session, &Session::messageReceived, this, &ClaudeCodePanel::onMessage);
    connect(m_session, &Session::permissionRequested, this, &ClaudeCodePanel::onPermission);
    connect(m_session, &Session::permissionCancelled, this, [this](const QString &id) {
        if (QWidget *card = m_permissions.take(id))
            card->deleteLater();
        if (m_permissions.isEmpty())
            closeNotifications(false);
    });
    connect(m_session, &Session::initialized, this, [this](const QJsonObject &info) {
        m_init = info;
        m_commands.clear();
        for (const QJsonValue &v : info.value(QStringLiteral("commands")).toArray()) {
            const QJsonObject c = v.toObject();
            QString help = str(c, "description").simplified();
            if (help.size() > 90)
                help = help.left(90) + QStringLiteral("…");
            m_commands << QStringLiteral("/%1   %2").arg(str(c, "name"), help);
        }
    });
    connect(m_session, &Session::exited, this, &ClaudeCodePanel::onExited);
    connect(m_session, &Session::answered, this, [this](const QString &id, const QJsonObject &response, const QString &error) {
        if (auto done = m_queries.take(id))
            done(response, error);
    });
    connect(&DesktopNotify::instance(), &DesktopNotify::activated, this, [this](const QString &key) {
        if (key.startsWith(QStringLiteral("claudecode-%1-").arg(quintptr(this), 0, 16)))
            emit showRequested();
    });
    // Back in the window: what the notifications said is on screen now.
    connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this] {
        if (window()->isActiveWindow())
            closeNotifications();
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &ClaudeCodePanel::applyTheme);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &ClaudeCodePanel::refreshEmptyHint); // icon tints

    applyTheme();
    setBusy(false);
    showSetup(Setup::Checking);
    showEmptyHint();
}

ClaudeCodePanel::~ClaudeCodePanel()
{
    closeNotifications();
    m_session->disconnect(this);
    m_session->stop();
}

// --- setup page: install and sign-in ----------------------------------------------------------------------------

void ClaudeCodePanel::buildSetupPage()
{
    m_setup = new QWidget;
    m_setup->setObjectName(QStringLiteral("aiList"));
    auto *col = new QWidget(m_setup);
    col->setMaximumWidth(520);
    m_setupCol = new QVBoxLayout(col);
    m_setupCol->setContentsMargins(16, 16, 16, 16);
    m_setupCol->setSpacing(10);

    m_setupTitle = new QLabel(kName, col);
    m_setupTitle->setObjectName(QStringLiteral("ccSetupTitle"));
    m_setupText = wrapLabel(QString(), col);
    m_setupText->setTextFormat(Qt::RichText);
    m_setupText->setOpenExternalLinks(true);
    m_setupError = wrapLabel(QString(), col, "aiError");
    m_setupError->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // Not installed: the install command.
    m_installRow = new QWidget(col);
    {
        auto *l = new QHBoxLayout(m_installRow);
        l->setContentsMargins(0, 0, 0, 0);
        auto *cmd = new QLineEdit(installCommand(), m_installRow);
        cmd->setObjectName(QStringLiteral("ccCode"));
        cmd->setReadOnly(true);
        auto *copy = new QPushButton(tr("Copy"), m_installRow);
        copy->setObjectName(QStringLiteral("agentFooterBtn"));
        connect(copy, &QPushButton::clicked, this, [] { QApplication::clipboard()->setText(installCommand()); });
        l->addWidget(cmd, 1);
        l->addWidget(copy);
    }

    // Signed out: the three ways in.
    m_signInBox = new QWidget(col);
    {
        auto *l = new QVBoxLayout(m_signInBox);
        l->setContentsMargins(0, 4, 0, 0);
        l->setSpacing(8);
        auto choice = [this](const QString &title, const QString &hint) {
            auto *b = new QPushButton(title + QLatin1Char('\n') + hint, m_signInBox);
            b->setObjectName(QStringLiteral("ccChoice"));
            b->setCursor(Qt::PointingHandCursor);
            return b;
        };
        QPushButton *account = choice(tr("Claude account"), tr("Use your Pro, Max, Team or Enterprise subscription"));
        QPushButton *console = choice(tr("Anthropic Console"), tr("Pay as you go, billed to your Console organization"));
        m_useKeyBtn = choice(tr("QODE's Claude API key"), QString());
        connect(account, &QPushButton::clicked, this, [this] { startLogin(false); });
        connect(console, &QPushButton::clicked, this, [this] { startLogin(true); });
        connect(m_useKeyBtn, &QPushButton::clicked, this, [this] {
            if (AiEnv::apiKey(AiEnv::Provider::Claude, m_root).isEmpty()) {
                AiKeyDialog dlg(AiEnv::Provider::Claude, this);
                dlg.exec();
                if (AiEnv::apiKey(AiEnv::Provider::Claude, m_root).isEmpty()) {
                    showSetup(Setup::SignIn);
                    return;
                }
            }
            setAuthMode(AuthMode::ApiKey);
            m_session->stop();
            showSetup(Setup::Checking);
            m_auth->refresh();
        });
        l->addWidget(account);
        l->addWidget(console);
        l->addWidget(m_useKeyBtn);
    }

    // Signing in: the link and the code field.
    m_loginBox = new QWidget(col);
    {
        auto *l = new QVBoxLayout(m_loginBox);
        l->setContentsMargins(0, 4, 0, 0);
        l->setSpacing(8);
        m_loginText = wrapLabel(QString(), m_loginBox);
        auto *links = new QHBoxLayout;
        auto *open = new QPushButton(tr("Open the sign-in page"), m_loginBox);
        open->setObjectName(QStringLiteral("agentFooterBtn"));
        auto *copy = new QPushButton(tr("Copy link"), m_loginBox);
        copy->setObjectName(QStringLiteral("agentFooterBtn"));
        connect(open, &QPushButton::clicked, this, [this] {
            if (!m_auth->loginUrl().isEmpty())
                QDesktopServices::openUrl(QUrl(m_auth->loginUrl()));
        });
        connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_auth->loginUrl()); });
        links->addWidget(open);
        links->addWidget(copy);
        links->addStretch(1);
        auto *codeRow = new QHBoxLayout;
        m_codeEdit = new QLineEdit(m_loginBox);
        m_codeEdit->setObjectName(QStringLiteral("aiCustomModel"));
        m_codeEdit->setPlaceholderText(tr("Paste the code from the browser"));
        m_codeBtn = new QPushButton(tr("Continue"), m_loginBox);
        m_codeBtn->setObjectName(QStringLiteral("ccPrimary"));
        auto submit = [this] {
            if (m_codeEdit->text().trimmed().isEmpty())
                return;
            m_codeSubmitted = true;
            m_auth->submitCode(m_codeEdit->text());
            m_codeEdit->clear();
            m_codeEdit->setEnabled(false);
            m_codeBtn->setEnabled(false);
            m_loginText->setText(tr("Checking the code…"));
            m_setupError->clear();
        };
        connect(m_codeBtn, &QPushButton::clicked, this, submit);
        connect(m_codeEdit, &QLineEdit::returnPressed, this, submit);
        codeRow->addWidget(m_codeEdit, 1);
        codeRow->addWidget(m_codeBtn);
        auto *cancel = new QPushButton(tr("Cancel"), m_loginBox);
        cancel->setObjectName(QStringLiteral("agentFooterBtn"));
        connect(cancel, &QPushButton::clicked, this, [this] { m_auth->cancelLogin(); });
        l->addWidget(m_loginText);
        l->addLayout(links);
        l->addLayout(codeRow);
        l->addWidget(cancel, 0, Qt::AlignLeft);
    }

    m_retryBtn = new QPushButton(tr("Check again"), col);
    m_retryBtn->setObjectName(QStringLiteral("agentFooterBtn"));
    connect(m_retryBtn, &QPushButton::clicked, this, [this] {
        showSetup(Setup::Checking);
        m_auth->refresh();
    });

    m_setupCol->addWidget(m_setupTitle);
    m_setupCol->addWidget(m_setupText);
    m_setupCol->addWidget(m_installRow);
    m_setupCol->addWidget(m_signInBox);
    m_setupCol->addWidget(m_loginBox);
    m_setupCol->addWidget(m_setupError);
    m_setupCol->addWidget(m_retryBtn, 0, Qt::AlignLeft);

    auto *outer = new QVBoxLayout(m_setup);
    outer->addStretch(1);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    row->addWidget(col, 4);
    row->addStretch(1);
    outer->addLayout(row);
    outer->addStretch(2);
    m_pages->addWidget(m_setup);
}

void ClaudeCodePanel::showSetup(Setup state, const QString &message)
{
    m_pages->setCurrentWidget(m_setup);
    for (QToolButton *b : {m_historyBtn, m_newBtn})
        b->setEnabled(false);
    m_installRow->setVisible(state == Setup::NotInstalled);
    m_signInBox->setVisible(state == Setup::SignIn);
    m_loginBox->setVisible(state == Setup::SigningIn);
    m_retryBtn->setVisible(state == Setup::NotInstalled || state == Setup::SignIn);
    m_setupError->setText(message);
    m_setupError->setVisible(!message.isEmpty());
    switch (state) {
    case Setup::Checking:
        m_setupText->setText(tr("Checking Claude Code…"));
        break;
    case Setup::NotInstalled:
        m_setupText->setText(installHint()); // Windows: also says when Git for Windows is missing
        break;
    case Setup::SignIn: {
        m_setupText->setText(tr("<p>Sign in to use Claude Code. The sign-in belongs to Claude Code itself, so <code>claude</code> in a "
                                "terminal and QODE share it. QODE never reads it.</p>"));
        const bool hasKey = !AiEnv::apiKey(AiEnv::Provider::Claude, m_root).isEmpty();
        m_useKeyBtn->setText(hasKey ? tr("QODE's Claude API key\nUse the key QODE already has, pay per use")
                                    : tr("Claude API key…\nEnter an Anthropic API key, pay per use"));
        break;
    }
    case Setup::SigningIn: {
        const bool hasUrl = !m_auth->loginUrl().isEmpty();
        m_loginText->setText(hasUrl ? tr("A sign-in page opened in your browser. Sign in there, then copy the code it shows and paste it here.")
                                    : tr("Starting the sign-in…"));
        m_codeEdit->setEnabled(hasUrl);
        m_codeBtn->setEnabled(hasUrl);
        for (QPushButton *b : m_loginBox->findChildren<QPushButton *>())
            if (b != m_codeBtn && b->text() != tr("Cancel"))
                b->setEnabled(hasUrl);
        if (hasUrl)
            m_codeEdit->setFocus();
        break;
    }
    }
}

bool ClaudeCodePanel::ready() const
{
    const AuthStatus &s = m_auth->status();
    return s.checked && s.installed && s.loggedIn;
}

void ClaudeCodePanel::updatePage()
{
    if (m_auth->loggingIn())
        return;
    const AuthStatus &s = m_auth->status();
    if (!s.checked) {
        showSetup(Setup::Checking);
        return;
    }
    if (!s.installed) {
        showSetup(Setup::NotInstalled);
        return;
    }
    if (!s.loggedIn) {
        m_session->stop();
        showSetup(Setup::SignIn, s.error);
        return;
    }
    const bool wasSetup = m_pages->currentWidget() == m_setup;
    m_pages->setCurrentIndex(1);
    refreshEmptyHint(); // the suggestions need the sign-in
    m_historyBtn->setEnabled(!m_busy && !m_root.isEmpty());
    m_newBtn->setEnabled(!m_busy);
    m_input->setEnabled(!m_root.isEmpty());
    // Warm start: the CLI loads while the user types, and its command list is ready for the / popup.
    if (!m_root.isEmpty() && !m_session->isRunning() && m_sessionId.isEmpty())
        ensureSession();
    if (wasSetup && isVisible())
        m_input->setFocus();
}

void ClaudeCodePanel::startLogin(bool console)
{
    setAuthMode(AuthMode::Account);
    m_session->stop();
    m_codeSubmitted = false;
    m_auth->setProjectRoot(m_root);
    m_auth->login(console);
    showSetup(Setup::SigningIn);
}

void ClaudeCodePanel::rebuildAccountMenu()
{
    auto *menu = m_accountBtn->menu();
    menu->clear();
    const AuthStatus &s = m_auth->status();
    const bool apiKeyMode = authMode() == AuthMode::ApiKey;
    if (s.loggedIn) {
        QString who;
        if (s.method == QLatin1String("api_key"))
            who = apiKeyMode ? tr("Using QODE's Claude API key") : tr("Using the ANTHROPIC_API_KEY of the environment");
        else
            who = s.email.isEmpty() ? tr("Signed in") : tr("Signed in as %1").arg(s.email);
        if (!s.plan.isEmpty())
            who += QStringLiteral(" · ") + s.plan.at(0).toUpper() + s.plan.mid(1);
        menu->addAction(who)->setEnabled(false);
        if (!s.org.isEmpty() && s.method != QLatin1String("api_key"))
            menu->addAction(s.org)->setEnabled(false);
        menu->addSeparator();
    }
    if (apiKeyMode) {
        connect(menu->addAction(tr("Use my Claude account instead")), &QAction::triggered, this, [this] {
            setAuthMode(AuthMode::Account);
            m_session->stop();
            showSetup(Setup::Checking);
            m_auth->refresh();
        });
    } else {
        if (s.loggedIn && s.method != QLatin1String("api_key")) {
            connect(menu->addAction(tr("Sign in with another account…")), &QAction::triggered, this, [this] { startLogin(false); });
            connect(menu->addAction(tr("Sign out")), &QAction::triggered, this, [this] {
                if (QMessageBox::question(this, kName, tr("Sign out of Claude Code? This also signs out claude in the terminal.")) != QMessageBox::Yes)
                    return;
                m_session->stop();
                showSetup(Setup::Checking);
                m_auth->logout();
            });
        }
        if (!AiEnv::apiKey(AiEnv::Provider::Claude, m_root).isEmpty())
            connect(menu->addAction(tr("Use QODE's Claude API key instead")), &QAction::triggered, this, [this] {
                setAuthMode(AuthMode::ApiKey);
                m_session->stop();
                showSetup(Setup::Checking);
                m_auth->refresh();
            });
    }
    connect(menu->addAction(tr("Claude API key…")), &QAction::triggered, this, [this] {
        AiKeyDialog dlg(AiEnv::Provider::Claude, this);
        dlg.exec();
    });
    menu->addSeparator();
    QAction *notify = menu->addAction(tr("Notify when Claude Code finishes or needs you"));
    notify->setCheckable(true);
    notify->setChecked(SettingsManager::instance().claudeCode(QStringLiteral("notify")) != QLatin1String("off"));
    connect(notify, &QAction::toggled, this, [](bool on) {
        SettingsManager::instance().setClaudeCode(QStringLiteral("notify"), on ? QStringLiteral("on") : QStringLiteral("off"));
    });
    connect(menu->addAction(tr("Check sign-in again")), &QAction::triggered, this, [this] {
        showSetup(Setup::Checking);
        m_auth->refresh();
    });
}

// --- chat page ------------------------------------------------------------------------------------------------------

void ClaudeCodePanel::buildChatPage()
{
    auto *page = new QWidget;
    m_scroll = new QScrollArea(page);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list = new QWidget;
    m_list->setObjectName(QStringLiteral("aiList"));
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(10, 8, 10, 8);
    m_listLayout->setSpacing(10);
    m_listLayout->addStretch(1);
    m_scroll->setWidget(m_list);
    // On a wide window the transcript is one readable column in the middle, of a fixed width: it is not reflowed while
    // a splitter is dragged (see fitColumn).
    m_list->setMaximumWidth(kColumn + 20);
    m_scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    m_scroll->setObjectName(QStringLiteral("ccScroll")); // its background fills the sides of the column
    m_scroll->viewport()->installEventFilter(this);
    QScrollBar *bar = m_scroll->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int v) { m_stick = v >= bar->maximum() - 24; });
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar] {
        if (m_stick)
            bar->setValue(bar->maximum());
    });

    m_statusLabel = new QLabel(page);
    m_statusLabel->setObjectName(QStringLiteral("aiWho"));
    agent::useTabularDigits(m_statusLabel); // the running clock does not jitter
    m_input = new AiInput(page);
    m_send = new QToolButton(page);
    m_send->setObjectName(QStringLiteral("aiSend"));
    m_send->setCursor(Qt::PointingHandCursor);
    m_send->setFixedSize(28, 28); // a circle at any zoom with the 13.5px radius (see AgentPanel)
    m_inputBox = new QFrame(page);
    m_inputBox->setObjectName(QStringLiteral("aiInputBox"));
    m_attach = new AttachmentBar(m_inputBox);
    m_quote = new AiQuote(m_inputBox, true);
    m_quote->hide();
    connect(m_quote, &AiQuote::removed, this, [this] {
        setQuote(QString());
        m_input->setFocus();
    });
    auto *ibCol = new QVBoxLayout(m_inputBox);
    ibCol->setContentsMargins(8, 4, 6, 4);
    ibCol->setSpacing(2);
    auto *ib = new QHBoxLayout;
    ib->setSpacing(6);
    ib->addWidget(m_input, 1);
    ib->addWidget(m_send, 0, Qt::AlignVCenter);
    ibCol->addWidget(m_quote);
    ibCol->addWidget(m_attach);
    ibCol->addLayout(ib);
    m_input->installEventFilter(this);
    m_input->setAcceptsImages(true);
    connect(m_input, &AiInput::imagesPasted, this, [this](const QList<QImage> &images) {
        for (const QImage &img : images)
            if (!m_attach->addImage(ImageAttachment::fromImage(img))) {
                m_statusLabel->setText(tr("A message takes up to %1 images.").arg(AttachmentBar::kMaxImages));
                break;
            }
    });
    connect(m_attach, &AttachmentBar::changed, this, &ClaudeCodePanel::updateSendButton);
    connect(m_attach, &AttachmentBar::openRequested, this, &ClaudeCodePanel::openFileRequested);
    connect(m_attach, &AttachmentBar::selectionDismissed, this, [this] {
        m_selectionSkip = editorSelection().key(); // left out until the editor selects something else
    });
    m_tasks = new TaskList(page);
    m_queueList = new QueueList(page);
    connect(m_queueList, &QueueList::removeRequested, this, [this](int i) {
        if (i >= 0 && i < m_queue.size())
            m_queue.removeAt(i);
        updateQueue();
    });
    connect(m_queueList, &QueueList::sendNowRequested, this, [this](int i) {
        if (i < 0 || i >= m_queue.size())
            return;
        m_queue.move(i, 0);
        if (m_busy) {
            m_sendNow = true;
            stopTurn();
        } else {
            sendNext();
        }
    });

    // Model and permission mode, under the input as in the terminal.
    auto picker = [page](const QString &tip) {
        auto *b = new QToolButton(page);
        b->setObjectName(QStringLiteral("ccPicker"));
        b->setToolTip(tip);
        b->setPopupMode(QToolButton::InstantPopup);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        b->setCursor(Qt::PointingHandCursor);
        b->setMenu(new QMenu(b));
        return b;
    };
    m_modelBtn = picker(tr("Model"));
    m_modeBtn = picker(tr("When Claude Code asks before acting"));
    for (const Choice &c : kModels) {
        const QString id = QString::fromLatin1(c.id);
        QAction *a = m_modelBtn->menu()->addAction(QStringLiteral("%1  —  %2").arg(QString::fromLatin1(c.label), tr(c.hint)));
        connect(a, &QAction::triggered, this, [this, id] {
            SettingsManager::instance().setClaudeCode(QStringLiteral("model"), id);
            if (m_session->isRunning())
                m_session->setModel(id);
            if (!id.isEmpty())
                m_model.clear(); // the next init / message names the resolved model
            m_contextWindow = 0; // the next result reports the new model's
            updatePickers();
            updateMeter();
        });
    }
    for (const Choice &c : kModes) {
        const QString id = QString::fromLatin1(c.id);
        QAction *a = m_modeBtn->menu()->addAction(QStringLiteral("%1  —  %2").arg(QString::fromLatin1(c.label), tr(c.hint)));
        connect(a, &QAction::triggered, this, [this, id] {
            if (id == QLatin1String("bypassPermissions") &&
                QMessageBox::warning(this, kName, tr("In this mode Claude Code edits files and runs any command without asking. Use it only in a project you trust and can restore."),
                                     QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
                return;
            SettingsManager::instance().setClaudeCode(QStringLiteral("permissionMode"), id);
            if (m_session->isRunning())
                m_session->setPermissionMode(id);
            updatePickers();
        });
    }
    m_meter = new ContextMeter(page);
    connect(m_meter, &QToolButton::clicked, this, &ClaudeCodePanel::compactConversation);
    m_usage = new QLabel(page);
    m_usage->setObjectName(QStringLiteral("aiWho"));
    auto *pickers = new QHBoxLayout;
    pickers->setContentsMargins(0, 0, 0, 0);
    pickers->setSpacing(6);
    pickers->addWidget(m_modelBtn);
    pickers->addWidget(m_modeBtn);
    pickers->addWidget(m_meter);
    pickers->addStretch(1);
    pickers->addWidget(m_usage);

    m_bottom = new QVBoxLayout;
    m_bottom->setContentsMargins(10, 4, 10, 10);
    m_bottom->setSpacing(4);
    m_bottom->addWidget(m_tasks);
    m_bottom->addWidget(m_queueList);
    m_bottom->addWidget(m_statusLabel);
    m_bottom->addWidget(m_inputBox);
    m_bottom->addLayout(pickers);

    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(m_scroll, 1);
    lay->addLayout(m_bottom);
    m_pages->addWidget(page);

    m_popup = new AiMentionPopup(this);
    m_popup->hide();

    connect(m_send, &QToolButton::clicked, this, [this] { m_busy && !hasDraft() ? stopTurn() : send(); });
    connect(m_input, &AiInput::textChanged, this, &ClaudeCodePanel::updateSendButton);
    connect(m_input, &AiInput::sendRequested, this, &ClaudeCodePanel::send);
    connect(m_input, &AiInput::mentionQuery, this, &ClaudeCodePanel::updateMentions);
    connect(m_input, &AiInput::mentionClosed, this, &ClaudeCodePanel::closeMentions);
    connect(m_input, &AiInput::textChanged, this, &ClaudeCodePanel::updateSlash);
    connect(m_input, &AiInput::historyRequested, this, &ClaudeCodePanel::recallPrompt);
    connect(m_input, &AiInput::mentionKey, this, [this](int key) {
        if (key == Qt::Key_Up) {
            m_popup->step(-1);
        } else if (key == Qt::Key_Down) {
            m_popup->step(1);
        } else if (key == Qt::Key_Escape || !m_popup->count()) {
            closeMentions();
        } else if (m_slash) {
            const QString cmd = m_popup->current().section(QLatin1Char(' '), 0, 0);
            const bool exact = m_input->toPlainText().trimmed() == cmd;
            closeMentions();
            if (exact && key != Qt::Key_Tab) {
                send();
            } else {
                m_input->setPlainText(cmd + QLatin1Char(' '));
                m_input->moveCursor(QTextCursor::End);
            }
        } else {
            m_input->completeMention(m_popup->current());
            closeMentions();
        }
    });
    connect(m_popup, &AiMentionPopup::chosen, this, [this](const QString &item) {
        if (m_slash) {
            closeMentions();
            m_input->setPlainText(item.section(QLatin1Char(' '), 0, 0) + QLatin1Char(' '));
            m_input->moveCursor(QTextCursor::End);
        } else {
            m_input->completeMention(item);
            closeMentions();
        }
        m_input->setFocus();
    });
    updatePickers();
    updateMeter();
}

void ClaudeCodePanel::setProject(const QString &root)
{
    if (root == m_root)
        return;
    m_root = root;
    m_auth->setProjectRoot(root);
    m_session->stop();
    m_sessionId.clear();
    m_files.clear();
    if (m_busy)
        finishTurn();
    m_queue.clear();
    m_queueHeld = m_sendNow = false;
    updateQueue();
    m_attach->clearImages();
    m_attach->detachFile();
    m_selectionTimer->start(); // the chips of this project's files
    m_turnChanges.clear();
    m_recall = history::typedPrompts(root);
    m_contextUsed = -1;
    m_compacted = false;
    updateMeter();
    clearTranscript();
    showEmptyHint();
    if (ready())
        updatePage();
}

void ClaudeCodePanel::setProjectFiles(const QStringList &absolutePaths)
{
    m_files.clear();
    const QString prefix = m_root + QLatin1Char('/');
    for (const QString &p : absolutePaths)
        if (!m_root.isEmpty() && p.startsWith(prefix) && !AiEnv::isSensitive(p))
            m_files << p.mid(prefix.size());
}

void ClaudeCodePanel::setEditors(EditorManager *editors)
{
    m_editors = editors;
    connect(editors, &EditorManager::cursorInfoChanged, m_selectionTimer, qOverload<>(&QTimer::start));
    connect(editors, &EditorManager::currentChanged, m_selectionTimer, qOverload<>(&QTimer::start));
}

// The lines selected in the active editor (whole lines are reported; a selection that ends at the start of a line
// does not include that line). Secret files are never sent.
Selection ClaudeCodePanel::editorSelection() const
{
    Selection s;
    CodeEditor *ed = m_editors ? m_editors->currentEditor() : nullptr;
    Document *doc = m_editors ? m_editors->currentDocument() : nullptr;
    if (!ed || !doc || doc->filePath().isEmpty() || AiEnv::isSensitive(doc->filePath()))
        return s;
    const QTextCursor c = ed->textCursor();
    if (!c.hasSelection() || c.selectedText().trimmed().isEmpty())
        return s;
    const QTextBlock first = ed->document()->findBlock(c.selectionStart());
    const QTextBlock last = ed->document()->findBlock(c.selectionEnd());
    int lastLine = last.blockNumber();
    if (c.selectionEnd() == last.position() && last != first)
        --lastLine;
    s.path = doc->filePath();
    s.first = first.blockNumber() + 1;
    s.last = lastLine + 1;
    s.text = c.selectedText().replace(QChar::ParagraphSeparator, QLatin1Char('\n')).replace(QChar::LineSeparator, QLatin1Char('\n')).left(60000);
    return s;
}

// Never a secret file, a file outside the project or an untitled buffer.
QString ClaudeCodePanel::editorFile() const
{
    Document *doc = m_editors ? m_editors->currentDocument() : nullptr;
    if (!doc || doc->isUntitled() || m_root.isEmpty())
        return {};
    const QString p = doc->filePath();
    return Platform::isUnder(p, m_root) && !AiEnv::isSensitive(p) ? p : QString();
}

void ClaudeCodePanel::updateSelection()
{
    m_attach->setCurrentFile(editorFile());
    Selection s = editorSelection();
    if (s.isEmpty())
        m_selectionSkip.clear();
    else if (s.key() == m_selectionSkip)
        s = Selection();
    m_attach->setSelection(s);
}

void ClaudeCodePanel::focusInput()
{
    if (m_pages->currentIndex() == 1)
        m_input->setFocus();
}

void ClaudeCodePanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Checked when first shown, and again while signed out (the user may have signed in from a terminal meanwhile).
    if ((!m_checkedOnce || !ready()) && !m_auth->loggingIn() && !m_auth->refreshing()) {
        m_checkedOnce = true;
        m_auth->setProjectRoot(m_root);
        m_auth->refresh();
    }
}

// --- conversation ---------------------------------------------------------------------------------------------------

void ClaudeCodePanel::ensureSession()
{
    if (m_session->isRunning() || m_root.isEmpty())
        return;
    Session::Options o;
    o.root = m_root;
    o.model = SettingsManager::instance().claudeCode(QStringLiteral("model"));
    o.permissionMode = SettingsManager::instance().claudeCode(QStringLiteral("permissionMode"));
    if (o.permissionMode.isEmpty())
        o.permissionMode = QStringLiteral("default");
    o.resume = m_sessionId;
    m_costBefore = m_cost;
    m_session->start(o);
}

void ClaudeCodePanel::newConversation()
{
    if (m_busy)
        return;
    m_session->stop();
    m_sessionId.clear();
    m_cost = 0;
    m_contextUsed = -1;
    m_compacted = false;
    updateMeter();
    clearTranscript();
    showEmptyHint();
    updateStats();
    if (ready())
        ensureSession();
    focusInput();
}

void ClaudeCodePanel::rebuildHistoryMenu()
{
    QMenu *menu = m_historyBtn->menu();
    menu->clear();
    connect(menu->addAction(Icons::tinted(QStringLiteral(":/new-icons/plus.svg"), Theme::byName(SettingsManager::instance().theme()).textMuted),
                            tr("New conversation")),
            &QAction::triggered, this, &ClaudeCodePanel::newConversation);
    menu->addSeparator();
    const QVector<history::Info> list = history::list(m_root);
    if (list.isEmpty())
        menu->addAction(tr("No earlier conversations in this project"))->setEnabled(false);
    for (const history::Info &i : list) {
        QString title = i.title.simplified();
        if (title.size() > 70)
            title = title.left(70) + QStringLiteral("…");
        QAction *a = menu->addAction(QStringLiteral("%1   ·   %2").arg(title, when(i.updated)));
        a->setCheckable(true);
        a->setChecked(i.id == m_sessionId);
        connect(a, &QAction::triggered, this, [this, id = i.id] { openConversation(id); });
    }
}

void ClaudeCodePanel::openConversation(const QString &id)
{
    if (m_busy)
        return;
    m_session->stop();
    clearTranscript();
    m_cost = 0;
    m_turnChanges.clear();
    const history::Conversation conv = history::load(m_root, id);
    const QVector<QJsonObject> &entries = conv.entries;
    // A long conversation shows its last 40 prompts; Claude Code still has all of it.
    int start = 0, prompts = 0;
    for (int i = entries.size() - 1; i >= 0; --i)
        if (entries.at(i).value(QStringLiteral("content")).isString() && ++prompts > 40) {
            start = i + 1;
            break;
        }
    if (start > 0)
        addNote(tr("Earlier messages of this conversation are not shown."));
    agent::AgentTurnView *view = nullptr;
    for (int i = start; i < entries.size(); ++i) {
        const QJsonObject &e = entries.at(i);
        const QJsonValue content = e.value(QStringLiteral("content"));
        if (str(e, "role") == QLatin1String("user") && content.isString()) {
            notePrompt(content.toString());
            if (view)
                view->endLive();
            view = nullptr;
            flushChanges();
            QVector<ImageAttachment> images;
            for (const QJsonValue &v : e.value(QStringLiteral("images")).toArray()) {
                const QJsonObject src = v.toObject();
                if (str(src, "type") == QLatin1String("base64"))
                    images << ImageAttachment::fromBase64(str(src, "media_type"), str(src, "data").toLatin1());
            }
            QStringList context;
            for (const QJsonValue &v : e.value(QStringLiteral("files")).toArray())
                context << tr("+ file %1").arg(v.toString());
            for (const QJsonValue &v : e.value(QStringLiteral("selections")).toArray())
                if (const QString label = Selection::labelOf(v.toString()); !label.isEmpty())
                    context << tr("+ selection %1").arg(label);
            addWidget(userBubble(content.toString(), images, context, str(e, "quote")));
        } else if (str(e, "role") == QLatin1String("assistant")) {
            if (!view) {
                m_model = str(e, "model");
                view = newTurnView();
                addWidget(view);
            }
            addAssistantContent(view, content.toArray(), false, false);
        } else if (view) {
            addToolResults(view, content.toArray(), e.value(QStringLiteral("toolUseResult")).toObject());
        }
    }
    if (view)
        view->endLive();
    flushChanges();
    m_toolRows.clear();
    m_toolInputs.clear();
    m_toolViews.clear();
    m_sessionId = id;
    m_contextUsed = conv.contextTokens; // right before anything is sent
    m_compacted = false;
    updateMeter();
    if (entries.isEmpty())
        addNote(tr("This conversation could not be read."), true);
    addNote(tr("Claude Code continues this conversation with your next message."));
    m_stick = true;
    ensureSession();
    focusInput();
}

bool ClaudeCodePanel::runLocalSlash(const QString &text)
{
    const QString cmd = text.section(QLatin1Char(' '), 0, 0).toLower();
    if (cmd == QLatin1String("/clear") || cmd == QLatin1String("/new")) {
        newConversation();
        return true;
    }
    if (cmd == QLatin1String("/login")) {
        startLogin(false);
        return true;
    }
    if (cmd == QLatin1String("/logout")) {
        m_session->stop();
        showSetup(Setup::Checking);
        m_auth->logout();
        return true;
    }
    if (cmd == QLatin1String("/resume")) {
        m_historyBtn->showMenu();
        return true;
    }
    return false;
}

bool ClaudeCodePanel::hasDraft() const
{
    return !m_input->toPlainText().trimmed().isEmpty() || m_attach->hasImages();
}

void ClaudeCodePanel::send()
{
    if (m_root.isEmpty() || !ready())
        return;
    Prompt p;
    p.text = m_input->toPlainText().trimmed();
    // A command goes as typed: the CLI only runs it from plain text. The attachments wait for the next message.
    const bool command = p.text.startsWith(QLatin1Char('/'));
    if (!command) {
        p.images = m_attach->images();
        p.selection = m_attach->selection();
        p.file = relative(m_attach->attachedFile());
        p.quote = m_quote->isHidden() ? QString() : m_quote->text();
    }
    if (p.text.isEmpty() && p.images.isEmpty())
        return;
    closeMentions();
    m_input->clear();
    if (!command) {
        m_attach->clearImages();
        m_attach->detachFile(); // a dimmed chip of the then-current file again
        setQuote(QString());
        if (!p.selection.isEmpty()) { // sent once; selecting other lines attaches those
            m_selectionSkip = p.selection.key();
            m_attach->setSelection(Selection());
        }
    }
    if (command && showInfo(p.text))
        return;
    if (m_busy) {
        m_queue.append(p);
        updateQueue();
        return;
    }
    dispatch(p);
}

bool ClaudeCodePanel::showInfo(const QString &text)
{
    const QString cmd = text.simplified().toLower();
    if (cmd != QLatin1String("/usage") && cmd != QLatin1String("/mcp"))
        return false;
    notePrompt(text);
    if (m_hint) {
        m_hint->deleteLater();
        m_hint = nullptr;
    }
    addWidget(userBubble(text));
    if (cmd == QLatin1String("/usage")) {
        auto *card = new UsageCard;
        connect(card, &InfoCard::refreshRequested, this, [this, card] { loadUsage(card); });
        addWidget(card);
        loadUsage(card);
    } else {
        auto *card = new McpCard;
        connect(card, &InfoCard::refreshRequested, this, [this, card] { loadMcp(card); });
        connect(card, &McpCard::reconnectRequested, this, [this, card](const QString &name) {
            card->setMessage(tr("Connecting to %1…").arg(name));
            QPointer<McpCard> guard(card);
            query(QJsonObject{{"subtype", "mcp_reconnect"}, {"serverName", name}}, [this, guard, name](const QJsonObject &, const QString &error) {
                if (!guard)
                    return;
                guard->setMessage(error.isEmpty() ? QString() : tr("%1: %2").arg(name, error), true);
                loadMcp(guard);
            });
        });
        connect(card, &McpCard::toggleRequested, this, [this, card](const QString &name, bool enabled) {
            QPointer<McpCard> guard(card);
            query(QJsonObject{{"subtype", "mcp_toggle"}, {"serverName", name}, {"enabled", enabled}},
                  [this, guard, name](const QJsonObject &, const QString &error) {
                      if (!guard)
                          return;
                      guard->setMessage(error.isEmpty() ? QString() : tr("%1: %2").arg(name, error), true);
                      loadMcp(guard);
                  });
        });
        addWidget(card);
        loadMcp(card);
    }
    m_stick = true;
    return true;
}

void ClaudeCodePanel::loadUsage(UsageCard *card)
{
    card->setLoading(tr("Reading usage…"));
    QPointer<UsageCard> guard(card);
    query(QJsonObject{{"subtype", "get_usage"}}, [this, guard](const QJsonObject &usage, const QString &error) {
        if (!guard)
            return;
        if (!error.isEmpty())
            guard->setError(tr("Claude Code could not report the usage: %1").arg(error));
        else
            guard->setUsage(m_auth->status(), usage, m_cost);
    });
}

void ClaudeCodePanel::loadMcp(McpCard *card)
{
    QPointer<McpCard> guard(card);
    query(QJsonObject{{"subtype", "mcp_status"}}, [guard](const QJsonObject &status, const QString &error) {
        if (!guard)
            return;
        if (!error.isEmpty())
            guard->setError(tr("Claude Code could not list the MCP servers: %1").arg(error));
        else
            guard->setServers(status.value(QStringLiteral("mcpServers")).toArray());
    });
}

// A control request to the running CLI (started when needed). Answers that never come (the process ended) fail after
// a minute; the usage scan reads a week of transcripts, so it may take a few seconds.
void ClaudeCodePanel::query(const QJsonObject &request, std::function<void(const QJsonObject &, const QString &)> done)
{
    ensureSession();
    const QString id = m_session->query(request);
    m_queries.insert(id, std::move(done));
    QTimer::singleShot(60000, this, [this, id] {
        if (auto done = m_queries.take(id))
            done({}, tr("Claude Code did not answer."));
    });
}

void ClaudeCodePanel::askAbout(const QString &text)
{
    if (m_root.isEmpty() || !ready())
        return;
    Prompt p;
    p.text = AiQuote::askPrompt();
    p.quote = text;
    if (m_busy) {
        m_queue.append(p);
        updateQueue();
        return;
    }
    dispatch(p);
}

void ClaudeCodePanel::setQuote(const QString &text)
{
    m_quote->setText(text);
    m_quote->setVisible(!text.isEmpty());
    if (!text.isEmpty())
        m_input->setFocus();
}

void ClaudeCodePanel::sendNext()
{
    if (m_queue.isEmpty() || m_busy || m_root.isEmpty() || !ready())
        return;
    const Prompt p = m_queue.takeFirst();
    m_queueHeld = false;
    updateQueue();
    dispatch(p);
}

void ClaudeCodePanel::afterTurn(bool ok)
{
    const bool go = ok || m_sendNow;
    m_sendNow = false;
    // Stop pressed: no notification. Queued messages that follow on notify once, after the last.
    if (!m_stopping && !(go && !m_queue.isEmpty()))
        notifyTurnEnd(ok);
    if (m_queue.isEmpty())
        return;
    m_queueHeld = !go;
    updateQueue();
    if (go)
        QTimer::singleShot(0, this, &ClaudeCodePanel::sendNext);
}

bool ClaudeCodePanel::notifyWanted() const
{
    return SettingsManager::instance().claudeCode(QStringLiteral("notify")) != QLatin1String("off") && (!window()->isActiveWindow() || !isVisible());
}

void ClaudeCodePanel::notifyTurnEnd(bool ok)
{
    closeNotifications(false); // nothing waits any more
    if (!notifyWanted())
        return;
    QApplication::alert(window());
    const QString body = ok ? (m_turnReply.isEmpty() ? tr("Done.") : m_turnReply) : (m_turnError.isEmpty() ? tr("Stopped") : firstLine(m_turnError));
    DesktopNotify::instance().notify(tr("Claude Code finished"), body, QStringLiteral("claudecode-%1-finished").arg(quintptr(this), 0, 16));
}

void ClaudeCodePanel::notifyWaiting(const QJsonObject &request)
{
    if (!notifyWanted())
        return;
    // The tool and a short summary: "Bash: npm test".
    const QString tool = str(request, "tool_name");
    const QJsonObject in = request.value(QStringLiteral("input")).toObject();
    QString what;
    if (tool == QLatin1String("Bash"))
        what = QStringLiteral("Bash: %1").arg(firstLine(str(in, "command"), 100));
    else if (tool == QLatin1String("AskUserQuestion"))
        what = tr("Question: %1").arg(firstLine(str(in.value(QStringLiteral("questions")).toArray().first().toObject(), "question"), 100));
    else if (tool == QLatin1String("ExitPlanMode"))
        what = tr("A plan is ready for review");
    else if (editsFiles(tool) || tool == QLatin1String("Read"))
        what = QStringLiteral("%1: %2").arg(tool, relative(str(in, "file_path").isEmpty() ? str(in, "notebook_path") : str(in, "file_path")));
    else
        what = firstLine(describeTool(tool, in).remove(QStringLiteral("**")).remove(QLatin1Char('`')), 100);
    DesktopNotify::instance().notify(tr("Claude Code is waiting for you"), what, QStringLiteral("claudecode-%1-waiting").arg(quintptr(this), 0, 16));
}

void ClaudeCodePanel::closeNotifications(bool all)
{
    const QString prefix = QStringLiteral("claudecode-%1-").arg(quintptr(this), 0, 16);
    DesktopNotify::instance().close(prefix + QStringLiteral("waiting"));
    if (all)
        DesktopNotify::instance().close(prefix + QStringLiteral("finished"));
}

void ClaudeCodePanel::updateQueue()
{
    QStringList items;
    for (const Prompt &p : std::as_const(m_queue))
        items << p.summary();
    m_queueList->setItems(items, m_queueHeld);
}

void ClaudeCodePanel::dispatch(const Prompt &p)
{
    notePrompt(p.text);
    if (p.images.isEmpty() && p.selection.isEmpty() && p.file.isEmpty() && p.quote.isEmpty() && runLocalSlash(p.text))
        return;
    m_queueHeld = false; // a turn runs again: what is queued goes out after it
    if (m_hint) {
        m_hint->deleteLater();
        m_hint = nullptr;
    }
    QStringList context;
    if (!p.file.isEmpty())
        context << tr("+ file %1").arg(p.file);
    if (!p.selection.isEmpty())
        context << tr("+ selection %1").arg(p.selection.label());
    addWidget(userBubble(p.text, p.images, context, p.quote));
    m_turnChanges.clear();
    m_live = newTurnView();
    addWidget(m_live);
    m_done.clear();
    m_cur = agent::RequestStats();
    m_requesting = false;
    m_gotText = false;
    m_stopping = false;
    m_turnReply.clear();
    m_turnError.clear();
    m_streamed.clear();
    m_turnClock.start();
    m_stick = true;
    ensureSession();
    m_session->sendUser(p.content());
    setBusy(true);
}

void ClaudeCodePanel::stopTurn()
{
    if (!m_busy || m_stopping)
        return;
    m_stopping = true;
    cancelPermissions(tr("Stopped"));
    m_session->interrupt();
    m_statusLabel->setText(tr("Stopping…"));
    m_stopTimer->start();
}

void ClaudeCodePanel::finishTurn()
{
    m_stopTimer->stop();
    if (m_live) {
        if (m_requesting) {
            m_cur.ms = m_reqClock.elapsed();
            m_cur.done = true;
            m_cur.stopped = true;
            m_done.append(m_cur);
        }
        m_live->endLive();
        m_live->setLiveStats(m_done, agent::RequestStats(), false, m_turnClock.elapsed());
        m_turnReply = firstLine(m_live->replyText());
        m_live = nullptr;
        flushChanges();
    }
    m_tasks->clear();
    m_requesting = false;
    m_thinking = false;
    cancelPermissions(tr("Cancelled"));
    m_toolRows.clear();
    m_toolInputs.clear();
    m_toolViews.clear();
    setBusy(false);
}

void ClaudeCodePanel::setBusy(bool busy)
{
    m_busy = busy;
    const bool chat = m_pages->currentIndex() == 1;
    m_historyBtn->setEnabled(chat && !busy && !m_root.isEmpty());
    m_newBtn->setEnabled(chat && !busy);
    m_input->setPlaceholderText(busy ? tr("Write the next message: it is sent when Claude Code is done")
                                     : tr("Ask Claude Code…  @ points at a file, / runs a command, Ctrl+V pastes a screenshot"));
    if (busy) {
        m_statsTimer->start();
    } else {
        m_statsTimer->stop();
        m_statusLabel->clear();
    }
    applyTheme();
    updateStats();
    updateQueue();
}

// --- the stream -----------------------------------------------------------------------------------------------------

void ClaudeCodePanel::onMessage(const QJsonObject &o)
{
    const QString type = str(o, "type");
    const bool subagent = o.value(QStringLiteral("parent_tool_use_id")).isString();
    if (type == QLatin1String("system")) {
        const QString sub = str(o, "subtype");
        if (sub == QLatin1String("init")) {
            if (str(o, "model") != m_model && !m_model.isEmpty())
                m_contextWindow = 0;
            m_model = str(o, "model");
            m_sessionId = str(o, "session_id");
            if (m_live)
                m_live->setModel(prettyModel(m_model));
            updatePickers();
            updateMeter();
        } else if (sub == QLatin1String("status") && str(o, "status") == QLatin1String("requesting") && m_live && !subagent && !m_requesting) {
            m_requesting = true;
            m_cur = agent::RequestStats();
            m_cur.done = false;
            m_cur.exact = false;
            m_thinking = false;
            m_streamChars = m_thinkChars = 0;
            m_reqClock.start();
        } else if (sub == QLatin1String("compact_boundary") && !subagent) {
            const int before = o.value(QStringLiteral("compact_metadata")).toObject().value(QStringLiteral("pre_tokens")).toInt();
            addNote(before > 0 ? tr("The conversation was compacted from %1k tokens: Claude Code continues from a summary.").arg(qRound(before / 1000.0))
                               : tr("The conversation was compacted: Claude Code continues from a summary."));
            m_contextUsed = -1; // known again after the next reply
            m_compacted = true;
            updateMeter();
        }
        return;
    }
    if (type == QLatin1String("stream_event")) {
        if (!subagent)
            onStreamEvent(o.value(QStringLiteral("event")).toObject());
        return;
    }
    if (type == QLatin1String("assistant")) {
        const QJsonObject msg = o.value(QStringLiteral("message")).toObject();
        if (!m_live)
            return;
        if (!subagent && !str(msg, "model").isEmpty() && str(msg, "model") != m_model) {
            m_model = str(msg, "model");
            m_live->setModel(prettyModel(m_model));
            updatePickers();
        }
        addAssistantContent(m_live, msg.value(QStringLiteral("content")).toArray(), m_streamed.contains(str(msg, "id")), subagent);
        return;
    }
    if (type == QLatin1String("user")) {
        const QJsonObject msg = o.value(QStringLiteral("message")).toObject();
        if (m_live)
            addToolResults(m_live, msg.value(QStringLiteral("content")).toArray(), o.value(QStringLiteral("tool_use_result")).toObject());
        return;
    }
    if (type == QLatin1String("result")) {
        onResult(o);
        return;
    }
    if (type == QLatin1String("rate_limit_event")) {
        const QJsonObject info = o.value(QStringLiteral("rate_limit_info")).toObject();
        const QString status = str(info, "status");
        const QString kind = str(info, "rateLimitType");
        const QString window = kind == QLatin1String("five_hour") ? tr("5-hour") : kind.startsWith(QLatin1String("seven_day")) ? tr("weekly") : kind;
        const QDateTime resets = QDateTime::fromSecsSinceEpoch(qint64(info.value(QStringLiteral("resetsAt")).toDouble()));
        if (status == QLatin1String("allowed_warning"))
            m_limitText = tr("%1% of the %2 limit used").arg(qRound(info.value(QStringLiteral("utilization")).toDouble() * 100)).arg(window);
        else if (status == QLatin1String("rejected")) {
            m_limitText = tr("%1 limit reached").arg(window);
            addNote(tr("The %1 usage limit is reached. It resets %2.").arg(window, QLocale().toString(resets, QLocale::ShortFormat)), true);
        } else
            m_limitText.clear();
        updateStats();
    }
}

void ClaudeCodePanel::onStreamEvent(const QJsonObject &ev)
{
    if (!m_live)
        return;
    const QString type = str(ev, "type");
    const qint64 now = m_requesting ? m_reqClock.elapsed() : 0;
    if (type == QLatin1String("message_start")) {
        if (!m_requesting) {
            m_requesting = true;
            m_cur = agent::RequestStats();
            m_cur.done = false;
            m_cur.exact = false;
            m_streamChars = m_thinkChars = 0;
            m_reqClock.start();
        }
        const QJsonObject msg = ev.value(QStringLiteral("message")).toObject();
        m_curMsgId = str(msg, "id");
        const QJsonObject u = msg.value(QStringLiteral("usage")).toObject();
        const int cached = u.value(QStringLiteral("cache_read_input_tokens")).toInt();
        m_cur.prompt = u.value(QStringLiteral("input_tokens")).toInt() + cached + u.value(QStringLiteral("cache_creation_input_tokens")).toInt();
        m_cur.cached = cached;
    } else if (type == QLatin1String("content_block_start")) {
        const QString kind = str(ev.value(QStringLiteral("content_block")).toObject(), "type");
        if (m_cur.firstMs < 0)
            m_cur.firstMs = now;
        if (kind == QLatin1String("thinking") || kind == QLatin1String("redacted_thinking")) {
            if (!m_thinking) {
                m_thinking = true;
                m_thinkStart = now;
            }
        } else if (m_thinking) {
            m_cur.thinkMs += now - m_thinkStart;
            m_thinking = false;
        }
    } else if (type == QLatin1String("content_block_delta")) {
        const QJsonObject d = ev.value(QStringLiteral("delta")).toObject();
        const QString kind = str(d, "type");
        if (kind == QLatin1String("text_delta")) {
            const QString t = str(d, "text");
            m_live->appendText(t);
            m_gotText = m_gotText || !t.trimmed().isEmpty();
            m_streamChars += t.size();
            m_streamed.insert(m_curMsgId);
        } else if (kind == QLatin1String("thinking_delta")) {
            const QString t = str(d, "thinking");
            m_live->appendThought(t);
            m_thinkChars += t.size();
            m_streamed.insert(m_curMsgId);
        } else if (kind == QLatin1String("input_json_delta")) {
            m_streamChars += str(d, "partial_json").size();
        }
        if (!m_cur.exact) {
            m_cur.output = (m_streamChars + m_thinkChars) / 4;
            m_cur.thinking = m_thinkChars / 4;
        }
    } else if (type == QLatin1String("message_delta")) {
        const QJsonObject u = ev.value(QStringLiteral("usage")).toObject();
        if (u.contains(QStringLiteral("output_tokens"))) {
            m_cur.output = u.value(QStringLiteral("output_tokens")).toInt();
            m_cur.thinking = u.value(QStringLiteral("output_tokens_details")).toObject().value(QStringLiteral("thinking_tokens")).toInt(m_cur.thinking);
            m_cur.exact = true;
        }
    } else if (type == QLatin1String("message_stop")) {
        if (m_thinking) {
            m_cur.thinkMs += now - m_thinkStart;
            m_thinking = false;
        }
        m_cur.ms = now;
        m_cur.done = true;
        m_done.append(m_cur);
        m_requesting = false;
        // Subagents' stream events never get here (parent_tool_use_id), so this is the main thread's context.
        if (m_cur.prompt > 0) {
            m_contextUsed = m_cur.prompt + m_cur.output;
            m_compacted = false;
            updateMeter();
        }
        updateStats();
    }
}

void ClaudeCodePanel::onResult(const QJsonObject &o)
{
    // The CLI reports the total of its process so far; a restarted process (a resumed conversation) starts at 0.
    const double cost = o.value(QStringLiteral("total_cost_usd")).toDouble();
    if (cost > 0)
        m_cost = m_costBefore + cost;
    // The window of the main model ({model id: {contextWindow, maxOutputTokens, …}}; subagents may add other models).
    const QJsonObject usage = o.value(QStringLiteral("modelUsage")).toObject();
    for (auto it = usage.begin(); it != usage.end(); ++it)
        if (it.key() == m_model || m_model.startsWith(it.key()) || usage.size() == 1) {
            if (const int window = it.value().toObject().value(QStringLiteral("contextWindow")).toInt(); window > 0)
                m_contextWindow = window;
            break;
        }
    updateMeter();
    const QString sub = str(o, "subtype");
    const bool error = o.value(QStringLiteral("is_error")).toBool() || (sub != QLatin1String("success") && !sub.isEmpty());
    QString text = str(o, "result");
    if (error) {
        QStringList errors;
        for (const QJsonValue &v : o.value(QStringLiteral("errors")).toArray())
            errors << v.toString();
        if (text.isEmpty())
            text = errors.join(QLatin1Char('\n'));
        m_turnError = text.isEmpty() ? tr("Claude Code reported an error (%1).").arg(sub) : text;
        if (m_stopping)
            addNote(tr("Stopped."));
        else if (sub == QLatin1String("error_max_turns"))
            addNote(tr("Claude Code stopped after reaching its turn limit."), true);
        else
            addNote(text.isEmpty() ? tr("Claude Code reported an error (%1).").arg(sub) : text, true);
        if (authProblem(text))
            m_auth->refresh();
    } else if (m_live && !m_gotText && !text.trimmed().isEmpty()) {
        m_live->appendText(text); // a command's output (/context, /cost, …) comes only as the result
    }
    finishTurn();
    afterTurn(!error);
}

void ClaudeCodePanel::onExited(int code, const QString &error)
{
    for (const auto &done : std::exchange(m_queries, {}))
        done({}, tr("Claude Code stopped."));
    const bool wasBusy = m_busy;
    if (wasBusy || !error.isEmpty()) {
        QString text = error.isEmpty() ? tr("Claude Code stopped unexpectedly (exit code %1).").arg(code) : tr("Claude Code stopped: %1").arg(error);
        if (!wasBusy && error.isEmpty())
            text.clear();
        if (!text.isEmpty())
            addNote(text, true);
        m_turnError = text;
    }
    if (wasBusy) {
        finishTurn();
        afterTurn(false);
    }
    if (authProblem(error))
        m_auth->refresh();
}

// --- content ----------------------------------------------------------------------------------------------------------

void ClaudeCodePanel::addAssistantContent(agent::AgentTurnView *view, const QJsonArray &content, bool textStreamed, bool subagent)
{
    for (const QJsonValue &v : content) {
        const QJsonObject b = v.toObject();
        const QString type = str(b, "type");
        if (type == QLatin1String("text") && !textStreamed && !subagent) {
            const QString t = str(b, "text");
            view->appendText(t);
            m_gotText = m_gotText || !t.trimmed().isEmpty();
        } else if (type == QLatin1String("thinking") && !textStreamed && !subagent && !str(b, "thinking").trimmed().isEmpty()) {
            view->appendThought(str(b, "thinking"));
        } else if (type == QLatin1String("tool_use")) {
            const QString id = str(b, "id"), name = str(b, "name");
            const QJsonObject input = b.value(QStringLiteral("input")).toObject();
            QString call = describeTool(name, input);
            if (subagent)
                call = QStringLiteral("↳ ") + call;
            m_toolRows.insert(id, view->addTool(name, call, inputText(name, input)));
            m_toolViews.insert(id, view);
            m_toolInputs.insert(id, QJsonObject{{"name", name}, {"input", input}});
            if (name == QLatin1String("TodoWrite") && !subagent && view == m_live)
                m_tasks->setTodos(input.value(QStringLiteral("todos")).toArray());
        }
    }
}

void ClaudeCodePanel::addToolResults(agent::AgentTurnView *, const QJsonArray &content, const QJsonObject &structured)
{
    int results = 0;
    for (const QJsonValue &v : content)
        if (str(v.toObject(), "type") == QLatin1String("tool_result"))
            ++results;
    for (const QJsonValue &v : content) {
        const QJsonObject b = v.toObject();
        if (str(b, "type") != QLatin1String("tool_result"))
            continue;
        const QString id = str(b, "tool_use_id");
        if (!m_toolRows.contains(id))
            continue;
        const QJsonObject info = m_toolInputs.value(id);
        const QString name = str(info, "name");
        // The structured result belongs to the message's only result.
        const QJsonObject st = results == 1 ? structured : QJsonObject();
        const agent::ToolResult r = toolResult(name, info.value(QStringLiteral("input")).toObject(), b, st);
        if (agent::AgentTurnView *view = m_toolViews.value(id))
            view->finishToolAt(m_toolRows.value(id), r);
        if (r.hasChange)
            noteChange(name, info.value(QStringLiteral("input")).toObject(), st, r);
        if (m_live && (editsFiles(name) || name == QLatin1String("Bash")))
            scheduleProjectChanged();
    }
}

QString ClaudeCodePanel::describeTool(const QString &name, const QJsonObject &in) const
{
    QString file = str(in, "file_path");
    if (file.isEmpty())
        file = str(in, "notebook_path");
    file = relative(file);
    const QString path = relative(str(in, "path"));
    auto clip = [](QString s, int max) {
        s = s.simplified();
        return s.size() > max ? s.left(max) + QStringLiteral("…") : s;
    };
    if (name == QLatin1String("Bash")) {
        // What the command is for, when Claude Code said so; the command itself is in the folded-out row.
        if (!str(in, "description").trimmed().isEmpty())
            return QStringLiteral("**Bash** %1").arg(clip(str(in, "description"), 140));
        return tr("Run `%1`").arg(clip(str(in, "command").section(QLatin1Char('\n'), 0, 0), 90)); // one row, as in the QODE chat
    }
    if (name == QLatin1String("Read")) {
        const int offset = in.value(QStringLiteral("offset")).toInt(), limit = in.value(QStringLiteral("limit")).toInt();
        return limit > 0 ? tr("Read %1 %2–%3").arg(file).arg(qMax(1, offset)).arg(qMax(1, offset) + limit - 1) : tr("Read %1").arg(file);
    }
    if (name == QLatin1String("Edit") || name == QLatin1String("MultiEdit"))
        return tr("Edit %1").arg(file);
    if (name == QLatin1String("Write"))
        return tr("Write %1").arg(file);
    if (name == QLatin1String("NotebookEdit"))
        return tr("Edit notebook %1").arg(file);
    if (name == QLatin1String("Glob"))
        return path.isEmpty() ? tr("Find files %1").arg(str(in, "pattern")) : tr("Find files %1 in %2").arg(str(in, "pattern"), path);
    if (name == QLatin1String("Grep")) {
        QString s = tr("Search “%1”").arg(clip(str(in, "pattern"), 80));
        if (!path.isEmpty())
            s += tr(" in %1").arg(path);
        if (!str(in, "glob").isEmpty())
            s += QStringLiteral(" (%1)").arg(str(in, "glob"));
        return s;
    }
    if (name == QLatin1String("WebFetch"))
        return tr("Fetch %1").arg(str(in, "url"));
    if (name == QLatin1String("WebSearch"))
        return tr("Search the web for “%1”").arg(clip(str(in, "query"), 100));
    if (name == QLatin1String("Task") || name == QLatin1String("Agent"))
        return tr("Subtask: %1").arg(clip(str(in, "description"), 120));
    if (name == QLatin1String("TodoWrite")) {
        const QJsonArray todos = in.value(QStringLiteral("todos")).toArray();
        int done = 0;
        QString now;
        for (const QJsonValue &t : todos) {
            const QJsonObject o = t.toObject();
            if (str(o, "status") == QLatin1String("completed"))
                ++done;
            else if (str(o, "status") == QLatin1String("in_progress") && now.isEmpty())
                now = str(o, "activeForm").isEmpty() ? str(o, "content") : str(o, "activeForm");
        }
        QString s = tr("To-dos: %1 of %2 done").arg(done).arg(todos.size());
        if (!now.isEmpty())
            s += tr(" · now: %1").arg(clip(now, 100));
        return s;
    }
    if (name == QLatin1String("Skill"))
        return tr("Skill %1").arg(str(in, "skill").isEmpty() ? str(in, "command") : str(in, "skill"));
    if (name == QLatin1String("ExitPlanMode"))
        return tr("Plan ready for review");
    if (name == QLatin1String("AskUserQuestion"))
        return tr("Asked you a question");
    if (name.startsWith(QLatin1String("mcp__"))) {
        const QStringList parts = name.split(QStringLiteral("__"));
        return parts.size() >= 3 ? QStringLiteral("%1 · %2").arg(parts.at(1), parts.mid(2).join(QStringLiteral("__"))) : name;
    }
    const QString args = QString::fromUtf8(QJsonDocument(in).toJson(QJsonDocument::Compact));
    return QStringLiteral("%1 %2").arg(name, clip(args, 120));
}

agent::ToolResult ClaudeCodePanel::toolResult(const QString &name, const QJsonObject &in, const QJsonObject &block, const QJsonObject &structured) const
{
    agent::ToolResult r;
    r.ok = !block.value(QStringLiteral("is_error")).toBool();
    const QString text = resultText(block);
    r.output = text;
    if (!r.ok) {
        r.summary = firstLine(text, 300);
        return r;
    }
    if (editsFiles(name) && name != QLatin1String("NotebookEdit")) {
        AiChange &c = r.change;
        c.path = relative(str(in, "file_path"));
        c.ok = true;
        const QJsonArray hunks = structured.value(QStringLiteral("structuredPatch")).toArray();
        if (name == QLatin1String("Write") && str(structured, "type") != QLatin1String("update")) {
            c.op = QStringLiteral("create");
            c.startLine = 1;
            c.after = str(in, "content");
            c.note = tr("new file, %n line(s)", nullptr, int(c.after.count(QLatin1Char('\n')) + (c.after.endsWith(QLatin1Char('\n')) ? 0 : 1)));
        } else if (!hunks.isEmpty()) {
            // The hunks as the CLI computed them: " " context, "-" removed, "+" added.
            c.op = QStringLiteral("replace");
            c.startLine = hunks.first().toObject().value(QStringLiteral("oldStart")).toInt();
            QStringList before, after;
            int added = 0, removed = 0;
            for (const QJsonValue &h : hunks) {
                if (!before.isEmpty()) {
                    before << QStringLiteral("…");
                    after << QStringLiteral("…");
                }
                for (const QJsonValue &lv : h.toObject().value(QStringLiteral("lines")).toArray()) {
                    const QString l = lv.toString();
                    if (l.startsWith(QLatin1Char('-')) || l.startsWith(QLatin1Char(' ')))
                        before << l.mid(1);
                    if (l.startsWith(QLatin1Char('+')) || l.startsWith(QLatin1Char(' ')))
                        after << l.mid(1);
                    added += l.startsWith(QLatin1Char('+'));
                    removed += l.startsWith(QLatin1Char('-'));
                }
            }
            c.note = tr("+%1 −%2 lines").arg(added).arg(removed);
            c.before = before.join(QLatin1Char('\n'));
            c.after = after.join(QLatin1Char('\n'));
        } else if (name == QLatin1String("Edit")) {
            c.op = QStringLiteral("replace");
            c.before = str(in, "old_string");
            c.after = str(in, "new_string");
        } else {
            c.op = QStringLiteral("replace");
            c.after = str(in, "content");
        }
        c.before = c.before.left(60000);
        c.after = c.after.left(60000);
        r.hasChange = true;
        return r;
    }
    const int lines = lineCount(text);
    if (name == QLatin1String("Bash"))
        r.summary = lines == 0 ? tr("no output") : lines == 1 ? firstLine(text) : tr("%n lines of output", nullptr, lines);
    else if (name == QLatin1String("Read"))
        r.summary = tr("%n line(s)", nullptr, lines);
    else if (name == QLatin1String("TodoWrite"))
        r.summary.clear();
    else
        r.summary = firstLine(text);
    return r;
}

void ClaudeCodePanel::noteChange(const QString &name, const QJsonObject &in, const QJsonObject &structured, const agent::ToolResult &r)
{
    FileChange c;
    c.path = str(in, "file_path");
    if (c.path.isEmpty())
        return;
    c.display = relative(c.path);
    const QJsonArray hunks = structured.value(QStringLiteral("structuredPatch")).toArray();
    if (r.change.op == QLatin1String("create")) {
        c.created = true;
        c.added = textLines(r.change.after);
        c.lines << 1;
    } else if (!hunks.isEmpty()) {
        for (const QJsonValue &hv : hunks) {
            const QJsonObject h = hv.toObject();
            int line = h.value(QStringLiteral("newStart")).toInt(), first = 0;
            for (const QJsonValue &lv : h.value(QStringLiteral("lines")).toArray()) {
                const QString l = lv.toString();
                const bool add = l.startsWith(QLatin1Char('+')), del = l.startsWith(QLatin1Char('-'));
                if ((add || del) && !first)
                    first = line; // the first changed line, past the hunk's leading context
                c.added += add;
                c.removed += del;
                if (!del)
                    ++line;
            }
            c.lines << first;
        }
    } else if (name == QLatin1String("Edit")) {
        const QString neu = str(in, "new_string");
        c.added = textLines(neu);
        c.removed = textLines(str(in, "old_string"));
        // No hunks (an older CLI): find the new text in the file.
        QFile f(c.path);
        if (!neu.isEmpty() && f.open(QIODevice::ReadOnly)) {
            const QString text = TextEncoding::decode(f.readAll()).text;
            const qsizetype at = text.indexOf(neu);
            c.lines << (at < 0 ? 0 : int(text.left(at).count(QLatin1Char('\n'))) + 1);
        }
    }
    addChange(m_turnChanges, c);
}

void ClaudeCodePanel::flushChanges()
{
    if (m_turnChanges.isEmpty())
        return;
    auto *card = new ChangesCard(m_turnChanges);
    connect(card, &ChangesCard::openRequested, this, &ClaudeCodePanel::openFileRequested);
    addWidget(card);
    m_turnChanges.clear();
}

// --- permissions ------------------------------------------------------------------------------------------------------

void ClaudeCodePanel::onPermission(const QString &id, const QJsonObject &request)
{
    if (m_hint) {
        m_hint->deleteLater();
        m_hint = nullptr;
    }
    const QString tool = str(request, "tool_name");
    QWidget *card = tool == QLatin1String("AskUserQuestion") ? questionCard(id, request)
                    : tool == QLatin1String("ExitPlanMode")  ? planCard(id, request)
                                                             : permissionCard(id, request);
    m_permissions.insert(id, card);
    m_stick = true;
    addWidget(card);
    updateStats();
    QApplication::alert(window());
    notifyWaiting(request);
}

namespace {
// The frame of every card that waits for the user: a title, then the body and the button row.
QFrame *cardFrame(const QString &title, QVBoxLayout **body)
{
    auto *box = new QFrame;
    box->setObjectName(QStringLiteral("aiNotice"));
    auto *l = new QVBoxLayout(box);
    l->setContentsMargins(12, 10, 12, 10);
    l->setSpacing(8);
    auto *t = wrapLabel(title, box, "ccCardTitle");
    t->setTextFormat(Qt::PlainText);
    l->addWidget(t);
    *body = l;
    return box;
}

QWidget *buttonRow(QFrame *box, QVBoxLayout *body)
{
    auto *buttons = new QWidget(box);
    buttons->setObjectName(QStringLiteral("ccButtons"));
    auto *bl = new QHBoxLayout(buttons);
    bl->setContentsMargins(0, 2, 0, 0);
    bl->setSpacing(6);
    body->addWidget(buttons);
    return buttons;
}

QPushButton *cardButton(const QString &text, QWidget *row, bool primary = false)
{
    auto *b = new QPushButton(text, row);
    b->setObjectName(primary ? QStringLiteral("ccPrimary") : QStringLiteral("agentFooterBtn"));
    b->setCursor(Qt::PointingHandCursor);
    static_cast<QHBoxLayout *>(row->layout())->addWidget(b);
    return b;
}
} // namespace

QWidget *ClaudeCodePanel::permissionCard(const QString &id, const QJsonObject &req)
{
    const QString tool = str(req, "tool_name");
    const QJsonObject in = req.value(QStringLiteral("input")).toObject();
    const QString file = relative(str(in, "file_path"));
    QString title;
    if (tool == QLatin1String("Bash"))
        title = tr("Claude Code wants to run a command");
    else if (tool == QLatin1String("Write") && !QFileInfo::exists(str(in, "file_path")))
        title = tr("Claude Code wants to create %1").arg(file);
    else if (editsFiles(tool))
        title = tr("Claude Code wants to edit %1").arg(file);
    else if (tool == QLatin1String("WebFetch"))
        title = tr("Claude Code wants to fetch %1").arg(str(in, "url"));
    else if (tool == QLatin1String("Read") || tool == QLatin1String("Glob") || tool == QLatin1String("Grep"))
        title = tr("Claude Code wants to read outside the project: %1").arg(describeTool(tool, in));
    else
        title = tr("Claude Code wants to use %1").arg(str(req, "display_name").isEmpty() ? tool : str(req, "display_name"));

    QVBoxLayout *body = nullptr;
    QFrame *box = cardFrame(title, &body);
    if (tool == QLatin1String("Bash")) {
        auto *cmd = wrapLabel(str(in, "command"), box, "ccCode");
        cmd->setTextFormat(Qt::PlainText);
        cmd->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body->addWidget(cmd);
        if (!str(in, "description").isEmpty())
            body->addWidget(wrapLabel(str(in, "description"), box, "aiHint"));
    } else if (tool == QLatin1String("Edit") || tool == QLatin1String("Write")) {
        AiChange c;
        c.path = file;
        c.ok = true;
        c.op = tool == QLatin1String("Edit") ? QStringLiteral("replace") : QStringLiteral("create");
        c.before = str(in, "old_string").left(60000);
        c.after = (tool == QLatin1String("Edit") ? str(in, "new_string") : str(in, "content")).left(60000);
        auto *card = new AiChangeCard(c, box);
        connect(card, &AiChangeCard::openRequested, this, [this](const QString &rel) { emit openFileRequested(QDir(m_root).absoluteFilePath(rel)); });
        body->addWidget(card);
    } else if (!editsFiles(tool) && tool != QLatin1String("WebFetch")) {
        QString args = QString::fromUtf8(QJsonDocument(in).toJson(QJsonDocument::Indented));
        if (args.size() > 1200)
            args = args.left(1200) + QStringLiteral("…");
        auto *l = wrapLabel(args, box, "ccCode");
        l->setTextFormat(Qt::PlainText);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body->addWidget(l);
    }
    if (!str(req, "decision_reason").isEmpty())
        body->addWidget(wrapLabel(str(req, "decision_reason"), box, "aiHint"));

    QWidget *row = buttonRow(box, body);
    connect(cardButton(tr("Allow"), row, true), &QPushButton::clicked, this,
            [this, id, in] { answer(id, QJsonObject{{"behavior", "allow"}, {"updatedInput", in}}); });
    // Claude Code's own suggestions: a rule for this command, all edits for the session, a folder, …
    for (const QJsonValue &v : req.value(QStringLiteral("permission_suggestions")).toArray()) {
        const QJsonObject s = v.toObject();
        const QString type = str(s, "type"), dest = str(s, "destination");
        const QString where = dest == QLatin1String("localSettings")     ? tr(" in this project")
                              : dest == QLatin1String("projectSettings") ? tr(" in this project (shared)")
                              : dest == QLatin1String("userSettings")    ? tr(" everywhere")
                                                                         : tr(" this session");
        QString label;
        if (type == QLatin1String("setMode")) {
            const QString mode = str(s, "mode");
            label = mode == QLatin1String("acceptEdits") ? tr("Allow all edits this session") : tr("Switch to %1").arg(modeLabel(mode));
        } else if (type == QLatin1String("addRules") && str(s, "behavior") != QLatin1String("deny")) {
            QStringList rules;
            for (const QJsonValue &rv : s.value(QStringLiteral("rules")).toArray()) {
                const QJsonObject rule = rv.toObject();
                QString content = str(rule, "ruleContent");
                if (content.size() > 50)
                    content = content.left(50) + QStringLiteral("…");
                rules << (content.isEmpty() ? str(rule, "toolName") : QStringLiteral("`%1`").arg(content));
            }
            label = tr("Always allow %1%2").arg(rules.join(QStringLiteral(", ")), where);
        } else if (type == QLatin1String("addDirectories")) {
            QStringList dirs;
            for (const QJsonValue &d : s.value(QStringLiteral("directories")).toArray())
                dirs << QFileInfo(d.toString()).fileName();
            label = tr("Allow %1%2").arg(dirs.join(QStringLiteral(", ")), where);
        }
        if (label.isEmpty())
            continue;
        QPushButton *b = cardButton(label, row);
        b->setToolTip(QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Compact)));
        connect(b, &QPushButton::clicked, this, [this, id, in, s] {
            if (str(s, "type") == QLatin1String("setMode")) {
                SettingsManager::instance().setClaudeCode(QStringLiteral("permissionMode"), str(s, "mode"));
                updatePickers();
            }
            answer(id, QJsonObject{{"behavior", "allow"}, {"updatedInput", in}, {"updatedPermissions", QJsonArray{s}}});
        });
    }
    connect(cardButton(tr("Deny"), row), &QPushButton::clicked, this,
            [this, id] { answer(id, QJsonObject{{"behavior", "deny"}, {"message", "The user denied this. Ask how to proceed if unsure."}}); });
    static_cast<QHBoxLayout *>(row->layout())->addStretch(1);
    return box;
}

QWidget *ClaudeCodePanel::questionCard(const QString &id, const QJsonObject &req)
{
    const QJsonObject in = req.value(QStringLiteral("input")).toObject();
    const QJsonArray questions = in.value(QStringLiteral("questions")).toArray();
    QVBoxLayout *body = nullptr;
    QFrame *box = cardFrame(questions.size() == 1 ? tr("Claude Code has a question") : tr("Claude Code has %1 questions").arg(questions.size()), &body);
    struct Q {
        QString text;
        QButtonGroup *group;
        QLineEdit *other;
    };
    auto qs = std::make_shared<QVector<Q>>();
    for (const QJsonValue &qv : questions) {
        const QJsonObject q = qv.toObject();
        if (!str(q, "header").isEmpty())
            body->addWidget(wrapLabel(str(q, "header").toUpper(), box, "aiWho"));
        auto *ql = wrapLabel(str(q, "question"), box, "ccCardTitle");
        ql->setTextFormat(Qt::PlainText);
        body->addWidget(ql);
        auto *group = new QButtonGroup(box);
        group->setExclusive(!q.value(QStringLiteral("multiSelect")).toBool());
        for (const QJsonValue &ov : q.value(QStringLiteral("options")).toArray()) {
            const QJsonObject opt = ov.toObject();
            QString text = str(opt, "label");
            if (!str(opt, "description").isEmpty())
                text += QLatin1Char('\n') + str(opt, "description");
            auto *b = new QPushButton(text, box);
            b->setObjectName(QStringLiteral("ccChoice"));
            b->setCheckable(true);
            b->setProperty("label", str(opt, "label"));
            b->setCursor(Qt::PointingHandCursor);
            group->addButton(b);
            body->addWidget(b);
        }
        auto *other = new QLineEdit(box);
        other->setObjectName(QStringLiteral("aiCustomModel"));
        other->setPlaceholderText(tr("Or type your own answer"));
        body->addWidget(other);
        qs->append({str(q, "question"), group, other});
    }
    auto *status = wrapLabel(QString(), box, "aiHint");
    status->hide();
    body->addWidget(status);
    QWidget *row = buttonRow(box, body);
    connect(cardButton(tr("Answer"), row, true), &QPushButton::clicked, this, [this, id, in, qs, status] {
        QJsonObject answers;
        for (const Q &q : *qs) {
            QString a = q.other->text().trimmed();
            if (a.isEmpty()) {
                QStringList picked;
                for (QAbstractButton *b : q.group->buttons())
                    if (b->isChecked())
                        picked << b->property("label").toString();
                a = picked.join(QStringLiteral(", "));
            }
            if (a.isEmpty()) {
                status->setText(tr("Answer every question first."));
                status->show();
                return;
            }
            answers.insert(q.text, a);
        }
        QJsonObject updated = in;
        updated.insert(QStringLiteral("answers"), answers);
        answer(id, QJsonObject{{"behavior", "allow"}, {"updatedInput", updated}});
    });
    connect(cardButton(tr("Skip"), row), &QPushButton::clicked, this,
            [this, id] { answer(id, QJsonObject{{"behavior", "deny"}, {"message", "The user chose not to answer. Continue with your best judgement."}}); });
    static_cast<QHBoxLayout *>(row->layout())->addStretch(1);
    return box;
}

QWidget *ClaudeCodePanel::planCard(const QString &id, const QJsonObject &req)
{
    const QJsonObject in = req.value(QStringLiteral("input")).toObject();
    QVBoxLayout *body = nullptr;
    QFrame *box = cardFrame(tr("Claude Code's plan"), &body);
    if (!str(in, "plan").trimmed().isEmpty()) {
        auto *view = new MarkdownView(box);
        view->document()->setDocumentMargin(2);
        view->setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; }"));
        view->setAutoHeight(true);
        view->showMarkdown(str(in, "plan"));
        connect(view, &QTextBrowser::anchorClicked, this, &ClaudeCodePanel::followLink);
        body->addWidget(view);
    }
    auto *feedback = new QLineEdit(box);
    feedback->setObjectName(QStringLiteral("aiCustomModel"));
    feedback->setPlaceholderText(tr("What should change? (for “Keep planning”)"));
    body->addWidget(feedback);
    QWidget *row = buttonRow(box, body);
    auto approve = [this, id, in](const QString &mode) {
        SettingsManager::instance().setClaudeCode(QStringLiteral("permissionMode"), mode);
        updatePickers();
        answer(id,
               QJsonObject{{"behavior", "allow"},
                           {"updatedInput", in},
                           {"updatedPermissions", QJsonArray{QJsonObject{{"type", "setMode"}, {"mode", mode}, {"destination", "session"}}}}});
    };
    connect(cardButton(tr("Approve, accept edits"), row, true), &QPushButton::clicked, this,
            [approve] { approve(QStringLiteral("acceptEdits")); });
    connect(cardButton(tr("Approve, ask before edits"), row), &QPushButton::clicked, this,
            [approve] { approve(QStringLiteral("default")); });
    connect(cardButton(tr("Keep planning"), row), &QPushButton::clicked, this, [this, id, feedback] {
        const QString f = feedback->text().trimmed();
        answer(id, QJsonObject{{"behavior", "deny"}, {"message", f.isEmpty() ? QStringLiteral("The user wants to keep planning.") : QStringLiteral("Keep planning. The user says: ") + f}});
    });
    static_cast<QHBoxLayout *>(row->layout())->addStretch(1);
    return box;
}

void ClaudeCodePanel::answer(const QString &id, const QJsonObject &decision)
{
    QWidget *card = m_permissions.take(id);
    if (!card)
        return;
    m_session->answerPermission(id, decision);
    // The card goes; the tool's row in the turn shows what came of it (a diff, output, or the denial).
    card->deleteLater();
    if (m_permissions.isEmpty()) {
        m_statusLabel->clear();
        closeNotifications(false);
    }
    m_input->setFocus();
}

void ClaudeCodePanel::cancelPermissions(const QString &why)
{
    for (QWidget *card : std::as_const(m_permissions))
        card->deleteLater();
    if (!m_permissions.isEmpty() && !why.isEmpty())
        addNote(why);
    if (!m_permissions.isEmpty())
        closeNotifications(false);
    m_permissions.clear();
}

// --- view -------------------------------------------------------------------------------------------------------------

agent::AgentTurnView *ClaudeCodePanel::newTurnView()
{
    auto *view = new agent::AgentTurnView(prettyModel(m_model), m_list, kName);
    connect(view, &agent::AgentTurnView::openRequested, this, [this](const QString &rel) { emit openFileRequested(QDir(m_root).absoluteFilePath(rel)); });
    connect(view, &agent::AgentTurnView::linkClicked, this, &ClaudeCodePanel::followLink);
    connect(view, &agent::AgentTurnView::askRequested, this, &ClaudeCodePanel::askAbout);
    connect(view, &agent::AgentTurnView::quoteRequested, this, &ClaudeCodePanel::setQuote);
    if (m_lastTurn)
        m_lastTurn->setCopyPinned(false);
    m_lastTurn = view;
    view->setCopyPinned(true);
    return view;
}

QWidget *ClaudeCodePanel::userBubble(const QString &text, const QVector<ImageAttachment> &images, const QStringList &context,
                                     const QString &quote)
{
    auto *box = new QFrame;
    box->setObjectName(QStringLiteral("aiUser"));
    auto *l = new QVBoxLayout(box);
    l->setContentsMargins(10, 8, 10, 9);
    l->setSpacing(4);
    auto *who = new QLabel(tr("YOU"), box);
    who->setObjectName(QStringLiteral("aiWho"));
    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 0, 0, 0);
    head->addWidget(who, 1);
    if (!text.isEmpty()) {
        auto *copy = new CopyButton(box, false);
        copy->setSource([text] { return text; });
        copy->revealOnHover(box);
        head->addWidget(copy);
    }
    l->addLayout(head);
    if (!quote.isEmpty()) {
        auto *q = new AiQuote(box);
        q->setText(quote);
        l->addWidget(q);
    }
    if (!text.isEmpty()) {
        auto *t = new QLabel(text, box);
        t->setTextFormat(Qt::PlainText);
        t->setWordWrap(true);
        t->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->addWidget(t);
    }
    if (!images.isEmpty()) {
        auto *row = new FlowLayout;
        for (const ImageAttachment &img : images)
            row->addWidget(thumbnailLabel(img, 64, box));
        l->addLayout(row);
    }
    for (const QString &c : context) {
        auto *sel = new QLabel(c, box);
        sel->setObjectName(QStringLiteral("aiHint"));
        sel->setTextFormat(Qt::PlainText);
        l->addWidget(sel);
    }
    return box;
}

void ClaudeCodePanel::addWidget(QWidget *w)
{
    w->setParent(m_list);
    m_listLayout->insertWidget(m_listLayout->count() - 1, w);
    w->show();
}

void ClaudeCodePanel::addNote(const QString &text, bool error)
{
    auto *l = wrapLabel(text, m_list, error ? "aiError" : "aiHint");
    l->setTextFormat(Qt::PlainText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    addWidget(l);
}

void ClaudeCodePanel::clearTranscript()
{
    setQuote(QString()); // it quotes this conversation
    m_permissions.clear();
    m_live = nullptr;
    m_hint = nullptr;
    while (m_listLayout->count() > 1) {
        QLayoutItem *item = m_listLayout->takeAt(0);
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

void ClaudeCodePanel::showEmptyHint()
{
    if (m_hint)
        return;
    const QString project = m_root.isEmpty() ? QString() : QFileInfo(m_root).fileName();
    auto *box = new QWidget(m_list);
    auto *col = new QVBoxLayout(box);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(6);
    auto *l = wrapLabel(m_root.isEmpty()
                            ? tr("<p><b>Open a project folder to work with Claude Code.</b></p>")
                            : tr("<p><b>Claude Code works in %1.</b></p>"
                                 "<p>It reads the project, edits files and runs commands, and asks first when its permission mode "
                                 "says so. Type <b>@</b> for a file and <b>/</b> for commands; selected lines and the open file can go "
                                 "along, <b>Ctrl+V</b> pastes a screenshot and <b>↑</b> recalls earlier prompts. Earlier conversations, "
                                 "the same ones <code>claude</code> keeps in the terminal, are in the history menu.</p>")
                                  .arg(project.toHtmlEscaped()),
                        box, "aiHint");
    l->setTextFormat(Qt::RichText);
    col->addWidget(l);
    if (!m_root.isEmpty() && ready())
        col->addWidget(suggestionList(box));
    m_hint = box;
    addWidget(box);
}

void ClaudeCodePanel::refreshEmptyHint()
{
    // Only while the transcript is the hint alone (and the stretch under it).
    if (!m_hint || m_listLayout->count() != 2)
        return;
    m_listLayout->removeWidget(m_hint);
    m_hint->deleteLater();
    m_hint = nullptr;
    showEmptyHint();
}

void ClaudeCodePanel::setGitDirty(bool dirty)
{
    if (dirty == m_gitDirty)
        return;
    m_gitDirty = dirty;
    refreshEmptyHint();
}

namespace {
struct Suggestion {
    const char *icon;
    QString label, prompt;
};

// The lint and test commands of a project, from cheap file checks (no processes). Empty when not found.
QPair<QString, QString> projectCommands(const QString &root)
{
    const QDir dir(root);
    QString lint, test;
    if (QFile f(dir.filePath(QStringLiteral("package.json"))); f.open(QIODevice::ReadOnly)) {
        const QJsonObject scripts = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("scripts")).toObject();
        const QString pm = dir.exists(QStringLiteral("pnpm-lock.yaml")) ? QStringLiteral("pnpm")
                           : dir.exists(QStringLiteral("yarn.lock"))    ? QStringLiteral("yarn")
                           : dir.exists(QStringLiteral("bun.lockb")) || dir.exists(QStringLiteral("bun.lock")) ? QStringLiteral("bun")
                                                                         : QStringLiteral("npm");
        if (scripts.contains(QStringLiteral("lint")))
            lint = pm + QStringLiteral(" run lint");
        if (scripts.contains(QStringLiteral("test")))
            test = pm + QStringLiteral(" run test");
    }
    if (lint.isEmpty()) {
        QFile py(dir.filePath(QStringLiteral("pyproject.toml")));
        if ((py.open(QIODevice::ReadOnly) && py.readAll().contains("[tool.ruff")) || dir.exists(QStringLiteral("ruff.toml")) ||
            dir.exists(QStringLiteral(".ruff.toml")))
            lint = QStringLiteral("ruff check --fix");
    }
    if (dir.exists(QStringLiteral("Cargo.toml"))) {
        if (lint.isEmpty())
            lint = QStringLiteral("cargo clippy");
        if (test.isEmpty())
            test = QStringLiteral("cargo test");
    }
    if (dir.exists(QStringLiteral("go.mod"))) {
        if (lint.isEmpty())
            lint = QStringLiteral("go vet ./...");
        if (test.isEmpty())
            test = QStringLiteral("go test ./...");
    }
    if (lint.isEmpty())
        if (const QString gradlew = Platform::wrapperCommand(dir.path(), QStringLiteral("gradlew")); !gradlew.isEmpty())
            lint = gradlew + QStringLiteral(" check");
    return {lint, test};
}
} // namespace

// What to start with, from what the project has. A row puts its prompt in the input to be edited and sent; /init runs.
QWidget *ClaudeCodePanel::suggestionList(QWidget *parent)
{
    QVector<Suggestion> items{{"sparkles", tr("Explain this project"),
                               tr("Give me an overview of this project: what it does, how it is laid out, and how to build and run it.")}};
    if (m_gitDirty)
        items.append({"git-branch", tr("Review uncommitted changes"), tr("Review my uncommitted changes (git diff) for bugs and suggest fixes.")});
    const auto [lint, test] = projectCommands(m_root);
    if (!lint.isEmpty())
        items.append({"check", tr("Run lint and fix"), tr("Run `%1` and fix what it reports.").arg(lint)});
    else if (!test.isEmpty())
        items.append({"play", tr("Run the tests"), tr("Run `%1` and fix any failures.").arg(test)});
    if (!QFileInfo::exists(m_root + QStringLiteral("/CLAUDE.md")))
        items.append({"file-plus", tr("Set up CLAUDE.md"), QStringLiteral("/init")});

    auto *box = new QWidget(parent);
    auto *l = new QVBoxLayout(box);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(2);
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    for (const Suggestion &sg : std::as_const(items)) {
        auto *b = new QPushButton(Icons::tinted(QStringLiteral(":/new-icons/%1.svg").arg(QLatin1String(sg.icon)), t.textMuted), sg.label, box);
        b->setObjectName(QStringLiteral("ccSuggestion"));
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(sg.prompt);
        const int s = SettingsManager::instance().scaled(16);
        b->setIconSize(QSize(s, s));
        connect(b, &QPushButton::clicked, this, [this, prompt = sg.prompt] {
            m_input->setPlainText(prompt);
            m_input->moveCursor(QTextCursor::End);
            if (prompt.startsWith(QLatin1Char('/')))
                send();
            else
                m_input->setFocus();
        });
        l->addWidget(b);
    }
    return box;
}

void ClaudeCodePanel::recallPrompt(int delta)
{
    if (!m_input->recalling())
        m_recallPos = int(m_recall.size()); // a fresh start: Up goes to the newest
    m_recallPos = qBound(0, m_recallPos + delta, int(m_recall.size()));
    m_input->setRecalled(m_recallPos < m_recall.size() ? m_recall.at(m_recallPos) : QString());
}

void ClaudeCodePanel::notePrompt(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return;
    m_recall.removeAll(t); // once, at its newest place
    m_recall << t;
    if (m_recall.size() > 200)
        m_recall.removeFirst();
}

void ClaudeCodePanel::updateSlash()
{
    const QString text = m_input->toPlainText();
    // A recalled command stays plain text: Up / Down go on recalling instead of stepping the list.
    if (!text.startsWith(QLatin1Char('/')) || text.contains(QLatin1Char(' ')) || text.contains(QLatin1Char('\n')) || m_busy || m_input->recalling()) {
        if (m_slash)
            closeMentions();
        return;
    }
    QStringList all{tr("/clear   start a new conversation"), tr("/resume   continue an earlier conversation"),
                    tr("/login   sign in to Claude Code"), tr("/logout   sign out of Claude Code")};
    for (const QString &c : std::as_const(m_commands))
        if (!c.startsWith(QLatin1String("/clear ")) && !c.startsWith(QLatin1String("/resume ")) && !c.startsWith(QLatin1String("/login ")) &&
            !c.startsWith(QLatin1String("/logout ")))
            all << c;
    QStringList items;
    for (const QString &c : std::as_const(all))
        if (c.startsWith(text, Qt::CaseInsensitive))
            items << c;
    if (items.isEmpty()) {
        if (m_slash)
            closeMentions();
        return;
    }
    m_slash = true;
    m_popup->setItems(items.mid(0, 40));
    m_input->setMentionOpen(true);
    m_popup->showAbove(m_inputBox);
}

void ClaudeCodePanel::updateMentions(const QString &query)
{
    if (m_files.isEmpty()) {
        closeMentions();
        return;
    }
    QStringList result;
    if (query.isEmpty()) {
        result = m_files.mid(0, 8);
    } else {
        QVector<QPair<int, int>> scored;
        for (int i = 0; i < m_files.size(); ++i) {
            const QString &f = m_files.at(i);
            const int s = FuzzyMatcher::score(query, f, f.lastIndexOf(QLatin1Char('/')) + 1);
            if (s >= 0)
                scored.append({s, i});
        }
        std::sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
        for (int i = 0; i < qMin(8, int(scored.size())); ++i)
            result << m_files.at(scored.at(i).second);
    }
    if (result.isEmpty()) {
        closeMentions();
        return;
    }
    m_slash = false;
    m_popup->setItems(result);
    m_input->setMentionOpen(true);
    m_popup->showAbove(m_inputBox);
}

void ClaudeCodePanel::closeMentions()
{
    m_slash = false;
    m_input->setMentionOpen(false);
    if (m_popup->isVisible())
        m_popup->hide();
}

void ClaudeCodePanel::updatePickers()
{
    const QString model = SettingsManager::instance().claudeCode(QStringLiteral("model"));
    QString label;
    for (const Choice &c : kModels)
        if (model == QLatin1String(c.id))
            label = QString::fromLatin1(c.label);
    if (!m_model.isEmpty())
        label = model.isEmpty() ? tr("%1 (default)").arg(prettyModel(m_model)) : prettyModel(m_model);
    else if (label.isEmpty())
        label = model;
    else if (model.isEmpty())
        label = tr("Default model");
    m_modelBtn->setText(label + QStringLiteral("  ▾"));
    QString mode = SettingsManager::instance().claudeCode(QStringLiteral("permissionMode"));
    if (mode.isEmpty())
        mode = QStringLiteral("default");
    m_modeBtn->setText(modeLabel(mode) + QStringLiteral("  ▾"));
    const QList<QAction *> modelActs = m_modelBtn->menu()->actions();
    for (int i = 0; i < modelActs.size(); ++i) {
        modelActs.at(i)->setCheckable(true);
        modelActs.at(i)->setChecked(model == QLatin1String(kModels[i].id));
    }
    const QList<QAction *> modeActs = m_modeBtn->menu()->actions();
    for (int i = 0; i < modeActs.size(); ++i) {
        modeActs.at(i)->setCheckable(true);
        modeActs.at(i)->setChecked(mode == QLatin1String(kModes[i].id));
    }
    m_modeBtn->setProperty("danger", mode == QLatin1String("bypassPermissions"));
    m_modeBtn->style()->unpolish(m_modeBtn);
    m_modeBtn->style()->polish(m_modeBtn);
}

void ClaudeCodePanel::updateStats()
{
    if (m_live) {
        agent::RequestStats cur = m_cur;
        if (m_requesting)
            cur.ms = m_reqClock.elapsed();
        m_live->setLiveStats(m_done, cur, m_requesting, m_turnClock.elapsed());
        // The clock keeps running while a card waits for the user.
        const QString elapsed = agent::formatElapsed(m_turnClock.elapsed());
        if (m_stopping)
            m_statusLabel->setText(tr("Stopping…"));
        else if (!m_permissions.isEmpty())
            m_statusLabel->setText(tr("Waiting for you · %1").arg(elapsed));
        else
            m_statusLabel->setText(tr("Working %1").arg(elapsed));
    }
    QStringList parts;
    // A subscription is not billed per request; the CLI's figure is only meaningful when paying per use.
    const QString method = m_auth->status().method;
    if (m_cost > 0 && method != QLatin1String("claude.ai"))
        parts << tr("$%1 this conversation").arg(m_cost, 0, 'f', m_cost < 1 ? 4 : 2);
    if (!m_limitText.isEmpty())
        parts << m_limitText;
    m_usage->setText(parts.join(QStringLiteral("  ·  ")));
}

int ClaudeCodePanel::contextWindow() const
{
    if (m_contextWindow > 0)
        return m_contextWindow;
    const QString model = m_model.isEmpty() ? SettingsManager::instance().claudeCode(QStringLiteral("model")) : m_model;
    return model.endsWith(QLatin1String("[1m]")) ? 1000000 : 200000;
}

void ClaudeCodePanel::updateMeter()
{
    m_meter->setUsage(m_contextUsed, contextWindow(), m_compacted);
    m_meter->setEnabled(!m_sessionId.isEmpty()); // nothing to compact in an empty conversation
}

// Sent the way the / popup sends a command: as plain text, while attachments wait for the next message. The CLI
// reports the result through compact_boundary, so there is no confirmation.
void ClaudeCodePanel::compactConversation()
{
    if (m_sessionId.isEmpty() || m_root.isEmpty() || !ready())
        return;
    Prompt p;
    p.text = QStringLiteral("/compact");
    if (m_busy) {
        m_queue.append(p);
        updateQueue();
        return;
    }
    dispatch(p);
}

void ClaudeCodePanel::scheduleProjectChanged()
{
    m_changedTimer->start();
}

void ClaudeCodePanel::followLink(const QUrl &url)
{
    QString abs;
    int line = 0;
    if (AiEdits::resolveFileLink(m_root, url, &abs, &line)) {
        emit openFileRequested(abs, line);
        return;
    }
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

QString ClaudeCodePanel::relative(const QString &path) const
{
    if (path.isEmpty() || m_root.isEmpty())
        return path;
    const QString prefix = m_root + QLatin1Char('/');
    if (path.startsWith(prefix))
        return path.mid(prefix.size());
    if (path.startsWith(QDir::homePath() + QLatin1Char('/')))
        return QStringLiteral("~") + path.mid(QDir::homePath().size());
    return path;
}

// The input lines up with the transcript's column (centred in the scroll area's viewport, so the scroll bar is left
// out). The transcript itself is not given margins here: a layout sees a Resize before any event filter or resizeEvent,
// so margins set from either came one layout late, and every step of a splitter drag reflowed each reply and tool row
// twice (to the new width and back to the column) where the column did not change at all. Its maximum width keeps it
// at the column instead.
void ClaudeCodePanel::fitColumn()
{
    const int side = qMax(10, (m_scroll->viewport()->width() - kColumn) / 2);
    m_bottom->setContentsMargins(side, 4, qMax(10, width() - side - kColumn), 10);
}

bool ClaudeCodePanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Resize && watched == m_scroll->viewport())
        fitColumn();
    if (watched == m_input && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        m_inputBox->setProperty("focused", event->type() == QEvent::FocusIn);
        m_inputBox->style()->unpolish(m_inputBox);
        m_inputBox->style()->polish(m_inputBox);
    }
    return QWidget::eventFilter(watched, event);
}

void ClaudeCodePanel::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    setStyleSheet(QStringLiteral(
                      "QWidget#agentRoot, QWidget#aiList, QScrollArea#ccScroll { background: %1; }"
                      "QWidget#agentHeader { background: %4; border-bottom: 1px solid %5; }"
                      "QWidget#agentHeader QLabel { background: transparent; border: none; }"
                      "QLabel#aiWho { color: %2; font-size: 8pt; letter-spacing: 1px; background: transparent; }"
                      "QLabel#aiHint { color: %2; background: transparent; }"
                      "QLabel#aiError { color: %3; background: transparent; padding: 2px 4px; }"
                      "QLabel#ccSetupTitle { font-size: 15pt; font-weight: 600; background: transparent; }"
                      "QLabel#ccCardTitle { font-weight: 600; background: transparent; }"
                      "QLabel#ccCode, QLineEdit#ccCode { font-family: monospace; background: %6; border: 1px solid %5; border-radius: 6px; padding: 6px 8px; }"
                      "QFrame#aiUser { background: %4; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#aiUser QLabel { background: transparent; }"
                      "QFrame#aiNotice { background: %4; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#aiNotice > QLabel { background: transparent; }"
                      "QFrame#aiInputBox { background: %6; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#aiInputBox[focused=\"true\"] { border-color: %7; }"
                      "AiInput { background: transparent; border: none; padding: 2px; }"
                      "QLineEdit#aiCustomModel { background: %6; border: 1px solid %5; border-radius: 6px; padding: 5px 8px; }"
                      "QLineEdit#aiCustomModel:focus { border-color: %7; }"
                      "QToolButton#aiSend { background: %9; border: none; border-radius: 13.5px; padding: 0px; }"
                      "QToolButton#aiSend:disabled { background: %4; }"
                      "QToolButton#ccPicker { color: %2; background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 2px 6px; }"
                      "QToolButton#ccPicker:hover { border-color: %5; }"
                      "QToolButton#ccPicker[danger=\"true\"] { color: %3; }"
                      "QToolButton#ccPicker::menu-indicator { image: none; width: 0px; }"
                      "QPushButton#ccChoice { text-align: left; background: %6; border: 1px solid %5; border-radius: 8px; padding: 8px 12px; }"
                      "QPushButton#ccChoice:hover, QPushButton#ccChoice:checked { border-color: %7; }"
                      "QPushButton#ccPrimary { background: %7; color: %8; border: none; border-radius: 6px; padding: 4px 14px; }"
                      "QPushButton#ccPrimary:disabled { background: %5; }"
                      "QPushButton#agentFooterBtn { background: transparent; border: 1px solid %5; border-radius: 6px; padding: 3px 10px; }"
                      "QPushButton#agentFooterBtn:hover { border-color: %7; }"
                      "QPushButton#agentFooterBtn:disabled { color: %2; }"
                      // attachments, queue, tasks, files changed
                      "QFrame#ccChip { background: %4; border: 1px solid %5; border-radius: 6px; }"
                      "QPushButton#ccChipText { background: transparent; border: none; padding: 1px 3px; text-align: left; }"
                      "QToolButton#ccChipX { background: transparent; border: none; border-radius: 4px; padding: 0px; }"
                      "QToolButton#ccChipX:hover { background: %5; }"
                      "QLabel#ccThumb { background: %6; border: 1px solid %5; border-radius: 4px; }"
                      "QFrame#ccQueued { background: %4; border: 1px dashed %5; border-radius: 8px; }"
                      "QFrame#ccQueued QLabel { background: transparent; }"
                      "QFrame#ccTasks { background: %4; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#ccTasks QLabel { background: transparent; }"
                      "QPushButton#ccTasksHeader { background: transparent; border: none; text-align: left; padding: 2px; color: %2; font-weight: 600; }"
                      "QLabel#ccTaskNow { font-weight: 600; }"
                      "QLabel#ccTaskDone { color: %2; text-decoration: line-through; }"
                      "QLabel#ccTaskMore { color: %2; }"
                      "QFrame#ccChanges { background: %4; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#ccChanges QLabel { background: transparent; }"
                      "QFrame#ccFileRow { background: transparent; border-radius: 6px; }"
                      "QFrame#ccFileRow:hover, QFrame#ccFileRow[current=\"true\"] { background: %6; }"
                      // /usage and /mcp cards
                      "QFrame#ccInfo { background: %4; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#ccInfo QLabel, QFrame#ccInfo QWidget { background: transparent; }"
                      "QLabel#ccInfoTitle { font-weight: 600; font-size: 11pt; }"
                      "QLabel#ccInfoSection { color: %2; font-weight: 600; font-size: 8pt; letter-spacing: 1px; padding-top: 4px; }"
                      "QLabel#ccInfoStrong { font-weight: 600; }"
                      "QLabel#ccInfoSmall { color: %2; font-size: 8pt; }"
                      "QToolButton#ccInfoRefresh { border: none; border-radius: 4px; padding: 2px; }"
                      "QToolButton#ccInfoRefresh:hover { background: %5; }"
                      "QFrame#ccSegBox { border: 1px solid %5; border-radius: 6px; }"
                      "QPushButton#ccSeg { background: transparent; border: none; border-radius: 5px; padding: 3px 12px; color: %2; }"
                      "QPushButton#ccSeg:checked { background: %5; color: palette(text); }"
                      "QPushButton#ccLink { background: transparent; border: none; padding: 2px 0px; color: %7; text-align: left; }"
                      "QPushButton#ccLink:hover { text-decoration: underline; }"
                      "QPushButton#ccSuggestion { text-align: left; background: transparent; border: none; border-radius: 6px; padding: 6px 8px; }"
                      "QPushButton#ccSuggestion:hover { background: %6; }")
                      .arg(t.window.name(), t.textMuted.name(), t.danger.name(), t.panel.name(), t.border.name(), t.editorBg.name(), t.accent.name(),
                           t.onAccent().name(), Theme::claudeCode().name()));
    if (auto *logo = findChild<QLabel *>(QStringLiteral("ccLogo")))
        logo->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/claudecode.svg"), Theme::claudeCode(), 16));
    updateSendButton();
}

// Send, or while Claude Code works: Stop with an empty input, Queue with something written.
void ClaudeCodePanel::updateSendButton()
{
    const bool stop = m_busy && !hasDraft();
    m_send->setToolTip(stop ? tr("Stop") : m_busy ? tr("Queue (Enter): sent when Claude Code is done") : tr("Send (Enter)"));
    m_send->setIcon(Icons::tinted(stop ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/arrow-up.svg"), Qt::white));
    m_send->setIconSize(QSize(16, 16));
}

} // namespace claudecode
