#include "AgentPanel.h"
#include "platform/Platform.h"
#include "platform/AppPaths.h"
#include "history/LocalHistory.h"

#include "agent/AgentRun.h"
#include "agent/AgentToolHelpers.h"
#include "agent/AgentTurnView.h"
#include "claudecode/ClaudeCodeInfo.h"
#include "mcp/McpClient.h"
#include "mcp/McpDialog.h"
#include "mcp/McpManager.h"
#include "ai/AiEdits.h"
#include "ai/AiEnv.h"
#include "ai/AiInput.h"
#include "ai/AiKeyDialog.h"
#include "ai/AiModels.h"
#include "ai/AiQuote.h"
#include "ai/AiWorkspace.h"
#include "ai/ClaudeClient.h"
#include "ai/GeminiClient.h"
#include "editor/CodeEditor.h"
#include "lsp/LspManager.h"
#include "lsp/LspServers.h"
#include "lsp/LspTypes.h"
#include "media/CopyButton.h"
#include "media/MarkdownView.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "palette/FuzzyMatcher.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QActionGroup>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QDebug>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStyle>
#include <QTextCursor>
#include <QTextDocument>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace agent {

namespace {
using ModelInfo = AiModels::Info;
const auto &kClaudeModels = AiModels::kClaude;
const auto &kGeminiModels = AiModels::kGemini;
const struct {
    const char *id, *label;
} kEfforts[] = {{"low", "Low: quick and cheap"},
                {"medium", "Medium: balanced"},
                {"high", "High: more thorough"},
                {"xhigh", "Extra high: hard problems"},
                {"max", "Max: no limit on thinking"}};

using AiModels::providerColor;
using AiModels::providerIcon;
QString modelLabel(const QString &id) { return AiModels::label(id); }

bool looksBinary(const QString &name)
{
    static const QSet<QString> bin = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("webp"),
                                      QStringLiteral("ico"), QStringLiteral("pdf"), QStringLiteral("zip"), QStringLiteral("gz"), QStringLiteral("jar"),
                                      QStringLiteral("ttf"), QStringLiteral("otf"), QStringLiteral("woff"), QStringLiteral("woff2"), QStringLiteral("mp3"),
                                      QStringLiteral("mp4"), QStringLiteral("wav"), QStringLiteral("so"), QStringLiteral("a"), QStringLiteral("o"),
                                      QStringLiteral("class"), QStringLiteral("deb"), QStringLiteral("exe"), QStringLiteral("bin"), QStringLiteral("lock")};
    return bin.contains(QFileInfo(name).suffix().toLower());
}

Diag toDiag(const QString &path, const LspDiagnostic &d)
{
    Diag x;
    x.path = path;
    x.line = d.startLine + 1;
    x.column = d.startColumn + 1;
    x.severity = d.severity;
    x.message = d.message;
    x.source = d.source;
    x.code = d.code;
    return x;
}
} // namespace

