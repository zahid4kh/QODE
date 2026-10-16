#include "AgentPanel.h"

#include "agent/AgentRun.h"
#include "agent/AgentTurnView.h"
#include "ai/AiEdits.h"
#include "ai/AiEnv.h"
#include "ai/AiInput.h"
#include "ai/AiKeyDialog.h"
#include "ai/AiWorkspace.h"
#include "ai/GeminiClient.h"
#include "editor/CodeEditor.h"
#include "lsp/LspManager.h"
#include "lsp/LspTypes.h"
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
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
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
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidgetAction>

namespace agent {

namespace {
const QStringList kModels = {QStringLiteral("gemini-3.5-flash-lite"), QStringLiteral("gemini-3.5-flash"), QStringLiteral("gemini-3.1-flash-lite"),
                             QStringLiteral("gemini-3.6-flash")};

bool looksBinary(const QString &name)
{
    static const QSet<QString> bin = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("webp"),
                                      QStringLiteral("ico"), QStringLiteral("pdf"), QStringLiteral("zip"), QStringLiteral("gz"), QStringLiteral("jar"),
                                      QStringLiteral("ttf"), QStringLiteral("otf"), QStringLiteral("woff"), QStringLiteral("woff2"), QStringLiteral("mp3"),
                                      QStringLiteral("mp4"), QStringLiteral("wav"), QStringLiteral("so"), QStringLiteral("a"), QStringLiteral("o"),
                                      QStringLiteral("class"), QStringLiteral("deb"), QStringLiteral("exe"), QStringLiteral("bin"), QStringLiteral("lock")};
    return bin.contains(QFileInfo(name).suffix().toLower());
}
} // namespace

