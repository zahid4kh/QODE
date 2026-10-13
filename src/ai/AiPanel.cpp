#include "AiPanel.h"

#include "ai/AiAgent.h"
#include "ai/AiChangeCard.h"
#include "ai/AiEnv.h"
#include "ai/AiInput.h"
#include "ai/AiWorkspace.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "media/MarkdownView.h"
#include "palette/FuzzyMatcher.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QActionGroup>
#include <QWidgetAction>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

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

QString stripNumber(const QString &step)
{
    static const QRegularExpression re(QStringLiteral("^\\s*\\d+[.)]\\s*"));
    return QString(step).remove(re);
}

QString planMarkdown(const QStringList &plan)
{
    QString md = QStringLiteral("**Plan**\n\n");
    for (int i = 0; i < plan.size(); ++i)
        md += QStringLiteral("%1. %2\n").arg(i + 1).arg(stripNumber(plan.at(i)));
    return md;
}
}

AiPanel::AiPanel(QWidget *parent)
    : QWidget(parent)
    , m_agent(new AiAgent(this))
{
    // --- header -------------------------------------------------------------------------------------------------
    m_title = new QLabel(tr("AI CHAT"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_historyBtn = new QToolButton(this);
    Icons::bind(m_historyBtn, QStringLiteral(":/new-icons/scroll-text.svg"));
    m_historyBtn->setToolTip(tr("Chats of this project"));
    m_historyBtn->setPopupMode(QToolButton::InstantPopup);
    m_historyBtn->setAutoRaise(true);
    auto *historyMenu = new QMenu(m_historyBtn);
    m_historyBtn->setMenu(historyMenu);
    connect(historyMenu, &QMenu::aboutToShow, this, &AiPanel::rebuildHistoryMenu);
    m_newBtn = new QToolButton(this);
    Icons::bind(m_newBtn, QStringLiteral(":/new-icons/plus.svg"));
    m_newBtn->setToolTip(tr("New chat"));
    m_newBtn->setAutoRaise(true);
    m_closeBtn = new QToolButton(this);
    Icons::bind(m_closeBtn, QStringLiteral(":/new-icons/x.svg"));
    m_closeBtn->setToolTip(tr("Close AI chat"));
    m_closeBtn->setAutoRaise(true);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("previewHeader"));
    header->setStyleSheet(QStringLiteral("QWidget#previewHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#previewHeader QLabel { background: transparent; border: none; }"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->addWidget(m_title, 1);
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
    connect(m_modelMenu, &QMenu::aboutToShow, this, &AiPanel::rebuildModelMenu);
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
            updateNotice();
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

    // --- input --------------------------------------------------------------------------------------------------
    m_context = new QLabel(this);
    m_context->setObjectName(QStringLiteral("aiWho"));
    m_input = new AiInput(this);
    m_input->setPlaceholderText(tr("Ask for a change…  @ attaches a file"));
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
    connect(m_closeBtn, &QToolButton::clicked, this, &AiPanel::closeRequested);
    connect(m_newBtn, &QToolButton::clicked, this, &AiPanel::startNewChat);
    connect(m_send, &QToolButton::clicked, this, [this] { m_busy ? stop() : send(); });
    connect(m_input, &AiInput::sendRequested, this, &AiPanel::send);
    connect(m_input, &AiInput::mentionQuery, this, &AiPanel::updateMentions);
    connect(m_input, &AiInput::mentionClosed, this, &AiPanel::closeMentions);
    connect(m_input, &AiInput::mentionKey, this, [this](int key) {
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
        m_input->completeMention(p);
        closeMentions();
        m_input->setFocus();
    });

    AiAgent::Hooks hooks;
    hooks.readFile = [this](const QString &abs, QString *text) { return m_editors && AiWorkspace(m_editors, m_root).readFile(abs, text); };
    hooks.apply = [this](const QVector<AiEdits::Edit> &edits) {
        return m_editors ? AiWorkspace(m_editors, m_root).apply(edits) : QVector<AiEdits::Outcome>();
    };
    m_agent->setHooks(hooks);
    connect(m_agent, &AiAgent::status, this, [this](const QString &s) {
        m_liveStatus = s;
        if (m_live)
            m_live->showMarkdown((m_livePlan.isEmpty() ? QString() : planMarkdown(m_livePlan) + QLatin1Char('\n')) + liveMarkdown());
        scrollToEnd();
    });
    connect(m_agent, &AiAgent::planReady, this, [this](const QStringList &steps) {
        m_livePlan = steps;
        if (m_live)
            m_live->showMarkdown(planMarkdown(steps) + QLatin1Char('\n') + liveMarkdown());
        scrollToEnd();
    });
    connect(m_agent, &AiAgent::finished, this, [this](const QString &md, const QVector<AiEdits::Outcome> &outcomes) {
        AiMessage m;
        m.role = QStringLiteral("assistant");
        m.text = md;
        m.plan = m_livePlan;
        m.model = m_pendingModel;
        m.time = QDateTime::currentDateTime();
        for (const AiEdits::Outcome &o : outcomes) {
            AiChange c;
            c.path = o.edit.path;
            c.op = AiEdits::opName(o.edit.op);
            c.startLine = o.edit.startLine;
            c.endLine = o.edit.endLine;
            c.ok = o.ok;
            c.note = o.note;
            c.before = o.before.left(60000);
            c.after = o.edit.code.left(60000);
            m.changes.append(c);
        }
        m_session.messages.append(m);
        m_livePlan.clear();
        setBusy(false);
        saveSession();
        rebuild();
        m_input->setFocus();
    });
    connect(m_agent, &AiAgent::failed, this, [this](const QString &err) {
        AiMessage m;
        m.role = QStringLiteral("error");
        m.text = err;
        m.time = QDateTime::currentDateTime();
        m_session.messages.append(m);
        m_livePlan.clear();
        setBusy(false);
        saveSession();
        rebuild();
        m_input->setFocus();
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AiPanel::applyTheme);

    applyTheme();
    setBusy(false);
    updateNotice();
    rebuild();
}

void AiPanel::setEditors(EditorManager *editors)
{
    m_editors = editors;
    connect(editors, &EditorManager::currentChanged, this, &AiPanel::updateContext);
    updateContext();
}

void AiPanel::focusInput()
{
    updateNotice(); // the key file may have been added since
    m_input->setFocus();
}

QString AiPanel::relative(const QString &absolute) const
{
    return m_root.isEmpty() ? absolute : QDir(m_root).relativeFilePath(absolute);
}

void AiPanel::setModel(const QString &name)
{
    const QString m = name.trimmed();
    if (m.isEmpty())
        return;
    m_modelName = m;
    SettingsManager::instance().setAiModel(m);
    m_modelText->setText(m);
}

// The presets as checkable entries, then a field for any other model name. The menu uses the application's rounded menu style.
void AiPanel::rebuildModelMenu()
{
    m_modelMenu->clear();
    m_modelMenu->setMinimumWidth(m_modelBtn->width());
    auto *group = new QActionGroup(m_modelMenu);
    QStringList names = kModels;
    if (!names.contains(m_modelName))
        names.prepend(m_modelName); // a custom model typed earlier stays selectable
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

void AiPanel::setProject(const QString &root)
{
    if (root == m_root)
        return;
    if (m_busy)
        stop();
    m_root = root;
    m_files.clear();
    m_fileSet.clear();
    m_store.setDirectory(root.isEmpty() ? QString() : SettingsManager::instance().projectDataDir());
    const auto sessions = m_store.list();
    AiSession loaded;
    if (!sessions.isEmpty() && m_store.load(sessions.first().id, &loaded))
        m_session = loaded;
    else
        m_session = AiSession::create(SettingsManager::instance().aiModel());
    updateNotice();
    updateContext();
    rebuild();
}

void AiPanel::setGitRepo(bool isRepo)
{
    if (m_gitOk == isRepo)
        return;
    m_gitOk = isRepo;
    updateNotice();
}

void AiPanel::setProjectFiles(const QStringList &absolutePaths)
{
    m_files.clear();
    m_fileSet.clear();
    m_files.reserve(absolutePaths.size());
    for (const QString &p : absolutePaths) {
        if (looksBinary(p))
            continue;
        const QString rel = relative(p);
        if (rel.startsWith(QLatin1String("..")))
            continue;
        m_files << rel;
        m_fileSet.insert(rel);
    }
}

void AiPanel::updateNotice()
{
    m_keyOk = !m_root.isEmpty() && !AiEnv::apiKey(m_root).isEmpty();
    QString text, button;
    if (m_root.isEmpty()) {
        text = tr("Open a project folder to use the AI chat.");
    } else if (!m_gitOk) {
        text = tr("<b>AI editing needs a git repository.</b> Every change Gemini makes can then be reviewed and reverted with git.");
        button = tr("Initialize Git repository");
    } else if (!m_keyOk) {
        text = tr("<b>No Gemini API key found.</b> Put <code>GEMINI_API_KEY=…</code> in a <code>.env.local</code> file in the project folder "
                  "(or next to QODE), or set the environment variable.");
        button = tr("Check again");
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

void AiPanel::updateContext()
{
    const QString f = focusedFile();
    if (f.isEmpty()) {
        m_context->setText(tr("No file open — nothing attached"));
        return;
    }
    const QString text = tr("Attached: %1").arg(relative(f));
    m_context->setText(QFontMetrics(m_context->font()).elidedText(text, Qt::ElideMiddle, qMax(120, m_context->width() - 4)));
    m_context->setToolTip(text);
}

QString AiPanel::focusedFile() const
{
    if (!m_editors || m_root.isEmpty())
        return {};
    Document *d = m_editors->currentDocument();
    if (!d || d->isUntitled())
        return {};
    const QString p = d->filePath();
    return p.startsWith(m_root + QLatin1Char('/')) ? p : QString();
}

// --- sessions -----------------------------------------------------------------------------------------------------

void AiPanel::saveSession()
{
    if (m_session.messages.isEmpty() || !m_store.valid())
        return;
    m_session.updated = QDateTime::currentDateTime();
    m_store.save(m_session);
}

void AiPanel::startNewChat()
{
    if (m_busy)
        stop();
    saveSession();
    m_session = AiSession::create(SettingsManager::instance().aiModel());
    m_livePlan.clear();
    rebuild();
    m_input->setFocus();
}

void AiPanel::loadSession(const QString &id)
{
    AiSession s;
    if (!m_store.load(id, &s))
        return;
    if (m_busy)
        stop();
    saveSession();
    m_session = s;
    rebuild();
}

void AiPanel::deleteCurrentChat()
{
    if (m_busy)
        stop();
    m_store.remove(m_session.id);
    const auto sessions = m_store.list();
    AiSession next;
    if (!sessions.isEmpty() && m_store.load(sessions.first().id, &next))
        m_session = next;
    else
        m_session = AiSession::create(SettingsManager::instance().aiModel());
    rebuild();
}

void AiPanel::rebuildHistoryMenu()
{
    QMenu *menu = m_historyBtn->menu();
    menu->clear();
    menu->addAction(tr("New chat"), this, &AiPanel::startNewChat);
    const auto sessions = m_store.list();
    if (!sessions.isEmpty())
        menu->addSeparator();
    for (const AiChatStore::Info &s : sessions) {
        const QString title = s.title.isEmpty() ? tr("(untitled)") : s.title;
        QAction *a = menu->addAction(QStringLiteral("%1   ·   %2").arg(title.left(48), QLocale().toString(s.updated, QLocale::ShortFormat)));
        a->setCheckable(true);
        a->setChecked(s.id == m_session.id);
        const QString id = s.id;
        connect(a, &QAction::triggered, this, [this, id] { loadSession(id); });
    }
    if (!m_session.messages.isEmpty()) {
        menu->addSeparator();
        menu->addAction(tr("Delete this chat"), this, &AiPanel::deleteCurrentChat);
    }
}

// --- sending ------------------------------------------------------------------------------------------------------

QStringList AiPanel::mentionedFiles(const QString &prompt) const
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

void AiPanel::send()
{
    if (m_busy)
        return;
    const QString prompt = m_input->toPlainText().trimmed();
    if (prompt.isEmpty())
        return;
    updateNotice();
    if (!m_input->isEnabled())
        return;
    const QString key = AiEnv::apiKey(m_root);

    const QString model = m_modelName;
    m_pendingModel = model;

    QStringList attachedRel;
    QStringList attachedAbs;
    const QString focused = focusedFile();
    if (!focused.isEmpty()) {
        attachedAbs << focused;
        attachedRel << relative(focused);
    }
    for (const QString &rel : mentionedFiles(prompt)) {
        const QString abs = QDir(m_root).filePath(rel);
        if (!attachedAbs.contains(abs)) {
            attachedAbs << abs;
            attachedRel << rel;
        }
    }

    AiAgent::Turn turn;
    turn.apiKey = key;
    turn.model = model;
    turn.root = m_root;
    turn.prompt = prompt;
    turn.attached = attachedAbs;
    turn.projectFiles = m_files;
    turn.history = m_session.history();

    AiMessage user;
    user.role = QStringLiteral("user");
    user.text = prompt;
    user.files = attachedRel;
    user.time = QDateTime::currentDateTime();
    m_session.messages.append(user);
    m_session.model = model;
    if (m_session.title.isEmpty())
        m_session.title = prompt.simplified().left(60);

    m_input->clear();
    m_livePlan.clear();
    m_liveStatus = tr("Planning…");
    setBusy(true);
    rebuild();
    m_agent->run(turn);
}

void AiPanel::stop()
{
    m_agent->cancel();
    AiMessage m;
    m.role = QStringLiteral("error");
    m.text = tr("Stopped.");
    m.time = QDateTime::currentDateTime();
    m_session.messages.append(m);
    m_livePlan.clear();
    setBusy(false);
    saveSession();
    rebuild();
}

void AiPanel::setBusy(bool busy)
{
    m_busy = busy;
    m_input->setReadOnly(busy);
    m_modelBtn->setEnabled(!busy);
    m_send->setToolTip(busy ? tr("Stop") : tr("Send (Enter)"));
    applyTheme();
    updateNotice();
}

// --- mentions -----------------------------------------------------------------------------------------------------

void AiPanel::updateMentions(const QString &query)
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
        QVector<QPair<int, int>> scored; // (score, index)
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

void AiPanel::closeMentions()
{
    m_input->setMentionOpen(false);
    if (m_popup->isVisible())
        m_popup->hide();
}

// --- transcript ---------------------------------------------------------------------------------------------------

QString AiPanel::markdownOf(const AiMessage &m) const
{
    return m.text;
}

QString AiPanel::liveMarkdown() const
{
    return QStringLiteral("*%1*").arg(m_liveStatus);
}

QWidget *AiPanel::userBubble(const AiMessage &m)
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

QWidget *AiPanel::replyBubble(const AiMessage &m)
{
    auto *box = new QWidget;
    auto *l = new QVBoxLayout(box);
    l->setContentsMargins(2, 0, 2, 0);
    l->setSpacing(6);
    auto *who = new QLabel(m.model.isEmpty() ? tr("GEMINI") : tr("GEMINI · %1").arg(m.model), box);
    who->setObjectName(QStringLiteral("aiWho"));
    l->addWidget(who);
    auto markdownView = [&](const QString &md) {
        auto *view = new MarkdownView(box);
        view->document()->setDocumentMargin(2);
        view->setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; }"));
        view->setAutoHeight(true);
        connect(view, &QTextBrowser::anchorClicked, this, &AiPanel::followLink);
        view->showMarkdown(md);
        l->addWidget(view);
    };
    if (!m.plan.isEmpty())
        markdownView(planMarkdown(m.plan));
    for (const AiChange &c : m.changes) {
        auto *card = new AiChangeCard(c, box);
        connect(card, &AiChangeCard::openRequested, this, [this](const QString &rel) {
            QString abs;
            if (AiEdits::resolveInside(m_root, rel, &abs))
                emit openFileRequested(abs);
        });
        l->addWidget(card);
    }
    if (!m.text.trimmed().isEmpty())
        markdownView(markdownOf(m));
    return box;
}

QWidget *AiPanel::liveBubble()
{
    auto *box = new QWidget;
    auto *l = new QVBoxLayout(box);
    l->setContentsMargins(2, 0, 2, 0);
    l->setSpacing(2);
    auto *who = new QLabel(tr("GEMINI · %1").arg(m_pendingModel), box);
    who->setObjectName(QStringLiteral("aiWho"));
    l->addWidget(who);
    m_live = new MarkdownView(box);
    m_live->document()->setDocumentMargin(2);
    m_live->setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; }"));
    m_live->setAutoHeight(true);
    connect(m_live, &QTextBrowser::anchorClicked, this, &AiPanel::followLink);
    m_live->showMarkdown((m_livePlan.isEmpty() ? QString() : planMarkdown(m_livePlan) + QLatin1Char('\n')) + liveMarkdown());
    l->addWidget(m_live);
    return box;
}

QWidget *AiPanel::emptyHint()
{
    auto *l = new QLabel(tr("<p><b>Ask Gemini to change your code.</b></p>"
                            "<p>It plans the steps, edits the files and tells you what it did. The edits land as unsaved changes "
                            "in your editor tabs, so you can review them with the git gutter and undo them with <b>Ctrl+Z</b>.</p>"
                            "<p>The file you are looking at is attached automatically. Type <b>@</b> to attach other files.</p>"));
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    l->setObjectName(QStringLiteral("aiHint"));
    l->setContentsMargins(6, 12, 6, 0);
    return l;
}

void AiPanel::rebuild()
{
    m_live = nullptr;
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
    for (const AiMessage &m : std::as_const(m_session.messages)) {
        if (m.role == QLatin1String("user")) {
            add(userBubble(m));
        } else if (m.role == QLatin1String("assistant")) {
            add(replyBubble(m));
        } else {
            auto *e = new QLabel(m.text);
            e->setObjectName(QStringLiteral("aiError"));
            e->setWordWrap(true);
            e->setTextInteractionFlags(Qt::TextSelectableByMouse);
            add(e);
        }
    }
    if (m_busy)
        add(liveBubble());
    scrollToEnd();
}

void AiPanel::scrollToEnd()
{
    for (int delay : {0, 60, 200})
        QTimer::singleShot(delay, this, [this] { m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum()); });
}

void AiPanel::followLink(const QUrl &url)
{
    if (url.scheme() == QLatin1String("qode-copy")) {
        if (auto *view = qobject_cast<MarkdownView *>(sender()))
            view->handleCopyLink(url);
        return;
    }
    if (url.scheme() == QLatin1String("qode-open")) {
        QString abs;
        if (AiEdits::resolveInside(m_root, QUrl::fromPercentEncoding(url.path().toUtf8()), &abs))
            emit openFileRequested(abs);
        return;
    }
    if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

void AiPanel::applyTheme()
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
                      "QToolButton#aiSend:disabled { background: %4; }")
                      .arg(t.window.name(), t.textMuted.name(), t.danger.name(), t.panel.name(), t.border.name(), t.editorBg.name(), t.accent.name()));
    // The send button's glyph sits on the accent colour.
    const QString res = m_busy ? QStringLiteral(":/new-icons/square.svg") : QStringLiteral(":/new-icons/arrow-up.svg");
    m_modelArrow->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/chevron-down.svg"), t.textMuted, 14));
    m_send->setIcon(Icons::tinted(res, t.onAccent()));
    m_send->setIconSize(QSize(16, 16));
}