AgentPanel::AgentPanel(QWidget *parent)
    : QWidget(parent)
    , m_index(new CodeIndex(this))
    , m_mcp(new mcp::Manager(this))
    , m_gemini(new GeminiClient(this))
    , m_claude(new ClaudeClient(this))
    , m_changedTimer(new QTimer(this))
    , m_clockTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("agentRoot"));
    setAttribute(Qt::WA_StyledBackground); // the agent view has its own, darker background (Theme::agentView)
    m_changedTimer->setSingleShot(true);
    m_changedTimer->setInterval(400);
    connect(m_changedTimer, &QTimer::timeout, this, &AgentPanel::projectChanged);
    m_clockTimer->setInterval(1000);
    connect(m_clockTimer, &QTimer::timeout, this, &AgentPanel::updateContext);
    // --- header -------------------------------------------------------------------------------------------------
    auto *title = new QLabel(tr("QODE"), this);
    title->setObjectName(QStringLiteral("panelTitle"));
    m_historyBtn = new QToolButton(this);
    Icons::bind(m_historyBtn, QStringLiteral(":/new-icons/scroll-text.svg"));
    m_historyBtn->setToolTip(tr("Chats of this project"));
    m_historyBtn->setPopupMode(QToolButton::InstantPopup);
    m_historyBtn->setAutoRaise(true);
    auto *historyMenu = new QMenu(m_historyBtn);
    m_historyBtn->setMenu(historyMenu);
    connect(historyMenu, &QMenu::aboutToShow, this, &AgentPanel::rebuildHistoryMenu);
    m_keyBtn = new QToolButton(this);
    Icons::bind(m_keyBtn, QStringLiteral(":/new-icons/key.svg"));
    m_keyBtn->setToolTip(tr("API keys (Gemini, Claude)"));
    m_keyBtn->setAutoRaise(true);
    m_keyBtn->setPopupMode(QToolButton::InstantPopup);
    auto *keyMenu = new QMenu(m_keyBtn);
    m_keyBtn->setMenu(keyMenu);
    connect(keyMenu, &QMenu::aboutToShow, this, [this, keyMenu] {
        keyMenu->clear();
        const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
        for (AiEnv::Provider p : {AiEnv::Provider::Claude, AiEnv::Provider::Gemini}) {
            const bool claude = p == AiEnv::Provider::Claude;
            const bool has = !AiEnv::apiKey(p, m_root).isEmpty();
            QAction *a = keyMenu->addAction(Icons::tinted(claude ? QStringLiteral(":/new-icons/claude.svg") : QStringLiteral(":/new-icons/gemini.svg"),
                                                          claude ? t.brandClaude : t.brandGemini),
                                            has ? tr("%1 API key ✓").arg(AiEnv::providerName(p)) : tr("%1 API key…").arg(AiEnv::providerName(p)));
            connect(a, &QAction::triggered, this, [this, p] { editApiKeyFor(p); });
        }
    });
    m_indexBtn = new QToolButton(this);
    Icons::bind(m_indexBtn, QStringLiteral(":/new-icons/search.svg"));
    m_indexBtn->setAutoRaise(true);
    m_indexBtn->setPopupMode(QToolButton::InstantPopup);
    auto *indexMenu = new QMenu(m_indexBtn);
    indexMenu->setToolTipsVisible(true);
    m_indexBtn->setMenu(indexMenu);
    connect(indexMenu, &QMenu::aboutToShow, this, &AgentPanel::rebuildIndexMenu);
    connect(m_index, &CodeIndex::statusChanged, this, &AgentPanel::updateIndexButton);
    updateIndexButton();
    m_mcpBtn = new QToolButton(this);
    Icons::bind(m_mcpBtn, QStringLiteral(":/new-icons/server.svg"));
    m_mcpBtn->setAutoRaise(true);
    m_mcpBtn->setPopupMode(QToolButton::InstantPopup);
    auto *mcpMenu = new QMenu(m_mcpBtn);
    mcpMenu->setToolTipsVisible(true);
    m_mcpBtn->setMenu(mcpMenu);
    connect(mcpMenu, &QMenu::aboutToShow, this, &AgentPanel::rebuildMcpMenu);
    connect(m_mcp, &mcp::Manager::changed, this, &AgentPanel::updateMcpButton);
    updateMcpButton();

    m_usageBtn = new QToolButton(this);
    m_usageBtn->setObjectName(QStringLiteral("aiUsage"));
    m_usageBtn->setAutoRaise(true);
    m_usageBtn->setPopupMode(QToolButton::InstantPopup);
    m_usageBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    useTabularDigits(m_usageBtn);
    auto *usageMenu = new QMenu(m_usageBtn);
    m_usageBtn->setMenu(usageMenu);
    connect(usageMenu, &QMenu::aboutToShow, this, &AgentPanel::rebuildUsageMenu);
    m_usageBtn->hide();

    m_newBtn = new QToolButton(this);
    Icons::bind(m_newBtn, QStringLiteral(":/new-icons/plus.svg"));
    m_newBtn->setToolTip(tr("New chat"));
    m_newBtn->setAutoRaise(true);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("agentHeader"));
    header->setAttribute(Qt::WA_StyledBackground);
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->addWidget(title, 1);
    hl->addWidget(m_usageBtn);
    hl->addWidget(m_indexBtn);
    hl->addWidget(m_mcpBtn);
    hl->addWidget(m_keyBtn);
    hl->addWidget(m_historyBtn);
    hl->addWidget(m_newBtn);

    // --- model --------------------------------------------------------------------------------------------------
    m_modelName = SettingsManager::instance().aiModel();
    m_modelBtn = new QPushButton(this);
    m_modelBtn->setObjectName(QStringLiteral("aiModel"));
    m_modelBtn->setToolTip(tr("Model: a Claude or Gemini model, or type any model id"));
    m_modelBtn->setCursor(Qt::PointingHandCursor);
    {
        auto *bl = new QHBoxLayout(m_modelBtn);
        bl->setContentsMargins(12, 0, 10, 0);
        bl->setSpacing(8);
        m_modelIcon = new QLabel(m_modelBtn);
        m_modelIcon->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_modelIcon->setFixedSize(16, 16);
        bl->addWidget(m_modelIcon);
        m_modelText = new QLabel(m_modelBtn);
        m_modelText->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_modelText->setObjectName(QStringLiteral("aiModelText"));
        bl->addWidget(m_modelText, 1);
        m_modelArrow = new QLabel(m_modelBtn);
        m_modelArrow->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_modelArrow->setFixedSize(14, 14);
        bl->addWidget(m_modelArrow);
    }
    m_modelMenu = new QMenu(m_modelBtn);
    m_modelBtn->setMenu(m_modelMenu);
    connect(m_modelMenu, &QMenu::aboutToShow, this, &AgentPanel::rebuildModelMenu);
    auto *modelLabel = new QLabel(tr("Model"), this);
    modelLabel->setObjectName(QStringLiteral("aiWho"));
    auto *modelRow = m_modelRow = new QHBoxLayout;
    modelRow->setContentsMargins(10, 10, 10, 6);
    modelRow->setSpacing(12);
    modelRow->addWidget(modelLabel);
    modelRow->addWidget(m_modelBtn, 1);
    setModel(m_modelName);

    // --- notice -------------------------------------------------------------------------------------------------
    m_notice = new QFrame(this);
    m_notice->setObjectName(QStringLiteral("aiNotice"));
    m_noticeText = new QLabel(m_notice);
    m_noticeText->setWordWrap(true);
    m_noticeText->setTextFormat(Qt::RichText);
    m_noticeBtn = new QPushButton(m_notice);
    auto *nl = new QVBoxLayout(m_notice);
    nl->setContentsMargins(10, 8, 10, 8);
    nl->setSpacing(6);
    nl->addWidget(m_noticeText);
    nl->addWidget(m_noticeBtn, 0, Qt::AlignLeft);
    connect(m_noticeBtn, &QPushButton::clicked, this, [this] {
        if (!m_gitOk && !m_root.isEmpty())
            emit gitInitRequested();
        else
            editApiKey();
    });

    // --- transcript ---------------------------------------------------------------------------------------------
    m_scroll = new QScrollArea(this);
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
    // Follow the output while the user is at the bottom; scrolling up pauses that.
    QScrollBar *bar = m_scroll->verticalScrollBar();
    connect(bar, &QScrollBar::valueChanged, this, [this, bar](int v) { m_stick = v >= bar->maximum() - 24; });
    connect(bar, &QScrollBar::rangeChanged, this, [this, bar] {
        if (m_stick)
            bar->setValue(bar->maximum());
    });

    // --- input --------------------------------------------------------------------------------------------------
    m_context = new QLabel(this);
    m_context->setObjectName(QStringLiteral("aiWho"));
    useTabularDigits(m_context);
    m_input = new AiInput(this);
    m_input->setPlaceholderText(tr("Ask QODE anything…  @ points at a file"));
    m_send = new QToolButton(this);
    m_send->setObjectName(QStringLiteral("aiSend"));
    m_send->setCursor(Qt::PointingHandCursor);
    // Fixed so the radius below always makes a circle, whatever the zoom. The radius is just under half: zoom rounds both
    // numbers, and a radius over half the side (14px -> 20 on a 39px button at 140%) makes Qt drop it and draw a square.
    m_send->setFixedSize(28, 28);
    m_inputBox = new QFrame(this);
    m_inputBox->setObjectName(QStringLiteral("aiInputBox"));
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
    ibCol->addLayout(ib);
    m_input->installEventFilter(this); // focus outlines the box

    auto *bottom = m_bottom = new QVBoxLayout;
    bottom->setContentsMargins(10, 4, 10, 10);
    bottom->setSpacing(4);
    bottom->addWidget(m_context);
    bottom->addWidget(m_inputBox);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(header);
    lay->addLayout(modelRow);
    m_noticeRow = new QHBoxLayout;
    m_noticeRow->addWidget(m_notice);
    lay->addLayout(m_noticeRow);
    lay->addWidget(m_scroll, 1);
    lay->addLayout(bottom);

    m_popup = new AiMentionPopup(this);
    m_popup->hide();

    // --- wiring -------------------------------------------------------------------------------------------------
    connect(m_newBtn, &QToolButton::clicked, this, &AgentPanel::startNewChat);
    connect(m_send, &QToolButton::clicked, this, [this] { m_busy ? stop() : send(); });
    connect(m_input, &AiInput::sendRequested, this, &AgentPanel::send);
    connect(m_input, &AiInput::mentionQuery, this, &AgentPanel::updateMentions);
    connect(m_input, &AiInput::mentionClosed, this, &AgentPanel::closeMentions);
    connect(m_input, &AiInput::textChanged, this, [this] { updateSlash(); });
    connect(m_input, &AiInput::historyRequested, this, &AgentPanel::recallPrompt);
    connect(m_input, &AiInput::mentionKey, this, [this](int key) {
        if (m_slash && key != Qt::Key_Up && key != Qt::Key_Down && key != Qt::Key_Escape && m_popup->count()) {
            const QString cmd = m_popup->current().section(QLatin1Char(' '), 0, 0);
            const bool exact = m_input->toPlainText().trimmed() == cmd;
            closeMentions();
            if (exact) {
                send();
            } else {
                m_input->setPlainText(cmd);
                QTextCursor c = m_input->textCursor();
                c.movePosition(QTextCursor::End);
                m_input->setTextCursor(c);
            }
            return;
        }
        if (key == Qt::Key_Up)
            m_popup->step(-1);
        else if (key == Qt::Key_Down)
            m_popup->step(1);
        else if (key == Qt::Key_Escape)
            closeMentions();
        else if (m_popup->count()) {
            m_input->completeMention(m_popup->current());
            closeMentions();
        } else {
            closeMentions();
        }
    });
    connect(m_popup, &AiMentionPopup::chosen, this, [this](const QString &p) {
        if (m_slash) {
            closeMentions();
            m_input->setPlainText(p.section(QLatin1Char(' '), 0, 0));
            m_input->setFocus();
            return;
        }
        m_input->completeMention(p);
        closeMentions();
        m_input->setFocus();
    });

    AgentTools::Hooks hooks;
    hooks.readFile = [this](const QString &abs, QString *text) { return m_editors && AiWorkspace(m_editors, m_root).readFile(abs, text); };
    hooks.apply = [this](const QVector<AiEdits::Edit> &edits) {
        return m_editors ? AiWorkspace(m_editors, m_root).apply(edits) : QVector<AiEdits::Outcome>();
    };
    hooks.projectFiles = [this] { return m_allFiles; };
    hooks.openFiles = [this] {
        QVector<QPair<QString, bool>> out;
        if (m_editors)
            for (const QString &p : m_editors->openFilePaths()) {
                Document *d = m_editors->documentForPath(p);
                if (Platform::isUnder(p, m_root))
                    out.append({p, d && d->isModified()});
            }
        return out;
    };
    hooks.saveFiles = [this](const QStringList &paths) {
        if (!m_editors)
            return;
        const LocalHistory::Reason reason(LocalHistory::Kind::QodeEdit);
        bool any = false;
        for (const QString &p : paths)
            if (Document *d = m_editors->documentForPath(p))
                any = m_editors->saveQuietly(d) || any;
        if (any)
            scheduleProjectChanged();
    };
    hooks.pathDeleted = [this](const QString &p) {
        if (m_editors)
            m_editors->closeDocumentsUnder(p);
    };
    hooks.pathMoved = [this](const QString &from, const QString &to) {
        LocalHistory::pathMoved(from, to);
        if (m_editors)
            m_editors->pathRenamed(from, to);
    };
    hooks.changed = [this] { scheduleProjectChanged(); };
    hooks.approve = [this](const QString &tool, const QString &title, const QString &detail) { return approve(tool, title, detail); };
    hooks.diagnostics = [this](const QString &abs, QVector<Diag> *rows, bool afterEdit, bool settle) {
        return collectDiagnostics(abs, rows, afterEdit, settle);
    };
    hooks.knownDiagnostics = [this](const QString &abs, QVector<Diag> *rows) {
        Document *d = m_editors && m_lsp ? m_editors->documentForPath(abs) : nullptr;
        if (!d || m_lsp->diagnosticsVersion(d) < 0)
            return false;
        for (const LspDiagnostic &x : m_lsp->diagnostics(abs))
            rows->append(toDiag(abs, x));
        return true;
    };
    hooks.checksFile = [this](const QString &abs) {
        Document *d = m_editors && m_lsp ? m_editors->documentForPath(abs) : nullptr;
        if (!d || !m_lsp->tracks(d))
            return false;
        if (m_lsp->isServed(d))
            return true;
        // Not open on its server yet: only worth a wait while the server is starting (a missing one never answers).
        const LspServerSpec *spec = LspServers::forFile(abs);
        for (const LspManager::ServerState &s : m_lsp->servers(false))
            if (spec && s.id == spec->id)
                return s.status == LspManager::Status::Starting;
        return false;
    };
    hooks.ide = [this](const QString &action, const QJsonObject &args) {
        return m_ide ? m_ide(action, args) : QJsonObject{{"error", "the IDE cannot be controlled right now"}};
    };
    hooks.codeSearch = [this](const QString &query, const QString &scope, int max, QString *note) {
        QHash<QString, QString> unsaved; // the index reads the disk; edits still in the tabs are searched as they are
        if (m_editors)
            for (const QString &p : m_editors->openFilePaths()) {
                Document *d = m_editors->documentForPath(p);
                if (d && d->isModified() && Platform::isUnder(p, m_root) && !AiEnv::isSensitive(p))
                    unsaved.insert(relative(p), d->text());
            }
        return m_index->search(query, scope, max, unsaved, note);
    };
    m_tools.setHooks(hooks);

    // Undo / redo reach the files through the editor tabs, so open documents stay in sync with the disk.
    FileAccess io;
    io.read = [this](const QString &abs, QString *text) {
        if (m_editors)
            if (Document *d = m_editors->documentForPath(abs)) {
                *text = d->text();
                return true;
            }
        QFile f(abs);
        if (!QFileInfo(abs).isFile() || !f.open(QIODevice::ReadOnly))
            return false;
        *text = QString::fromUtf8(f.readAll());
        return true;
    };
    io.write = [this](const QString &abs, const QString &text) { return writeText(abs, text); };
    io.remove = [this](const QString &abs) {
        if (m_editors)
            m_editors->closeDocumentsUnder(abs);
        LocalHistory::recordDisk(abs, LocalHistory::Kind::BeforeDelete);
        return QFile::remove(abs);
    };
    io.trash = [this](const QString &abs, QString *inTrash) {
        if (m_editors)
            m_editors->closeDocumentsUnder(abs);
        LocalHistory::recordDisk(abs, LocalHistory::Kind::BeforeDelete);
        return QFile::moveToTrash(abs, inTrash);
    };
    io.rename = [this](const QString &from, const QString &to) {
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (!QDir().rename(from, to))
            return false;
        LocalHistory::pathMoved(from, to);
        if (m_editors)
            m_editors->pathRenamed(from, to);
        return true;
    };
    m_journal.setAccess(io);
    m_tools.setJournal(&m_journal);

    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AgentPanel::applyTheme);

    applyTheme();
    setBusy(false);
    updateNotice();
    rebuild();
}