AgentPanel::AgentPanel(QWidget *parent)
    : QWidget(parent)
    , m_gemini(new GeminiClient(this))
    , m_changedTimer(new QTimer(this))
{
    m_changedTimer->setSingleShot(true);
    m_changedTimer->setInterval(400);
    connect(m_changedTimer, &QTimer::timeout, this, &AgentPanel::projectChanged);
    // --- header -------------------------------------------------------------------------------------------------
    auto *title = new QLabel(tr("AGENT"), this);
    title->setObjectName(QStringLiteral("panelTitle"));
    m_historyBtn = new QToolButton(this);
    Icons::bind(m_historyBtn, QStringLiteral(":/new-icons/scroll-text.svg"));
    m_historyBtn->setToolTip(tr("Agent chats of this project"));
    m_historyBtn->setPopupMode(QToolButton::InstantPopup);
    m_historyBtn->setAutoRaise(true);
    auto *historyMenu = new QMenu(m_historyBtn);
    m_historyBtn->setMenu(historyMenu);
    connect(historyMenu, &QMenu::aboutToShow, this, &AgentPanel::rebuildHistoryMenu);
    m_keyBtn = new QToolButton(this);
    Icons::bind(m_keyBtn, QStringLiteral(":/new-icons/key.svg"));
    m_keyBtn->setToolTip(tr("Gemini API key"));
    m_keyBtn->setAutoRaise(true);
    connect(m_keyBtn, &QToolButton::clicked, this, &AgentPanel::editApiKey);
    m_newBtn = new QToolButton(this);
    Icons::bind(m_newBtn, QStringLiteral(":/new-icons/plus.svg"));
    m_newBtn->setToolTip(tr("New agent chat"));
    m_newBtn->setAutoRaise(true);
    m_closeBtn = new QToolButton(this);
    Icons::bind(m_closeBtn, QStringLiteral(":/new-icons/x.svg"));
    m_closeBtn->setToolTip(tr("Close"));
    m_closeBtn->setAutoRaise(true);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("previewHeader"));
    header->setStyleSheet(QStringLiteral("QWidget#previewHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#previewHeader QLabel { background: transparent; border: none; }"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->addWidget(title, 1);
    hl->addWidget(m_keyBtn);
    hl->addWidget(m_historyBtn);
    hl->addWidget(m_newBtn);
    hl->addWidget(m_closeBtn);

    // --- model --------------------------------------------------------------------------------------------------
    m_modelName = SettingsManager::instance().aiModel();
    m_modelBtn = new QPushButton(this);
    m_modelBtn->setObjectName(QStringLiteral("aiModel"));
    m_modelBtn->setToolTip(tr("Gemini model: pick one or type any model name"));
    m_modelBtn->setCursor(Qt::PointingHandCursor);
    {
        auto *bl = new QHBoxLayout(m_modelBtn);
        bl->setContentsMargins(14, 0, 10, 0);
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
    auto *modelRow = new QHBoxLayout;
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
    m_input = new AiInput(this);
    m_input->setPlaceholderText(tr("Describe a task for the agent…  @ points at a file"));
    m_send = new QToolButton(this);
    m_send->setObjectName(QStringLiteral("aiSend"));
    m_send->setCursor(Qt::PointingHandCursor);
    m_inputBox = new QFrame(this);
    m_inputBox->setObjectName(QStringLiteral("aiInputBox"));
    auto *ib = new QHBoxLayout(m_inputBox);
    ib->setContentsMargins(8, 4, 6, 4);
    ib->setSpacing(6);
    ib->addWidget(m_input, 1);
    ib->addWidget(m_send, 0, Qt::AlignVCenter);

    auto *bottom = new QVBoxLayout;
    bottom->setContentsMargins(10, 4, 10, 10);
    bottom->setSpacing(4);
    bottom->addWidget(m_context);
    bottom->addWidget(m_inputBox);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(header);
    lay->addLayout(modelRow);
    lay->addWidget(m_notice);
    lay->addWidget(m_scroll, 1);
    lay->addLayout(bottom);

    m_popup = new AiMentionPopup(this);
    m_popup->hide();

    // --- wiring -------------------------------------------------------------------------------------------------
    connect(m_closeBtn, &QToolButton::clicked, this, &AgentPanel::closeRequested);
    connect(m_newBtn, &QToolButton::clicked, this, &AgentPanel::startNewChat);
    connect(m_send, &QToolButton::clicked, this, [this] { m_busy ? stop() : send(); });
    connect(m_input, &AiInput::sendRequested, this, &AgentPanel::send);
    connect(m_input, &AiInput::mentionQuery, this, &AgentPanel::updateMentions);
    connect(m_input, &AiInput::mentionClosed, this, &AgentPanel::closeMentions);
    connect(m_input, &AiInput::textChanged, this, [this] { updateSlash(); });
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
                if (p.startsWith(m_root + QLatin1Char('/')))
                    out.append({p, d && d->isModified()});
            }
        return out;
    };
    hooks.saveFiles = [this](const QStringList &paths) {
        if (!m_editors)
            return;
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
        if (m_editors)
            m_editors->pathRenamed(from, to);
    };
    hooks.changed = [this] { scheduleProjectChanged(); };
    hooks.approve = [this](const QString &tool, const QString &title, const QString &detail) { return approve(tool, title, detail); };
    hooks.diagnostics = [this](const QString &abs, QVector<Diag> *rows) { return collectDiagnostics(abs, rows); };
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
        return QFile::remove(abs);
    };
    io.trash = [this](const QString &abs, QString *inTrash) {
        if (m_editors)
            m_editors->closeDocumentsUnder(abs);
        return QFile::moveToTrash(abs, inTrash);
    };
    io.rename = [this](const QString &from, const QString &to) {
        QDir().mkpath(QFileInfo(to).absolutePath());
        if (!QDir().rename(from, to))
            return false;
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
    updateContext();
}

void AgentPanel::editApiKey()
{
    AiKeyDialog dlg(this);
    dlg.exec();
    updateNotice();
}

void AgentPanel::focusInput()
{
    updateNotice();
    if (!m_busy) // the model may have been changed in the other tab
        setModel(SettingsManager::instance().aiModel());
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
    m_modelText->setText(m);
}