void AgentPanel::setEditors(EditorManager *editors)
{
    m_editors = editors;
    connect(editors, &EditorManager::currentChanged, this, &AgentPanel::updateContext);
    connect(editors, &EditorManager::documentSaved, this, [this](Document *doc) {
        if (!doc->isUntitled())
            m_index->fileChanged(doc->filePath());
    });
    updateContext();
}

void AgentPanel::editApiKey()
{
    editApiKeyFor(provider());
}

void AgentPanel::editApiKeyFor(AiEnv::Provider p)
{
    AiKeyDialog dlg(p, this);
    dlg.exec();
    updateNotice();
    if (p == AiEnv::Provider::Gemini)
        m_index->keyChanged();
}

void AgentPanel::focusInput()
{
    updateNotice();
    if (!m_busy) { // the model may have been changed in another window; not to another provider mid-chat
        const QString global = SettingsManager::instance().aiModel();
        if (m_session.messages.isEmpty() || AiEnv::providerForModel(global) == provider())
            setModel(global);
    }
    m_input->setFocus();
}

QString AgentPanel::relative(const QString &absolute) const
{
    return m_root.isEmpty() ? absolute : QDir(m_root).relativeFilePath(absolute);
}

void AgentPanel::setModel(const QString &name)
{
    const QString m = name.trimmed();
    if (m.isEmpty())
        return;
    m_modelName = m;
    SettingsManager::instance().setAiModel(m);
    updateModelButton();
    if (m_notice) // the provider, and with it the key, may have changed
        updateNotice();
}

void AgentPanel::chooseModel(const QString &name)
{
    const QString m = name.trimmed();
    if (m.isEmpty() || m == m_modelName)
        return;
    const AiEnv::Provider from = provider(), to = AiEnv::providerForModel(m);
    // The two providers keep different histories (Claude's thinking blocks are bound to its own conversation), so a
    // chat never moves between them.
    if (to == from || m_session.messages.isEmpty()) {
        setModel(m);
        return;
    }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Information);
    box.setWindowTitle(tr("New chat required"));
    box.setText(tr("Switching from %1 to %2 requires a new chat.").arg(AiEnv::providerName(from), AiEnv::providerName(to)));
    box.setInformativeText(tr("A chat stays with the provider it was started with. This chat is kept in the chat history, "
                              "where you can go back to it."));
    QPushButton *start = box.addButton(tr("Start New Chat"), QMessageBox::AcceptRole);
    box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(start);
    box.exec();
    if (box.clickedButton() != start)
        return;
    setModel(m);
    startNewChat();
}

void AgentPanel::followChatProvider()
{
    if (!m_session.messages.isEmpty() && !m_session.model.trimmed().isEmpty()
        && AiEnv::providerForModel(m_session.model) != provider())
        setModel(m_session.model);
}

void AgentPanel::updateModelButton()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    QString text = modelLabel(m_modelName);
    if (provider() == AiEnv::Provider::Claude && !ClaudeClient::effortFor(m_modelName, SettingsManager::instance().aiEffort()).isEmpty())
        text += QStringLiteral("  ·  ") + ClaudeClient::effortFor(m_modelName, SettingsManager::instance().aiEffort());
    m_modelText->setText(text);
    m_modelIcon->setPixmap(Icons::pixmap(providerIcon(provider()), providerColor(t, provider()), 16));
}

void AgentPanel::rebuildModelMenu()
{
    m_modelMenu->clear();
    m_modelMenu->setMinimumWidth(m_modelBtn->width());
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    auto *group = new QActionGroup(m_modelMenu);
    bool known = false;
    auto addModels = [&](AiEnv::Provider p, const ModelInfo *models, int count) {
        const QIcon icon = Icons::tinted(providerIcon(p), providerColor(t, p));
        m_modelMenu->addSection(AiEnv::providerName(p));
        for (int i = 0; i < count; ++i) {
            const QString id = QString::fromLatin1(models[i].id);
            QString label = QString::fromLatin1(models[i].label);
            if (*models[i].hint)
                label += QStringLiteral("  —  ") + QString::fromLatin1(models[i].hint);
            QAction *a = m_modelMenu->addAction(icon, label);
            a->setToolTip(id);
            a->setCheckable(true);
            a->setActionGroup(group);
            a->setChecked(id == m_modelName);
            if (id == m_modelName) { // the icon takes the place of the check mark
                QFont f = a->font();
                f.setBold(true);
                a->setFont(f);
            }
            known = known || id == m_modelName;
            connect(a, &QAction::triggered, this, [this, id] { chooseModel(id); });
        }
    };
    addModels(AiEnv::Provider::Claude, kClaudeModels, int(std::size(kClaudeModels)));
    addModels(AiEnv::Provider::Gemini, kGeminiModels, int(std::size(kGeminiModels)));
    if (!known) { // a typed-in model id
        m_modelMenu->addSeparator();
        QAction *a = m_modelMenu->addAction(Icons::tinted(providerIcon(provider()), providerColor(t, provider())), m_modelName);
        a->setCheckable(true);
        a->setActionGroup(group);
        a->setChecked(true);
        QFont f = a->font();
        f.setBold(true);
        a->setFont(f);
    }

    if (provider() == AiEnv::Provider::Claude) {
        const QString current = ClaudeClient::effortFor(m_modelName, SettingsManager::instance().aiEffort());
        if (!current.isEmpty()) {
            m_modelMenu->addSeparator();
            QMenu *effort = m_modelMenu->addMenu(tr("Effort: %1").arg(current));
            effort->setToolTipsVisible(true);
            auto *eg = new QActionGroup(effort);
            for (const auto &e : kEfforts) {
                const QString id = QString::fromLatin1(e.id);
                const QString accepted = ClaudeClient::effortFor(m_modelName, id);
                QAction *a = effort->addAction(tr(e.label));
                a->setCheckable(true);
                a->setActionGroup(eg);
                a->setChecked(id == current);
                a->setEnabled(accepted == id); // older models do not know every level
                connect(a, &QAction::triggered, this, [this, id] {
                    SettingsManager::instance().setAiEffort(id);
                    updateModelButton();
                });
            }
        }
    }

    m_modelMenu->addSeparator();
    auto *wa = new QWidgetAction(m_modelMenu);
    auto *holder = new QWidget;
    auto *hl = new QHBoxLayout(holder);
    hl->setContentsMargins(6, 4, 6, 4);
    m_customModel = new QLineEdit(holder);
    m_customModel->setObjectName(QStringLiteral("aiCustomModel"));
    m_customModel->setPlaceholderText(tr("Other model id (claude-… or gemini-…), then Enter"));
    m_customModel->setClearButtonEnabled(true);
    hl->addWidget(m_customModel);
    wa->setDefaultWidget(holder);
    m_modelMenu->addAction(wa);
    connect(m_customModel, &QLineEdit::returnPressed, this, [this] {
        const QString id = m_customModel->text();
        m_modelMenu->close();
        chooseModel(id);
    });
}

// --- codebase index -----------------------------------------------------------------------------------------------

void AgentPanel::updateIndexButton()
{
    const CodeIndex::Status st = m_index->status();
    QString tip = tr("Codebase index: lets QODE find code by what it does");
    if (m_root.isEmpty())
        tip += tr("\nOpen a project to index it.");
    else if (st.building)
        tip += tr("\nIndexing the project…");
    else
        tip += tr("\n%1 files, %2 sections").arg(QLocale().toString(st.files), QLocale().toString(st.sections));
    if (st.embeddingsOn && st.hasKey && st.sections > 0 && !st.building)
        tip += tr("\nMeaning: %1 of %2 sections").arg(QLocale().toString(st.embedded), QLocale().toString(st.sections));
    m_indexBtn->setToolTip(tip);
}

void AgentPanel::rebuildIndexMenu()
{
    QMenu *menu = m_indexBtn->menu();
    menu->clear();
    const CodeIndex::Status st = m_index->status();
    auto info = [menu](const QString &text) { menu->addAction(text)->setEnabled(false); };
    menu->addSection(tr("Codebase index"));
    if (m_root.isEmpty()) {
        info(tr("Open a project to index it"));
        return;
    }
    info(st.building ? tr("Indexing the project…")
                     : tr("%1 files, %2 sections indexed").arg(QLocale().toString(st.files), QLocale().toString(st.sections)));
    if (!st.embeddingsOn)
        info(tr("Keyword ranking only"));
    else if (!st.hasKey)
        info(tr("Meaning search needs a Gemini API key"));
    else if (!st.error.isEmpty())
        info(st.error.left(120));
    else if (st.building || st.embedded < st.sections)
        info(tr("Meaning: %1 of %2 sections embedded…").arg(QLocale().toString(st.embedded), QLocale().toString(st.sections)));
    else
        info(tr("Meaning search ready"));
    menu->addSeparator();
    QAction *meaning = menu->addAction(tr("Search by meaning with Gemini embeddings"));
    meaning->setCheckable(true);
    meaning->setChecked(st.embeddingsOn);
    meaning->setToolTip(tr("Sends the project's code sections (never secret files) to Google's embedding API once; "
                           "afterwards only new or changed sections are sent. Off: keyword ranking only, nothing leaves this computer."));
    connect(meaning, &QAction::toggled, this, [this](bool on) { m_index->setEmbeddingsEnabled(on); });
    if (st.embeddingsOn && !st.hasKey)
        menu->addAction(tr("Gemini API key…"), this, [this] { editApiKeyFor(AiEnv::Provider::Gemini); });
    menu->addAction(tr("Rebuild index"), this, [this] { m_index->rebuild(); });
}

// --- project state ------------------------------------------------------------------------------------------------

void AgentPanel::setProject(const QString &root)
{
    if (root == m_root)
        return;
    abandonRun();
    saveSession();
    m_root = root;
    m_files.clear();
    m_fileSet.clear();
    m_allFiles.clear();
    m_alwaysAllow.clear();
    m_store.setDirectory(root.isEmpty() ? QString() : SettingsManager::instance().projectDataDir());
    if (!root.isEmpty()) {
        m_tools.setRoot(root);
        m_journal.setRoot(root);
    }
    m_mcp->setProject(root, root.isEmpty() ? QString() : SettingsManager::instance().projectDataDir());
    const QString data = SettingsManager::instance().projectDataDir();
    m_index->setProject(root, root.isEmpty() || data.isEmpty() ? QString()
                                                               : AppPaths::cache(QStringLiteral("code-index/")) + QFileInfo(data).fileName() + QStringLiteral(".vec"));
    const auto sessions = m_store.list();
    Session loaded;
    if (!sessions.isEmpty() && m_store.load(sessions.first().id, &loaded))
        m_session = loaded;
    else
        m_session = Session::create(SettingsManager::instance().aiModel());
    followChatProvider();
    setQuote(QString());
    loadJournal();
    setBusy(false);
    updateNotice();
    updateContext();
    rebuild();
}

void AgentPanel::setGitRepo(bool isRepo)
{
    if (m_gitOk == isRepo)
        return;
    m_gitOk = isRepo;
    updateNotice();
}

void AgentPanel::setProjectFiles(const QStringList &absolutePaths)
{
    m_allFiles = absolutePaths;
    m_index->setFiles(absolutePaths);
    m_files.clear();
    m_fileSet.clear();
    m_files.reserve(absolutePaths.size());
    for (const QString &p : absolutePaths) {
        if (looksBinary(p) || AiEnv::isSensitive(p))
            continue;
        const QString rel = relative(p);
        if (rel.startsWith(QLatin1String("..")))
            continue;
        m_files << rel;
        m_fileSet.insert(rel);
    }
}

void AgentPanel::updateNotice()
{
    m_keyOk = !m_root.isEmpty() && !AiEnv::apiKey(provider(), m_root).isEmpty();
    QString text, button;
    if (m_root.isEmpty()) {
        text = tr("Open a project folder to talk to QODE.");
    } else if (!m_gitOk) {
        text = tr("<b>QODE needs a git repository.</b> It edits many files on its own; git lets you review and revert everything it does.");
        button = tr("Initialize Git repository");
    } else if (!m_keyOk) {
        text = provider() == AiEnv::Provider::Claude
                   ? tr("<b>No Claude API key yet.</b> Add your own key from the Claude Console to use Claude models. It stays on this computer.")
                   : tr("<b>No Gemini API key yet.</b> Add your own Google AI Studio key to use Gemini models. It stays on this computer.");
        button = tr("Enter API key…");
    }
    m_notice->setVisible(!text.isEmpty());
    m_noticeText->setText(text);
    m_noticeBtn->setText(button);
    m_noticeBtn->setVisible(!button.isEmpty());
    const bool usable = text.isEmpty();
    m_input->setEnabled(usable);
    m_send->setEnabled(usable || m_busy);
    m_modelBtn->setEnabled(!m_busy);
}

void AgentPanel::updateContext()
{
    if (m_busy) {
        m_context->setText(m_runClock.isValid() ? tr("%1 · %2").arg(m_status, formatElapsed(m_runClock.elapsed())) : m_status);
        m_context->setToolTip(QString());
        return;
    }
    const QString f = focusedFile();
    if (f.isEmpty()) {
        m_context->setText(tr("No file open"));
        return;
    }
    const QString text = tr("Active file: %1").arg(relative(f));
    m_context->setText(QFontMetrics(m_context->font()).elidedText(text, Qt::ElideMiddle, qMax(120, m_context->width() - 4)));
    m_context->setToolTip(text);
}

QString AgentPanel::focusedFile() const
{
    if (!m_editors || m_root.isEmpty())
        return {};
    Document *d = m_editors->currentDocument();
    if (!d || d->isUntitled())
        return {};
    const QString p = d->filePath();
    return Platform::isUnder(p, m_root) && !AiEnv::isSensitive(p) ? p : QString();
}

// --- sessions -----------------------------------------------------------------------------------------------------

void AgentPanel::saveSession()
{
    if (m_session.messages.isEmpty() || !m_store.valid())
        return;
    m_session.updated = QDateTime::currentDateTime();
    m_store.save(m_session);
}

void AgentPanel::startNewChat()
{
    abandonRun();
    saveSession();
    m_session = Session::create(SettingsManager::instance().aiModel());
    m_alwaysAllow.clear();
    setQuote(QString());
    loadJournal();
    setBusy(false);
    rebuild();
    m_input->setFocus();
}

void AgentPanel::loadSession(const QString &id)
{
    Session s;
    if (!m_store.load(id, &s))
        return;
    abandonRun();
    saveSession();
    m_session = s;
    followChatProvider();
    m_alwaysAllow.clear();
    setQuote(QString());
    loadJournal();
    setBusy(false);
    rebuild();
}

void AgentPanel::deleteCurrentChat()
{
    abandonRun();
    m_store.remove(m_session.id);
    const auto sessions = m_store.list();
    Session next;
    if (!sessions.isEmpty() && m_store.load(sessions.first().id, &next))
        m_session = next;
    else
        m_session = Session::create(SettingsManager::instance().aiModel());
    followChatProvider();
    setQuote(QString());
    loadJournal();
    setBusy(false);
    rebuild();
}

void AgentPanel::loadJournal()
{
    m_journal.clear();
    if (!m_store.valid())
        return;
    QFile f(m_store.journalPath(m_session.id));
    if (f.open(QIODevice::ReadOnly))
        m_journal.load(QJsonDocument::fromJson(f.readAll()).object());
}