void AgentPanel::rebuildModelMenu()
{
    m_modelMenu->clear();
    m_modelMenu->setMinimumWidth(m_modelBtn->width());
    auto *group = new QActionGroup(m_modelMenu);
    QStringList names = kModels;
    if (!names.contains(m_modelName))
        names.prepend(m_modelName);
    for (const QString &n : std::as_const(names)) {
        QAction *a = m_modelMenu->addAction(n);
        a->setCheckable(true);
        a->setActionGroup(group);
        a->setChecked(n == m_modelName);
        connect(a, &QAction::triggered, this, [this, n] { setModel(n); });
    }
    m_modelMenu->addSeparator();
    auto *wa = new QWidgetAction(m_modelMenu);
    auto *holder = new QWidget;
    auto *hl = new QHBoxLayout(holder);
    hl->setContentsMargins(6, 4, 6, 4);
    m_customModel = new QLineEdit(holder);
    m_customModel->setObjectName(QStringLiteral("aiCustomModel"));
    m_customModel->setPlaceholderText(tr("Other model name, then Enter"));
    m_customModel->setClearButtonEnabled(true);
    hl->addWidget(m_customModel);
    wa->setDefaultWidget(holder);
    m_modelMenu->addAction(wa);
    connect(m_customModel, &QLineEdit::returnPressed, this, [this] {
        setModel(m_customModel->text());
        m_modelMenu->close();
    });
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
    const auto sessions = m_store.list();
    Session loaded;
    if (!sessions.isEmpty() && m_store.load(sessions.first().id, &loaded))
        m_session = loaded;
    else
        m_session = Session::create(SettingsManager::instance().aiModel());
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
    m_keyOk = !m_root.isEmpty() && !AiEnv::apiKey(m_root).isEmpty();
    QString text, button;
    if (m_root.isEmpty()) {
        text = tr("Open a project folder to use the agent.");
    } else if (!m_gitOk) {
        text = tr("<b>The agent needs a git repository.</b> It edits many files on its own; git lets you review and revert everything it does.");
        button = tr("Initialize Git repository");
    } else if (!m_keyOk) {
        text = tr("<b>No Gemini API key yet.</b> Add your own Google AI Studio key to use the agent. It stays on this computer.");
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
        m_context->setText(m_status);
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
    return p.startsWith(m_root + QLatin1Char('/')) && !AiEnv::isSensitive(p) ? p : QString();
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
    m_alwaysAllow.clear();
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
    menu->addAction(tr("New agent chat"), this, &AgentPanel::startNewChat);
    menu->addSeparator();
    QAction *undoLast = menu->addAction(tr("Undo the agent's last changes"), this, [this] { undoFromUi(Journal::Last, tr("the agent's last changes")); });
    QAction *undoAll = menu->addAction(tr("Undo all agent changes in this chat"), this, [this] { undoFromUi(Journal::All, tr("all agent changes in this chat")); });
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
            if (p.startsWith(m_root + QLatin1Char('/')) && !AiEnv::isSensitive(p))
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
    startRun(prompt, prompt, mentionedFiles(prompt));
}

// Runs one request. `display` is what the chat shows as the user's message, `request` what the model is asked.
void AgentPanel::startRun(const QString &display, const QString &request, const QStringList &mentioned)
{
    const int turn = int(m_session.messages.size());
    const QString sent = contextFor(mentioned) + request;

    AgentRun::Config cfg;
    cfg.apiKey = AiEnv::apiKey(m_root);
    cfg.model = m_modelName;
    cfg.systemInstruction = AgentRun::systemPrompt(m_root);
    cfg.contents = m_session.history();
    cfg.contents.append(QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", sent}}}}});

    Message user;
    user.role = QStringLiteral("user");
    user.text = display;
    user.sent = sent; // replayed as it was, so the start of every later request stays identical and can be cached
    user.files = mentioned;
    user.time = QDateTime::currentDateTime();
    user.turn = turn;
    m_session.messages.append(user);
    m_session.model = m_modelName;
    if (m_session.title.isEmpty())
        m_session.title = display.simplified().left(60);

    m_live = Message();
    m_live.role = QStringLiteral("assistant");
    m_live.model = m_modelName;
    m_live.time = QDateTime::currentDateTime();
    m_live.turn = turn;

    m_input->clear();
    m_stick = true;
    m_status = tr("Thinking…");
    setBusy(true);
    rebuild(); // the new user bubble, then the live turn
    m_liveView = new AgentTurnView(m_modelName, m_list);
    connect(m_liveView, &AgentTurnView::openRequested, this, [this](const QString &rel) {
        QString abs;
        if (AiEdits::resolveInside(m_root, rel, &abs))
            emit openFileRequested(abs);
    });
    connect(m_liveView, &AgentTurnView::linkClicked, this, &AgentPanel::followLink);
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
                               {"/redo", "bring back what was undone"}};

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

void AgentPanel::updateSlash()
{
    const QString text = m_input->toPlainText();
    if (!text.startsWith(QLatin1Char('/')) || text.contains(QLatin1Char(' ')) || text.contains(QLatin1Char('\n')) || !m_input->isEnabled() || m_busy) {
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

// Handles /init, /compact, /undo, /redo. False for anything else, which is then an ordinary prompt.
bool AgentPanel::runSlash(const QString &prompt)
{
    const QString cmd = prompt.section(QRegularExpression(QStringLiteral("\\s")), 0, 0).toLower();
    const QString arg = prompt.mid(cmd.size()).trimmed().toLower();
    if (cmd == QLatin1String("/init")) {
        if (QFileInfo::exists(m_root + QStringLiteral("/AGENTS.md")))
            addNote(tr("AGENTS.md already exists in this project, so /init left it alone. Ask the agent to update it if it is out of date."));
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
        undoFromUi(all ? int(Journal::All) : int(Journal::Last), all ? tr("all agent changes in this chat") : tr("the agent's last changes"));
        return true;
    }
    if (cmd == QLatin1String("/redo")) {
        redoFromUi();
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

    GeminiClient::Request req;
    req.apiKey = AiEnv::apiKey(m_root);
    req.model = m_modelName;
    req.contents = contents;
    req.systemInstruction = QStringLiteral(
        "You compact a conversation between a user and a coding agent so the work can continue from the summary alone. Write the summary for the "
        "agent itself, not for the user. Cover: the user's goals and requests; decisions, constraints and preferences they stated; facts learned about "
        "the project (stack, layout, commands, conventions); every file created, changed, moved or deleted, and why; commands run and what they showed; "
        "problems still open and the next steps. Be dense and factual, use short bullet lists, no filler, at most 700 words. Keep exact file paths, "
        "names and commands.");
    req.thinkingBudget = 512;

    m_compacting = true;
    m_status = tr("Compacting the conversation…");
    setBusy(true);
    const int gen = ++m_compactGen;
    m_gemini->generate(req, [this, gen](const GeminiClient::Result &r) {
        if (gen != m_compactGen)
            return; // the chat was closed or switched meanwhile
        m_compacting = false;
        Message m;
        m.time = QDateTime::currentDateTime();
        if (r.ok) {
            m.role = QStringLiteral("summary");
            m.text = r.text.trimmed();
            m.model = m_modelName;
            m.promptTokens = r.promptTokens;
        } else {
            m.role = QStringLiteral("error");
            m.text = r.error == QLatin1String("Cancelled") ? tr("Stopped.") : r.error;
        }
        m_session.messages.append(m);
        setBusy(false);
        saveSession();
        rebuild();
    });
}

// --- undo / redo --------------------------------------------------------------------------------------------------

// Asks, then reverts. Files the user changed after the agent are listed and only overwritten on request.
void AgentPanel::undoFromUi(int turn, const QString &what)
{
    if (m_busy)
        return;
    if (turn == Journal::Last && m_journal.lastTurn() < 0) {
        addNote(tr("The agent has not changed anything in this chat (or it was already undone)."));
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
    box.setWindowTitle(tr("Undo agent changes"));
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
        ask.setText(tr("These files were changed after the agent left them. Overwrite them with their earlier version?"));
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

QString AgentPanel::collectDiagnostics(const QString &abs, QVector<Diag> *rows)
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
            if (p.startsWith(m_root + QLatin1Char('/')))
                paths << p;
    }
    auto served = [&] {
        for (const QString &p : std::as_const(paths))
            if (Document *d = m_editors->documentForPath(p))
                if (m_lsp->isServed(d))
                    return true;
        return false;
    };
    // A server that was just asked to start needs a moment.
    QElapsedTimer clock;
    clock.start();
    while (!served() && clock.elapsed() < 4000 && !m_tools.cancelled()) {
        QEventLoop wait;
        QTimer::singleShot(150, &wait, &QEventLoop::quit);
        wait.exec();
    }
    if (!served())
        return tr("no language server is running for %1; use run_command to build or test instead").arg(abs.isEmpty() ? tr("the open files") : QFileInfo(abs).fileName());

    // Give the server time to see the latest edits: wait until its diagnostics have been quiet for a moment.
    QEventLoop loop;
    QTimer quiet, limit, poll;
    quiet.setSingleShot(true);
    limit.setSingleShot(true);
    connect(&quiet, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(&limit, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(&poll, &QTimer::timeout, &loop, [&] {
        if (m_tools.cancelled())
            loop.quit();
    });
    connect(m_lsp, &LspManager::diagnosticsChanged, &loop, [&] { quiet.start(700); });
    quiet.start(1000);
    limit.start(9000);
    poll.start(100);
    loop.exec();

    for (const QString &p : std::as_const(paths))
        for (const LspDiagnostic &d : m_lsp->diagnostics(p)) {
            Diag x;
            x.path = p;
            x.line = d.startLine + 1;
            x.column = d.startColumn + 1;
            x.severity = d.severity;
            x.message = d.message;
            x.source = d.source;
            x.code = d.code;
            rows->append(x);
        }
    return {};
}

void AgentPanel::stop()
{
    if (m_compacting)
        m_gemini->cancel(); // reports "Cancelled" through the callback
    else if (m_run)
        m_run->cancel(); // reports "Stopped." through failed()
}

// Drops a run without reporting it (the chat or project it belonged to is going away).
void AgentPanel::abandonRun()
{
    if (m_compacting) {
        ++m_compactGen; // its answer belongs to a chat that is gone
        m_compacting = false;
        m_gemini->cancel();
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
    m_busy = busy;
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
    box.setWindowTitle(tr("Agent request"));
    box.setText(tr("The agent wants to: <b>%1</b>").arg(title.toHtmlEscaped()));
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
                if (p.startsWith(m_root + QLatin1Char('/')) && m_fileSet.contains(relative(p)))
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
    l->addWidget(who);
    if (!m.files.isEmpty()) {
        auto *files = new QLabel(m.files.join(QStringLiteral("  ·  ")), box);
        files->setObjectName(QStringLiteral("aiWho"));
        files->setWordWrap(true);
        l->addWidget(files);
    }
    auto *text = new QLabel(m.text, box);
    text->setTextFormat(Qt::PlainText);
    text->setWordWrap(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->addWidget(text);
    return box;
}

QWidget *AgentPanel::emptyHint()
{
    auto *l = new QLabel(tr("<p><b>Give the agent a task.</b></p>"
                            "<p>It works on its own: it searches and reads your project, edits files, runs builds and tests, and reports what it did. "
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
    while (m_listLayout->count() > 1) {
        QLayoutItem *it = m_listLayout->takeAt(0);
        if (QWidget *w = it->widget())
            w->deleteLater();
        delete it;
    }
    int at = 0;
    auto add = [&](QWidget *w) { m_listLayout->insertWidget(at++, w); };
    if (m_session.messages.isEmpty() && !m_busy)
        add(emptyHint());
    for (const Message &m : std::as_const(m_session.messages)) {
        if (m.role == QLatin1String("user")) {
            add(userBubble(m));
        } else if (m.role == QLatin1String("assistant")) {
            auto *view = new AgentTurnView(m.model, m_list);
            view->setBlocks(m.blocks);
            connect(view, &AgentTurnView::openRequested, this, [this](const QString &rel) {
                QString abs;
                if (AiEdits::resolveInside(m_root, rel, &abs))
                    emit openFileRequested(abs);
            });
            connect(view, &AgentTurnView::linkClicked, this, &AgentPanel::followLink);
            add(view);
            if (QWidget *footer = turnFooter(m))
                add(footer);
        } else if (m.role == QLatin1String("summary")) {
            add(summaryCard(m));
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

namespace {
QString tokens(int n)
{
    return n >= 1000 ? QStringLiteral("%1k").arg(n / 1000.0, 0, 'f', n >= 10000 ? 0 : 1) : QString::number(n);
}
}

// Under each assistant turn: what it changed (with Undo / Redo) and what it cost.
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
    if (m.requests > 0) {
        QString u = tr("%n request(s)", nullptr, m.requests) + QStringLiteral(" · ") + tr("%1 in").arg(tokens(m.promptTokens));
        if (m.cachedTokens > 0)
            u += tr(" (%1 cached)").arg(tokens(m.cachedTokens));
        u += tr(" · %1 out").arg(tokens(m.outputTokens));
        facts << u;
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
        undo->setToolTip(tr("Undo everything the agent changed in this turn"));
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
    if (url.scheme() == QLatin1String("qode-open")) {
        QString abs;
        if (AiEdits::resolveInside(m_root, QUrl::fromPercentEncoding(url.path().toUtf8()), &abs))
            emit openFileRequested(abs);
        return;
    }
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

void AgentPanel::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    setStyleSheet(QStringLiteral(
                      "QWidget#aiList { background: %1; }"
                      "QLabel#aiWho { color: %2; font-size: 8pt; letter-spacing: 1px; }"
                      "QLabel#aiHint { color: %2; }"
                      "QLabel#aiError { color: %3; background: transparent; padding: 2px 4px; }"
                      "QFrame#aiUser { background: %4; border: 1px solid %5; border-radius: 10px; }"
                      "QFrame#aiUser QLabel { background: transparent; }"
                      "QFrame#aiNotice { background: %4; border-bottom: 1px solid %5; }"
                      "QFrame#aiNotice QLabel { background: transparent; }"
                      "QFrame#aiInputBox { background: %6; border: 1px solid %5; border-radius: 10px; }"
                      "AiInput { background: transparent; border: none; padding: 2px; }"
                      "QPushButton#aiModel { background: %6; border: 1px solid %5; border-radius: 8px; padding: 6px 12px; text-align: left; }"
                      "QLabel#aiModelText { background: transparent; }"
                      "QPushButton#aiModel:hover { border-color: %7; }"
                      "QPushButton#aiModel:disabled { color: %2; }"
                      "QPushButton#aiModel::menu-indicator { image: none; width: 0px; }"
                      "QLineEdit#aiCustomModel { background: %6; border: 1px solid %5; border-radius: 6px; padding: 5px 8px; min-width: 200px; }"
                      "QLineEdit#aiCustomModel:focus { border-color: %7; }"
                      "QToolButton#aiSend { background: %7; border: none; border-radius: 12px; padding: 4px; }"
                      "QToolButton#aiSend:disabled { background: %4; }"
                      "QLabel#agentFooter { color: %2; font-size: 8pt; background: transparent; }"
                      "QPushButton#agentFooterBtn { background: transparent; border: 1px solid %5; border-radius: 6px; padding: 2px 10px; font-size: 8pt; }"
                      "QPushButton#agentFooterBtn:hover { border-color: %7; }")
                      .arg(t.window.name(), t.textMuted.name(), t.danger.name(), t.panel.name(), t.border.name(), t.editorBg.name(), t.accent.name()));
    const QString res = m_busy ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/arrow-up.svg");
    m_modelArrow->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/chevron-down.svg"), t.textMuted, 14));
    m_send->setIcon(Icons::tinted(res, t.onAccent()));
    m_send->setIconSize(QSize(16, 16));
}

} // namespace agent