void AgentPanel::saveJournal()
{
    if (!m_store.valid())
        return;
    const QString path = m_store.journalPath(m_session.id);
    if (m_journal.isEmpty() && !m_journal.canRedo()) {
        QFile::remove(path);
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(m_journal.toJson()).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

void AgentPanel::rebuildHistoryMenu()
{
    QMenu *menu = m_historyBtn->menu();
    menu->clear();
    menu->addAction(tr("New chat"), this, &AgentPanel::startNewChat);
    menu->addSeparator();
    QAction *undoLast = menu->addAction(tr("Undo QODE's last changes"), this, [this] { undoFromUi(Journal::Last, tr("QODE's last changes")); });
    QAction *undoAll = menu->addAction(tr("Undo all of QODE's changes in this chat"), this, [this] { undoFromUi(Journal::All, tr("all of QODE's changes in this chat")); });
    QAction *redo = menu->addAction(tr("Redo undone changes"), this, &AgentPanel::redoFromUi);
    undoLast->setEnabled(!m_busy && !m_journal.isEmpty());
    undoAll->setEnabled(!m_busy && !m_journal.isEmpty());
    redo->setEnabled(!m_busy && m_journal.canRedo());
    const auto sessions = m_store.list();
    if (!sessions.isEmpty())
        menu->addSeparator();
    for (const Store::Info &s : sessions) {
        const QString title = s.title.isEmpty() ? tr("(untitled)") : s.title;
        QAction *a = menu->addAction(QStringLiteral("%1   ·   %2").arg(title.left(48), QLocale().toString(s.updated, QLocale::ShortFormat)));
        a->setCheckable(true);
        a->setChecked(s.id == m_session.id);
        const QString id = s.id;
        connect(a, &QAction::triggered, this, [this, id] { loadSession(id); });
    }
    if (!m_session.messages.isEmpty()) {
        menu->addSeparator();
        menu->addAction(tr("Delete this chat"), this, &AgentPanel::deleteCurrentChat);
    }
}

// --- running ------------------------------------------------------------------------------------------------------

QStringList AgentPanel::mentionedFiles(const QString &prompt) const
{
    static const QRegularExpression re(QStringLiteral("(?:^|\\s)@([^\\s@]+)"));
    QStringList out;
    auto it = re.globalMatch(prompt);
    while (it.hasNext()) {
        QString p = it.next().captured(1);
        while (!p.isEmpty() && !m_fileSet.contains(p) && QStringLiteral(".,;:)!?").contains(p.back()))
            p.chop(1);
        if (m_fileSet.contains(p) && !out.contains(p))
            out << p;
    }
    return out;
}

// What the model learns about the editor with each request (not stored in the chat).
QString AgentPanel::contextFor(const QStringList &mentioned) const
{
    QStringList lines;
    const QString focused = focusedFile();
    if (!focused.isEmpty())
        lines << QStringLiteral("Active file in the editor: %1").arg(relative(focused));
    QStringList tabs;
    if (m_editors)
        for (const QString &p : m_editors->openFilePaths())
            if (Platform::isUnder(p, m_root) && !AiEnv::isSensitive(p))
                tabs << relative(p);
    if (!tabs.isEmpty())
        lines << QStringLiteral("Open tabs: %1").arg(tabs.mid(0, 20).join(QStringLiteral(", ")));
    if (!mentioned.isEmpty())
        lines << QStringLiteral("The user pointed at these files: %1").arg(mentioned.join(QStringLiteral(", ")));
    return lines.isEmpty() ? QString() : QStringLiteral("[Editor context]\n%1\n\n[Request]\n").arg(lines.join(QLatin1Char('\n')));
}

void AgentPanel::send()
{
    if (m_busy)
        return;
    const QString prompt = m_input->toPlainText().trimmed();
    if (prompt.isEmpty())
        return;
    updateNotice();
    if (!m_input->isEnabled())
        return;
    if (prompt.startsWith(QLatin1Char('/')) && runSlash(prompt)) {
        m_input->clear();
        return;
    }
    const QString quote = m_quote->isHidden() ? QString() : m_quote->text();
    const QString lead = m_quoteLead;
    setQuote(QString());
    startRun(prompt, prompt, mentionedFiles(prompt), quote, lead);
}

void AgentPanel::askAbout(const QString &text)
{
    updateNotice();
    if (!m_input->isEnabled())
        return;
    if (m_busy) { // asked when the turn is over: ready to send
        setQuote(text);
        m_input->setPlainText(AiQuote::askPrompt());
        m_input->moveCursor(QTextCursor::End);
        m_input->setFocus();
        return;
    }
    const QString draft = m_input->toPlainText();
    startRun(AiQuote::askPrompt(), AiQuote::askPrompt(), {}, text);
    m_input->setPlainText(draft);
    m_input->moveCursor(QTextCursor::End);
}

void AgentPanel::startFix(const QString &display, const QString &request, const QStringList &files, const QString &quote, const QString &quoteLead)
{
    updateNotice();
    const QString shown = maskSecrets(quote); // terminal output can hold a token or a password
    if (m_busy || !m_input->isEnabled()) { // after this turn (or once a key / git is there): ready to send
        QString draft = display;
        for (const QString &f : files)
            draft += QStringLiteral(" @") + f;
        setQuote(shown);
        m_quoteLead = quoteLead;
        m_input->setPlainText(draft);
        m_input->moveCursor(QTextCursor::End);
        m_input->setFocus();
        return;
    }
    if (!m_session.messages.isEmpty())
        startNewChat();
    startRun(display, maskSecrets(request), files, shown, quoteLead);
}

void AgentPanel::setQuote(const QString &text)
{
    m_quoteLead.clear();
    m_quote->setText(text);
    m_quote->setVisible(!text.isEmpty());
    if (!text.isEmpty())
        m_input->setFocus();
}

// Before each run: the tool groups the project offers now (the window decides: a web preview, servers, .http files, a
// JVM debugger) added to the ones the chat already declared. Dropping a group would change the declarations, which
// are part of the cached prefix, so a chat's groups only grow.
void AgentPanel::prepareToolGroups()
{
    QStringList groups = m_session.toolGroups;
    if (m_ide)
        for (const QJsonValue &v : m_ide(QStringLiteral("tool_groups"), {}).value("groups").toArray())
            if (!groups.contains(v.toString()))
                groups.append(v.toString());
    QStringList ordered; // the fixed order, so the same set always gives the same declarations
    for (const QString &g : AgentTools::allGroups())
        if (groups.contains(g))
            ordered.append(g);
    m_session.toolGroups = ordered;
    m_tools.setGroups(ordered);
    if (qEnvironmentVariableIsSet("QODE_AGENT_DUMP_TOOLS")) {
        const QByteArray gemini = QJsonDocument(m_tools.toolsJson()).toJson(QJsonDocument::Compact);
        const QByteArray claude = QJsonDocument(m_tools.claudeToolsJson()).toJson(QJsonDocument::Compact);
        qInfo().noquote() << QStringLiteral("tool groups: [%1]; declarations: Gemini %2 chars (~%3 tokens), Claude %4 chars; prompt sections %5 chars")
                                 .arg(ordered.join(QStringLiteral(", ")))
                                 .arg(gemini.size())
                                 .arg(gemini.size() / 4)
                                 .arg(claude.size())
                                 .arg(m_tools.groupPrompt().size());
    }
}

// Runs one request. `display` is what the chat shows as the user's message, `request` what the model is asked.
void AgentPanel::startRun(const QString &display, const QString &request, const QStringList &mentioned, const QString &quote,
                          const QString &quoteLead)
{
    const int turn = int(m_session.messages.size());
    const QString sent = contextFor(mentioned) + (quote.isEmpty() ? QString() : AiQuote::block(quote, quoteLead) + QStringLiteral("\n\n")) + request;

    AgentRun::Config cfg;
    cfg.provider = provider();
    cfg.effort = SettingsManager::instance().aiEffort();
    cfg.apiKey = AiEnv::apiKey(provider(), m_root);
    cfg.model = m_modelName;
    prepareToolGroups();
    cfg.systemInstruction = AgentRun::systemPrompt(m_root) + m_tools.groupPrompt() + m_mcp->promptSection();
    prepareMcpTools();
    cfg.contents = m_session.history();
    cfg.contents.append(QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", sent}}}}});

    Message user;
    user.role = QStringLiteral("user");
    user.text = display;
    user.sent = sent; // replayed as it was, so the start of every later request stays identical and can be cached
    user.files = mentioned;
    user.quote = quote;
    user.time = QDateTime::currentDateTime();
    user.turn = turn;
    m_session.messages.append(user);
    m_session.model = m_modelName;
    if (m_session.title.isEmpty())
        m_session.title = display.simplified().left(60);

    m_live = Message();
    m_liveRequest = RequestStats();
    m_live.role = QStringLiteral("assistant");
    m_live.model = m_modelName;
    m_live.time = QDateTime::currentDateTime();
    m_live.turn = turn;
    m_runClock.start();

    m_input->clear();
    m_stick = true;
    m_status = tr("Thinking…");
    setBusy(true);
    rebuild(); // the new user bubble, then the live turn
    m_liveView = new AgentTurnView(m_modelName, m_list);
    if (m_lastTurn)
        m_lastTurn->setCopyPinned(false);
    m_lastTurn = m_liveView;
    m_liveView->setCopyPinned(true);
    wireTurn(m_liveView);
    m_listLayout->insertWidget(m_listLayout->count() - 1, m_liveView);

    m_tools.resetCancel();
    m_tools.setTurn(turn);
    m_run = new AgentRun(&m_tools, this);
    AgentRun *run = m_run;
    connect(run, &AgentRun::thoughtDelta, this, [this](const QString &t) {
        if (m_live.blocks.isEmpty() || m_live.blocks.last().kind != Block::Thought) {
            Block b;
            b.kind = Block::Thought;
            m_live.blocks.append(b);
        }
        m_live.blocks.last().text += t;
        m_liveView->appendThought(t);
    });
    connect(run, &AgentRun::textDelta, this, [this](const QString &t) {
        if (m_live.blocks.isEmpty() || m_live.blocks.last().kind != Block::Text) {
            Block b;
            b.kind = Block::Text;
            m_live.blocks.append(b);
        }
        m_live.blocks.last().text += t;
        m_liveView->appendText(t);
    });
    connect(run, &AgentRun::toolStarted, this, [this](const QString &name, const QString &call) {
        Block b;
        b.kind = Block::Tool;
        b.name = name;
        b.text = call;
        m_live.blocks.append(b);
        m_liveView->addTool(name, call);
    });
    connect(run, &AgentRun::toolFinished, this, [this](const QString &, const ToolResult &r) {
        for (int i = m_live.blocks.size() - 1; i >= 0; --i) {
            Block &b = m_live.blocks[i];
            if (b.kind == Block::Tool && !b.done) {
                b.done = true;
                b.ok = r.ok;
                b.summary = r.summary;
                b.hasChange = r.hasChange && r.ok;
                if (b.hasChange)
                    b.change = r.change;
                break;
            }
        }
        m_liveView->finishTool(r);
    });
    connect(run, &AgentRun::status, this, [this](const QString &s) {
        m_status = s;
        updateContext();
    });
    connect(run, &AgentRun::usage, this, [this](const AgentRun::Usage &u) {
        m_live.requests = u.requests;
        m_live.promptTokens = u.prompt;
        m_live.cachedTokens = u.cached;
        m_live.outputTokens = u.output;
        m_live.contextTokens = u.context;
    });
    connect(run, &AgentRun::requestProgress, this, [this](const RequestStats &r) {
        if (r.done)
            m_live.requestLog.append(r);
        m_liveRequest = r.done ? RequestStats() : r;
        if (m_liveView)
            m_liveView->setLiveStats(m_live.requestLog, r, !r.done, m_runClock.elapsed());
        updateUsage();
    });
    connect(run, &AgentRun::finished, this, [this] { finishRun(QString()); });
    connect(run, &AgentRun::failed, this, [this](const QString &err) { finishRun(err); });
    run->start(cfg);
}

// --- slash commands -----------------------------------------------------------------------------------------------

namespace {
struct SlashCommand {
    const char *name, *help;
};
const SlashCommand kSlash[] = {{"/init", "create AGENTS.md: a summary of the project for agents"},
                               {"/compact", "summarize this chat to free up context"},
                               {"/undo", "undo the agent's last changes (/undo all: everything)"},
                               {"/redo", "bring back what was undone"},
                               {"/mcp", "MCP servers: status and tools (manage them in View > MCP Servers)"}};

const char *kInitPrompt =
    "Create the file AGENTS.md in the project root: a briefing that future coding agents read before they touch this project. "
    "First explore: directory_tree, README, the build files (package.json, build.gradle*, pom.xml, CMakeLists.txt, *.pro, Cargo.toml, "
    "pyproject.toml, Makefile ... whatever exists) and a few central source files. Then write the file with create_file. Contents, in this order and in "
    "plain Markdown: (1) one paragraph: what the project is and does; (2) tech stack: language(s), frameworks, key libraries, versions that matter; "
    "(3) layout: the important folders and files and what lives where; (4) how to build, run and test, using only commands you saw in the build files or "
    "README; (5) code conventions you can actually observe (naming, formatting, patterns, where new code goes); (6) pitfalls and things not to touch. "
    "Only state what you verified in the files; leave a section out rather than guess. Keep it under about 120 lines. Do not change any other file. "
    "Finish with two sentences on what you wrote.";
} // namespace

void AgentPanel::recallPrompt(int delta)
{
    if (m_busy)
        return;
    QStringList prompts;
    for (const Message &m : std::as_const(m_session.messages))
        if (m.role == QLatin1String("user") && !m.text.trimmed().isEmpty())
            prompts << m.text.trimmed();
    if (!m_input->recalling())
        m_recallPos = int(prompts.size()); // a fresh start: Up goes to the newest
    m_recallPos = qBound(0, m_recallPos + delta, int(prompts.size()));
    m_input->setRecalled(m_recallPos < prompts.size() ? prompts.at(m_recallPos) : QString());
}

void AgentPanel::updateSlash()
{
    const QString text = m_input->toPlainText();
    // A recalled command stays plain text: Up / Down go on recalling instead of stepping the list.
    if (!text.startsWith(QLatin1Char('/')) || text.contains(QLatin1Char(' ')) || text.contains(QLatin1Char('\n')) || !m_input->isEnabled() || m_busy ||
        m_input->recalling()) {
        if (m_slash)
            closeMentions();
        return;
    }
    QStringList items;
    for (const SlashCommand &c : kSlash)
        if (QLatin1String(c.name).startsWith(text, Qt::CaseInsensitive))
            items << QStringLiteral("%1   %2").arg(QLatin1String(c.name), QLatin1String(c.help));
    if (items.isEmpty()) {
        if (m_slash)
            closeMentions();
        return;
    }
    m_slash = true;
    m_popup->setItems(items);
    m_input->setMentionOpen(true);
    m_popup->showAbove(m_inputBox);
}

// Handles /init, /compact, /undo, /redo, /mcp. False for anything else, which is then an ordinary prompt.
bool AgentPanel::runSlash(const QString &prompt)
{
    const QString cmd = prompt.section(QRegularExpression(QStringLiteral("\\s")), 0, 0).toLower();
    const QString arg = prompt.mid(cmd.size()).trimmed().toLower();
    if (cmd == QLatin1String("/init")) {
        if (QFileInfo::exists(m_root + QStringLiteral("/AGENTS.md")))
            addNote(tr("AGENTS.md already exists in this project, so /init left it alone. Ask QODE to update it if it is out of date."));
        else
            startRun(QStringLiteral("/init"), QString::fromLatin1(kInitPrompt), {});
        return true;
    }
    if (cmd == QLatin1String("/compact")) {
        compact();
        return true;
    }
    if (cmd == QLatin1String("/undo")) {
        const bool all = arg == QLatin1String("all");
        undoFromUi(all ? int(Journal::All) : int(Journal::Last), all ? tr("all of QODE's changes in this chat") : tr("QODE's last changes"));
        return true;
    }
    if (cmd == QLatin1String("/redo")) {
        redoFromUi();
        return true;
    }
    if (cmd == QLatin1String("/mcp")) {
        Message m;
        m.role = QStringLiteral("mcp"); // shown as the live card; not part of what the model gets
        m.time = QDateTime::currentDateTime();
        m_session.messages.append(m);
        saveSession();
        rebuild();
        return true;
    }
    return false;
}

void AgentPanel::addNote(const QString &text)
{
    Message m;
    m.role = QStringLiteral("note");
    m.text = text;
    m.time = QDateTime::currentDateTime();
    m_session.messages.append(m);
    saveSession();
    rebuild();
}

// --- compacting ---------------------------------------------------------------------------------------------------

void AgentPanel::compact()
{
    if (m_busy)
        return;
    const QJsonArray history = m_session.history();
    if (history.size() < 2) {
        addNote(tr("There is nothing to compact yet."));
        return;
    }
    QJsonArray contents = history;
    contents.append(QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text",
        "Write the summary now, as instructed."}}}}});

    const QString system = QStringLiteral(
        "You compact a conversation between a user and a coding agent so the work can continue from the summary alone. Write the summary for the "
        "agent itself, not for the user. Cover: the user's goals and requests; decisions, constraints and preferences they stated; facts learned about "
        "the project (stack, layout, commands, conventions); every file created, changed, moved or deleted, and why; commands run and what they showed; "
        "problems still open and the next steps. Be dense and factual, use short bullet lists, no filler, at most 700 words. Keep exact file paths, "
        "names and commands.");

    m_compacting = true;
    m_status = tr("Compacting the conversation…");
    setBusy(true);
    const int gen = ++m_compactGen;
    // Both clients answer with the same fields.
    auto done = [this, gen](bool ok, const QString &text, const QString &error, int promptTokens, int outputTokens) {
        if (gen != m_compactGen)
            return; // the chat was closed or switched meanwhile
        m_compacting = false;
        Message m;
        m.time = QDateTime::currentDateTime();
        if (ok) {
            m.role = QStringLiteral("summary");
            m.text = text.trimmed();
            m.model = m_modelName;
            m.promptTokens = promptTokens;
            m.outputTokens = outputTokens;
        } else {
            m.role = QStringLiteral("error");
            m.text = error == QLatin1String("Cancelled") ? tr("Stopped.") : error;
        }
        m_session.messages.append(m);
        setBusy(false);
        saveSession();
        rebuild();
    };
    if (provider() == AiEnv::Provider::Claude) {
        ClaudeClient::Request req;
        req.apiKey = AiEnv::apiKey(provider(), m_root);
        req.model = m_modelName;
        req.contents = contents;
        req.system = system;
        m_claude->generate(req, [done](const ClaudeClient::Result &r) { done(r.ok, r.text, r.error, r.promptTokens, r.outputTokens); });
        return;
    }
    GeminiClient::Request req;
    req.apiKey = AiEnv::apiKey(provider(), m_root);
    req.model = m_modelName;
    req.contents = contents;
    req.systemInstruction = system;
    req.thinkingBudget = 512;
    m_gemini->generate(req, [done](const GeminiClient::Result &r) { done(r.ok, r.text, r.error, r.promptTokens, r.outputTokens + r.thoughtTokens); });
}

// --- undo / redo --------------------------------------------------------------------------------------------------

// Asks, then reverts. Files the user changed after the agent are listed and only overwritten on request.
void AgentPanel::undoFromUi(int turn, const QString &what)
{
    if (m_busy)
        return;
    if (turn == Journal::Last && m_journal.lastTurn() < 0) {
        addNote(tr("QODE has not changed anything in this chat (or it was already undone)."));
        return;
    }
    const auto entries = m_journal.entries(turn);
    if (entries.isEmpty()) {
        addNote(tr("There is nothing to undo."));
        return;
    }
    QStringList lines;
    for (const auto &e : entries)
        lines << QStringLiteral("%1 %2").arg(e.what, e.path);
    if (lines.size() > 14)
        lines = lines.mid(0, 14) + QStringList{tr("… and %1 more").arg(entries.size() - 14)};
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Undo QODE's changes"));
    box.setText(tr("Undo %1?").arg(what));
    box.setInformativeText(lines.join(QLatin1Char('\n')));
    QPushButton *yes = box.addButton(tr("Undo"), QMessageBox::AcceptRole);
    box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.setDefaultButton(yes);
    box.exec();
    if (box.clickedButton() != yes)
        return;

    Journal::Scope scope;
    scope.turn = turn;
    Journal::Result r = m_journal.undo(scope);
    QStringList leftover;
    if (!r.conflicts.isEmpty()) {
        QMessageBox ask(this);
        ask.setIcon(QMessageBox::Warning);
        ask.setWindowTitle(tr("Files changed since"));
        ask.setText(tr("These files were changed after QODE left them. Overwrite them with their earlier version?"));
        ask.setInformativeText(r.conflicts.join(QLatin1Char('\n')));
        QPushButton *force = ask.addButton(tr("Overwrite"), QMessageBox::DestructiveRole);
        ask.addButton(tr("Keep my changes"), QMessageBox::RejectRole);
        ask.exec();
        if (ask.clickedButton() == force) {
            for (const QString &p : std::as_const(r.conflictPaths)) {
                Journal::Scope one = scope;
                one.path = p;
                one.force = true;
                const Journal::Result more = m_journal.undo(one);
                r.done << more.done;
                leftover << more.conflicts;
            }
        } else {
            leftover = r.conflicts;
        }
    }
    saveJournal();
    scheduleProjectChanged();
    QString note = r.done.isEmpty() ? tr("Nothing was undone.") : tr("Undid %1 change(s): %2.").arg(r.done.size()).arg(r.done.join(QStringLiteral(", ")));
    if (!leftover.isEmpty())
        note += tr(" Left alone: %1.").arg(leftover.join(QStringLiteral("; ")));
    addNote(note);
}

void AgentPanel::redoFromUi()
{
    if (m_busy)
        return;
    if (!m_journal.canRedo()) {
        addNote(tr("There is nothing to redo."));
        return;
    }
    const Journal::Result r = m_journal.redo();
    saveJournal();
    scheduleProjectChanged();
    QString note = r.done.isEmpty() ? tr("Nothing was redone.") : tr("Redid %1 change(s): %2.").arg(r.done.size()).arg(r.done.join(QStringLiteral(", ")));
    if (!r.conflicts.isEmpty())
        note += tr(" Not possible: %1.").arg(r.conflicts.join(QStringLiteral("; ")));
    addNote(note);
}

// Replaces a file's whole text: through the editor tab when it is open (so Ctrl+Z still works), saved at once so that
// the disk and the git status match; straight to disk otherwise.
bool AgentPanel::writeText(const QString &abs, const QString &text)
{
    const LocalHistory::Reason reason(LocalHistory::Kind::QodeEdit);
    if (m_editors) {
        if (Document *d = m_editors->documentForPath(abs)) {
            CodeEditor *ed = m_editors->editorFor(d);
            if (ed && !ed->isReadOnly()) {
                LspTextEdit t;
                t.startLine = 0;
                t.startColumn = 0;
                t.endLine = qMax(0, ed->document()->blockCount() - 1);
                t.endColumn = 1 << 28; // past the end: the editor clamps it
                t.text = text;
                if (!ed->applyTextEdits({t}))
                    return false;
                m_editors->saveQuietly(d);
                return true;
            }
        }
    }
    QDir().mkpath(QFileInfo(abs).absolutePath());
    LocalHistory::recordWrite(abs, text.toUtf8());
    QSaveFile f(abs);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(text.toUtf8());
    return f.commit();
}

void AgentPanel::scheduleProjectChanged()
{
    m_changedTimer->start();
}

// --- language server diagnostics -----------------------------------------------------------------------------------

QString AgentPanel::collectDiagnostics(const QString &abs, QVector<Diag> *rows, bool afterEdit, bool settle)
{
    if (!m_lsp || !m_editors)
        return tr("language server integration is not available");
    QStringList paths;
    if (!abs.isEmpty()) {
        if (!m_editors->documentForPath(abs))
            m_editors->openFile(abs); // the server only analyses open files
        paths << abs;
    } else {
        for (const QString &p : m_editors->openFilePaths())
            if (Platform::isUnder(p, m_root))
                paths << p;
    }
    auto served = [&] {
        for (const QString &p : std::as_const(paths))
            if (Document *d = m_editors->documentForPath(p))
                if (m_lsp->isServed(d))
                    return true;
        return false;
    };
    auto wait = [this](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    // A server that was just asked to start needs a moment; after an edit it is known to be starting (the first Kotlin
    // or Java file of a session can take a while).
    QElapsedTimer clock;
    clock.start();
    const int startLimit = afterEdit ? 30000 : 4000;
    while (!served() && clock.elapsed() < startLimit && !m_tools.cancelled())
        wait(150);
    if (!served())
        return tr("no language server is running for %1; use run_command to build or test instead").arg(abs.isEmpty() ? tr("the open files") : QFileInfo(abs).fileName());

    // Send the latest edits now and wait until the server has reported on exactly that text. A report counts even when
    // nothing changed (LspManager::diagnosticsVersion), so the errors of the text before an edit are never passed off as
    // the current ones, and an edit that changes nothing does not wait for a report that never comes.
    QVector<QPair<QString, int>> want; // path, version sent
    for (const QString &p : std::as_const(paths))
        if (Document *d = m_editors->documentForPath(p)) {
            const int version = m_lsp->syncNow(d);
            if (version >= 0)
                want.append({p, version});
        }
    auto fresh = [&] {
        for (const auto &w : std::as_const(want)) {
            Document *d = m_editors->documentForPath(w.first);
            if (d && m_lsp->diagnosticsVersion(d) < w.second)
                return false;
        }
        return true;
    };
    const int limit = afterEdit ? 15000 : 12000;
    clock.restart();
    if (!fresh()) {
        QEventLoop loop;
        QTimer poll;
        connect(m_lsp, &LspManager::diagnosticsReported, &loop, [&] {
            if (fresh())
                loop.quit();
        });
        connect(&poll, &QTimer::timeout, &loop, [&] {
            if (m_tools.cancelled() || clock.elapsed() > limit || fresh())
                loop.quit();
        });
        poll.start(100);
        loop.exec();
    }
    const bool late = !fresh();
    if (settle && !late) {
        // Any report restarts the quiet time; LspManager re-pulls 800 ms after a server's refresh request.
        QEventLoop loop;
        QTimer quiet, cap, poll;
        quiet.setSingleShot(true);
        cap.setSingleShot(true);
        connect(&quiet, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&cap, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(m_lsp, &LspManager::diagnosticsReported, &loop, [&quiet] { quiet.start(1500); });
        connect(&poll, &QTimer::timeout, &loop, [&] {
            if (m_tools.cancelled())
                loop.quit();
        });
        quiet.start(1500);
        cap.start(6000);
        poll.start(100);
        loop.exec();
    }

    for (const QString &p : std::as_const(paths))
        for (const LspDiagnostic &d : m_lsp->diagnostics(p))
            rows->append(toDiag(p, d));
    if (late && !m_tools.cancelled())
        return tr("the language server had not finished checking the latest text after %1 s; these results may be out of date").arg(limit / 1000);
    return {};
}

void AgentPanel::stop()
{
    if (m_compacting) { // reports "Cancelled" through the callback
        m_gemini->cancel();
        m_claude->cancel();
    } else if (m_run)
        m_run->cancel(); // reports "Stopped." through failed()
}

// Drops a run without reporting it (the chat or project it belonged to is going away).
void AgentPanel::abandonRun()
{
    if (m_compacting) {
        ++m_compactGen; // its answer belongs to a chat that is gone
        m_compacting = false;
        m_gemini->cancel();
        m_claude->cancel();
    }
    if (!m_run)
        return;
    AgentRun *run = m_run;
    m_run = nullptr;
    run->disconnect(this);
    run->cancel();
    run->deleteLater();
    m_liveView = nullptr;
    m_live = Message();
}

void AgentPanel::finishRun(const QString &error)
{
    if (m_run)
        m_run->deleteLater();
    m_run = nullptr;
    if (m_liveView)
        m_liveView->endLive();
    m_liveView = nullptr;
    for (Block &b : m_live.blocks)
        b.done = true;
    if (!m_live.blocks.isEmpty()) {
        m_live.time = QDateTime::currentDateTime();
        m_live.ms = m_runClock.elapsed();
        m_session.messages.append(m_live);
    }
    m_live = Message();
    if (!error.isEmpty()) {
        Message e;
        e.role = QStringLiteral("error");
        e.text = error;
        e.time = QDateTime::currentDateTime();
        m_session.messages.append(e);
    }
    setBusy(false);
    saveSession();
    saveJournal();
    scheduleProjectChanged();
    rebuild();
    m_input->setFocus();
}

void AgentPanel::setBusy(bool busy)
{
    if (busy && !m_busy)
        m_runClock.start();
    m_busy = busy;
    if (busy)
        m_clockTimer->start();
    else
        m_clockTimer->stop();
    m_input->setReadOnly(busy);
    m_modelBtn->setEnabled(!busy);
    m_send->setToolTip(busy ? tr("Stop") : tr("Send (Ctrl+Enter)"));
    applyTheme();
    updateNotice();
    updateContext();
}

// The agent wants to do something that needs the user's say: run a command, delete a file.
bool AgentPanel::approve(const QString &tool, const QString &title, const QString &detail)
{
    if (m_alwaysAllow.contains(tool))
        return true;
    QMessageBox box(this);
    box.setIcon(tool == QLatin1String("run_command") || tool == QLatin1String("delete_path") ? QMessageBox::Warning : QMessageBox::Question);
    box.setWindowTitle(tr("QODE request"));
    box.setText(tr("QODE wants to: <b>%1</b>").arg(title.toHtmlEscaped()));
    box.setInformativeText(detail.left(1500));
    QPushButton *once = box.addButton(tr("Allow once"), QMessageBox::AcceptRole);
    QPushButton *always = box.addButton(tr("Allow for this chat"), QMessageBox::AcceptRole);
    box.addButton(tr("Deny"), QMessageBox::RejectRole);
    box.setDefaultButton(once);
    box.exec();
    if (box.clickedButton() == always) {
        m_alwaysAllow.insert(tool);
        return true;
    }
    return box.clickedButton() == once;
}

// --- mentions -----------------------------------------------------------------------------------------------------

void AgentPanel::updateMentions(const QString &query)
{
    if (m_files.isEmpty() || !m_input->isEnabled()) {
        closeMentions();
        return;
    }
    QStringList result;
    if (query.isEmpty()) {
        if (m_editors)
            for (const QString &p : m_editors->openFilePaths())
                if (Platform::isUnder(p, m_root) && m_fileSet.contains(relative(p)))
                    result << relative(p);
        for (const QString &f : m_files) {
            if (result.size() >= 8)
                break;
            if (!result.contains(f))
                result << f;
        }
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
    result = result.mid(0, 8);
    if (result.isEmpty()) {
        closeMentions();
        return;
    }
    m_popup->setItems(result);
    m_input->setMentionOpen(true);
    m_popup->showAbove(m_inputBox);
}

void AgentPanel::closeMentions()
{
    m_slash = false;
    m_input->setMentionOpen(false);
    if (m_popup->isVisible())
        m_popup->hide();
}

// --- transcript ---------------------------------------------------------------------------------------------------

QWidget *AgentPanel::userBubble(const Message &m)
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
    auto *copy = new CopyButton(box, false);
    copy->setSource([text = m.text] { return text; });
    copy->revealOnHover(box);
    head->addWidget(copy);
    l->addLayout(head);
    if (!m.files.isEmpty()) {
        auto *files = new QLabel(m.files.join(QStringLiteral("  ·  ")), box);
        files->setObjectName(QStringLiteral("aiWho"));
        files->setWordWrap(true);
        l->addWidget(files);
    }
    if (!m.quote.isEmpty()) {
        auto *quote = new AiQuote(box);
        quote->setText(m.quote);
        l->addWidget(quote);
    }
    auto *text = new QLabel(m.text, box);
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(text);
    return box;
}

void AgentPanel::wireTurn(AgentTurnView *view)
{
    connect(view, &AgentTurnView::openRequested, this, [this](const QString &rel) {
        QString abs;
        if (AiEdits::resolveInside(m_root, rel, &abs))
            emit openFileRequested(abs);
    });
    connect(view, &AgentTurnView::linkClicked, this, &AgentPanel::followLink);
    connect(view, &AgentTurnView::askRequested, this, &AgentPanel::askAbout);
    connect(view, &AgentTurnView::quoteRequested, this, &AgentPanel::setQuote);
}

QWidget *AgentPanel::emptyHint()
{
    auto *l = new QLabel(tr("<p><b>Start talking to QODE.</b></p>"
                            "<p>QODE works on its own: it searches and reads your project, edits files, runs builds and tests, and reports what it did. "
                            "Edits land as unsaved, undoable changes in your editor tabs and can be reviewed with the git gutter.</p>"
                            "<p>Running a command or deleting a file always asks you first. Type <b>@</b> to point at a file.</p>"));
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    l->setObjectName(QStringLiteral("aiHint"));
    l->setContentsMargins(6, 12, 6, 0);
    return l;
}

void AgentPanel::rebuild()
{
    m_liveView = nullptr; // rebuilt below when a run is going
    updateUsage();        // every change of the chat (new, loaded, run finished, compacted) comes through here
    while (m_listLayout->count() > 1) {
        QLayoutItem *it = m_listLayout->takeAt(0);
        if (QWidget *w = it->widget())
            w->deleteLater();
        delete it;
    }
    int at = 0;
    auto add = [&](QWidget *w) { m_listLayout->insertWidget(at++, w); };
    m_lastTurn = nullptr;
    if (m_session.messages.isEmpty() && !m_busy)
        add(emptyHint());
    for (const Message &m : std::as_const(m_session.messages)) {
        if (m.role == QLatin1String("user")) {
            add(userBubble(m));
        } else if (m.role == QLatin1String("assistant")) {
            auto *view = new AgentTurnView(m.model, m_list);
            view->setBlocks(m.blocks);
            view->setStats(m);
            wireTurn(view);
            add(view);
            if (m_lastTurn)
                m_lastTurn->setCopyPinned(false);
            m_lastTurn = view;
            view->setCopyPinned(true);
            if (QWidget *footer = turnFooter(m))
                add(footer);
        } else if (m.role == QLatin1String("summary")) {
            add(summaryCard(m));
        } else if (m.role == QLatin1String("mcp")) {
            add(mcpCard());
        } else if (m.role == QLatin1String("note")) {
            auto *n = new QLabel(m.text);
            n->setObjectName(QStringLiteral("aiHint"));
            n->setWordWrap(true);
            n->setContentsMargins(6, 0, 6, 0);
            n->setTextInteractionFlags(Qt::TextSelectableByMouse);
            add(n);
        } else {
            auto *e = new QLabel(m.text);
            e->setObjectName(QStringLiteral("aiError"));
            e->setWordWrap(true);
            e->setTextInteractionFlags(Qt::TextSelectableByMouse);
            add(e);
        }
    }
    m_stick = true;
    QTimer::singleShot(0, this, [this] { m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum()); });
}

// Under each assistant turn: what it changed, with Undo / Redo (the time and tokens are in the turn view).
QWidget *AgentPanel::turnFooter(const Message &m)
{
    const auto entries = m.turn >= 0 ? m_journal.entries(m.turn) : QVector<Journal::Entry>();
    const bool canRedo = m.turn >= 0 && m_journal.canRedo(m.turn);
    QStringList facts;
    if (!entries.isEmpty()) {
        QSet<QString> files;
        int added = 0, removed = 0;
        for (const auto &e : entries) {
            files.insert(e.path);
            added += e.added;
            removed += e.removed;
        }
        facts << tr("%n file(s) changed", nullptr, int(files.size())) + QStringLiteral("  +%1 −%2").arg(added).arg(removed);
    } else if (canRedo) {
        facts << tr("Changes undone");
    }
    if (m.contextTokens > 100000)
        facts << tr("context is large: /compact is advised");
    if (facts.isEmpty())
        return nullptr;

    auto *row = new QWidget;
    auto *l = new QHBoxLayout(row);
    l->setContentsMargins(4, 0, 2, 0);
    l->setSpacing(6);
    auto *text = new QLabel(facts.join(QStringLiteral("   ·   ")), row);
    text->setObjectName(QStringLiteral("agentFooter"));
    text->setWordWrap(true);
    l->addWidget(text, 1);
    const int turn = m.turn;
    if (!entries.isEmpty()) {
        auto *undo = new QPushButton(tr("Undo"), row);
        undo->setObjectName(QStringLiteral("agentFooterBtn"));
        undo->setToolTip(tr("Undo everything QODE changed in this turn"));
        connect(undo, &QPushButton::clicked, this, [this, turn] { undoFromUi(turn, tr("the changes of this turn")); });
        l->addWidget(undo, 0, Qt::AlignTop);
    }
    if (canRedo) {
        auto *redo = new QPushButton(tr("Redo"), row);
        redo->setObjectName(QStringLiteral("agentFooterBtn"));
        redo->setToolTip(tr("Bring back the changes that were undone"));
        connect(redo, &QPushButton::clicked, this, &AgentPanel::redoFromUi);
        l->addWidget(redo, 0, Qt::AlignTop);
    }
    return row;
}

QWidget *AgentPanel::summaryCard(const Message &m)
{
    auto *box = new QFrame;
    box->setObjectName(QStringLiteral("aiUser"));
    auto *l = new QVBoxLayout(box);
    l->setContentsMargins(10, 8, 10, 8);
    l->setSpacing(4);
    auto *head = new QHBoxLayout;
    auto *who = new QLabel(tr("CONVERSATION COMPACTED"), box);
    who->setObjectName(QStringLiteral("aiWho"));
    auto *toggle = new QPushButton(tr("Show summary"), box);
    toggle->setObjectName(QStringLiteral("agentFooterBtn"));
    head->addWidget(who, 1);
    head->addWidget(toggle);
    l->addLayout(head);
    auto *hint = new QLabel(tr("The messages above are no longer sent to the model; it continues from this summary."), box);
    hint->setObjectName(QStringLiteral("aiHint"));
    hint->setWordWrap(true);
    l->addWidget(hint);
    auto *view = new MarkdownView(box);
    view->document()->setDocumentMargin(2);
    view->setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; }"));
    view->setAutoHeight(true);
    view->showMarkdown(m.text);
    view->setVisible(false);
    l->addWidget(view);
    connect(toggle, &QPushButton::clicked, this, [view, toggle, this] {
        view->setVisible(!view->isVisible());
        toggle->setText(view->isVisible() ? tr("Hide summary") : tr("Show summary"));
    });
    return box;
}

void AgentPanel::followLink(const QUrl &url)
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

// On a wide window the transcript, model row and input stay one readable column in the middle.
void AgentPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    constexpr int kColumn = 920;
    const int side = qMax(10, (width() - kColumn) / 2);
    m_modelRow->setContentsMargins(side, 10, side, 6);
    m_noticeRow->setContentsMargins(side, 0, side, 6);
    m_listLayout->setContentsMargins(side, 8, side, 8);
    m_bottom->setContentsMargins(side, 4, side, 10);
}

bool AgentPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_input && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        m_inputBox->setProperty("focused", event->type() == QEvent::FocusIn);
        m_inputBox->style()->unpolish(m_inputBox);
        m_inputBox->style()->polish(m_inputBox);
    }
    return QWidget::eventFilter(watched, event);
}

void AgentPanel::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    setStyleSheet(QStringLiteral(
                      "QWidget#agentRoot, QWidget#aiList { background: %1; }"
                      "QWidget#agentHeader { background: %4; border-bottom: 1px solid %5; }"
                      "QWidget#agentHeader QLabel { background: transparent; border: none; }"
                      "QLabel#aiWho { color: %2; font-size: 8pt; letter-spacing: 1px; }"
                      "QLabel#aiHint { color: %2; }"
                      "QLabel#aiError { color: %3; background: transparent; padding: 2px 4px; }"
                      "QFrame#aiUser { background: %4; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#aiUser QLabel { background: transparent; }"
                      "QFrame#aiNotice { background: %4; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#aiNotice QLabel { background: transparent; }"
                      "QFrame#aiInputBox { background: %6; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#aiInputBox[focused=\"true\"] { border-color: %7; }"
                      "AiInput { background: transparent; border: none; padding: 2px; }"
                      "QPushButton#aiModel { background: %6; border: 1px solid %5; border-radius: 8px; padding: 6px 12px; text-align: left; }"
                      "QLabel#aiModelText { background: transparent; }"
                      "QPushButton#aiModel:hover { border-color: %7; }"
                      "QPushButton#aiModel:disabled { color: %2; }"
                      "QPushButton#aiModel::menu-indicator { image: none; width: 0px; }"
                      "QLineEdit#aiCustomModel { background: %6; border: 1px solid %5; border-radius: 6px; padding: 5px 8px; min-width: 200px; }"
                      "QLineEdit#aiCustomModel:focus { border-color: %7; }"
                      "QToolButton#aiSend { background: %7; border: none; border-radius: 13.5px; padding: 0px; }"
                      "QToolButton#aiSend:disabled { background: %4; }"
                      "QToolButton#aiUsage { color: %2; font-size: 8pt; padding: 2px 6px; }"
                      "QToolButton#aiUsage::menu-indicator { image: none; width: 0px; }"
                      "QLabel#agentFooter { color: %2; font-size: 8pt; background: transparent; }"
                      "QPushButton#agentFooterBtn { background: transparent; border: 1px solid %5; border-radius: 6px; padding: 2px 10px; font-size: 8pt; }"
                      "QPushButton#agentFooterBtn:hover { border-color: %7; }"
                      // The /mcp card (claudecode::McpCard, shared with the Claude Code view)
                      "QFrame#ccInfo { background: %4; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#ccInfo QLabel, QFrame#ccInfo QWidget { background: transparent; }"
                      "QLabel#ccInfoTitle { font-weight: 600; font-size: 11pt; }"
                      "QLabel#ccInfoStrong { font-weight: 600; }"
                      "QLabel#ccInfoSmall { color: %2; font-size: 8pt; }"
                      "QToolButton#ccInfoRefresh { border: none; border-radius: 4px; padding: 2px; }"
                      "QToolButton#ccInfoRefresh:hover { background: %5; }"
                      "QFrame#ccFileRow { background: transparent; border-radius: 6px; }"
                      "QFrame#ccFileRow:hover { background: %5; }"
                      "QPushButton#ccLink { background: transparent; border: none; padding: 2px 0px; color: %7; text-align: left; }"
                      "QPushButton#ccLink:hover { text-decoration: underline; }")
                      .arg(t.window.name(), t.textMuted.name(), t.danger.name(), t.panel.name(), t.border.name(), t.editorBg.name(), t.accent.name()));
    const QString res = m_busy ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/arrow-up.svg");
    m_modelArrow->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/chevron-down.svg"), t.textMuted, 14));
    updateModelButton();
    m_send->setIcon(Icons::tinted(res, t.onAccent()));
    m_send->setIconSize(QSize(16, 16));
}

} // namespace agent
