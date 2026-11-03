#include "InlineEdit.h"

#include "ai/AiEnv.h"
#include "ai/AiKeyDialog.h"
#include "ai/AiModels.h"
#include "ai/InlineClient.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QActionGroup>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QStyle>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kMaxFileChars = 160000; // larger files are sent as a window around the marked lines
constexpr int kWindowLines = 700;
const QLatin1String kAnswerTag("@@answer");

QString systemPrompt()
{
    return QStringLiteral(
        "You are QODE's inline editor, built into a code editor. The user marked some lines of a file and typed an "
        "instruction. Your reply replaces the marked lines in the file, character for character.\n"
        "\n"
        "Rules:\n"
        "- Reply with the new text for the marked lines only, exactly as it should appear in the file: no Markdown code "
        "fences, no explanation before or after, no line numbers, no markers.\n"
        "- Keep what the instruction does not ask to change exactly as it was, including comments and blank lines.\n"
        "- Match the file's indentation (tabs or spaces and their width) and its style. Every line carries its full "
        "leading whitespace as it will appear in the file.\n"
        "- When the file shows <<<INSERT HERE>>> instead of marked lines, reply with the code to insert at that place, "
        "indented for it.\n"
        "- Never repeat code that is outside the marked lines.\n"
        "- If the instruction is a question that does not ask for a change, reply with the line @@answer and then a short "
        "answer (a few sentences; `inline code` and short lists are fine, no headings); the file is left as it is.");
}

// The reply without the code fence a model may still put around it. `final`: the reply is complete.
QString cleanReply(QString text, bool final)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    if (text.startsWith(QLatin1String("```"))) {
        const int nl = text.indexOf(QLatin1Char('\n'));
        if (nl < 0)
            return {};
        text.remove(0, nl + 1);
    }
    if (final) {
        while (text.endsWith(QLatin1Char('\n')))
            text.chop(1);
        const int nl = text.lastIndexOf(QLatin1Char('\n'));
        if (text.mid(nl + 1).trimmed() == QLatin1String("```")) {
            text.truncate(qMax(0, nl));
        }
        while (text.endsWith(QLatin1Char('\n')))
            text.chop(1);
    } else {
        const int nl = text.lastIndexOf(QLatin1Char('\n'));
        if (text.mid(nl + 1).startsWith(QLatin1Char('`'))) // a closing fence on its way
            text.truncate(qMax(0, nl));
    }
    return text;
}

} // namespace

InlineEdit *InlineEdit::open(CodeEditor *editor, Document *doc, const QString &projectRoot, int firstLine, int lastLine, int restoreAt,
                             bool *created)
{
    if (created)
        *created = false;
    if (!editor || !doc)
        return nullptr;
    if (InlineEdit *open = editor->findChild<InlineEdit *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (restoreAt >= 0) { // "/?" typed while a box is open: nothing to ask twice, keep the characters
            QTextCursor c(editor->document());
            c.setPosition(restoreAt);
            c.insertText(QStringLiteral("/?"));
        }
        open->raise();
        open->m_input->setFocus();
        return open;
    }
    auto *box = new InlineEdit(editor, doc, projectRoot, firstLine, lastLine, restoreAt);
    if (created)
        *created = true;
    QTextCursor caret = editor->textCursor();
    if (caret.hasSelection()) { // the marked lines show their own tint
        caret.setPosition(caret.selectionStart());
        editor->setTextCursor(caret);
    }
    box->show();
    box->reposition();
    box->m_input->setFocus();
    return box;
}

InlineEdit::InlineEdit(CodeEditor *editor, Document *doc, const QString &projectRoot, int firstLine, int lastLine, int restoreAt)
    : QFrame(editor)
    , m_editor(editor)
    , m_doc(doc)
    , m_root(projectRoot)
    , m_client(new InlineClient(this))
    , m_flushTimer(new QTimer(this))
    , m_clockTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("inlineEdit"));
    setAttribute(Qt::WA_StyledBackground);
    QTextDocument *td = editor->document();
    firstLine = qBound(0, firstLine, td->blockCount() - 1);
    lastLine = qBound(firstLine, lastLine, td->blockCount() - 1);
    const QTextBlock a = td->findBlockByNumber(firstLine);
    const QTextBlock b = td->findBlockByNumber(lastLine);
    m_start = a.position();
    m_origLen = b.position() + b.length() - 1 - m_start;
    m_insertMode = firstLine == lastLine && a.text().trimmed().isEmpty();
    if (restoreAt >= m_start)
        m_restoreAt = restoreAt - m_start;

    m_model = SettingsManager::instance().inlineModel();
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(40);
    connect(m_flushTimer, &QTimer::timeout, this, [this] { flush(false); });
    m_clockTimer->setInterval(100);
    connect(m_clockTimer, &QTimer::timeout, this, [this] {
        setStatus(QStringLiteral("%1 %2s").arg(m_activity, QString::number(m_clock.elapsed() / 1000.0, 'f', 1)));
    });
    connect(m_client, &InlineClient::textDelta, this, &InlineEdit::onDelta);
    connect(m_client, &InlineClient::finished, this, &InlineEdit::onFinished);
    connect(m_client, &InlineClient::retrying, this, [this](const QString &reason, int seconds) {
        m_activity = tr("%1, trying again in %2 s…").arg(reason).arg(seconds);
    });

    buildUi();
    applyTheme();
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &InlineEdit::applyTheme);
    connect(td, &QTextDocument::contentsChange, this, &InlineEdit::documentChanged);
    connect(editor->verticalScrollBar(), &QScrollBar::valueChanged, this, &InlineEdit::reposition);
    connect(editor->horizontalScrollBar(), &QScrollBar::valueChanged, this, &InlineEdit::reposition);
    editor->installEventFilter(this);
    setState(State::Idle);
    takeSnapshot();
    updateMarks();
}

InlineEdit::~InlineEdit()
{
    m_client->disconnect(this);
    m_client->cancel();
    releaseEditor();
    if (m_editor)
        m_editor->setInlineMarks({});
}

// --- UI ------------------------------------------------------------------------------------------------------------

void InlineEdit::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 6, 10, 10);
    outer->setSpacing(6);

    m_header = new QWidget(this);
    m_header->setObjectName(QStringLiteral("inlineHeader"));
    m_header->setCursor(Qt::OpenHandCursor);
    m_header->setToolTip(tr("Drag to move"));
    m_header->installEventFilter(this);
    auto *hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(6);
    auto *icon = new QLabel(m_header);
    icon->setObjectName(QStringLiteral("inlineIcon"));
    m_title = new QLabel(m_header);
    m_title->setObjectName(QStringLiteral("inlineTitle"));
    m_modelBtn = new QToolButton(m_header);
    m_modelBtn->setObjectName(QStringLiteral("inlineModel"));
    m_modelBtn->setPopupMode(QToolButton::InstantPopup);
    m_modelBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_modelBtn->setToolTip(tr("Model used for this request"));
    m_modelBtn->setMenu(new QMenu(m_modelBtn));
    connect(m_modelBtn->menu(), &QMenu::aboutToShow, this, &InlineEdit::rebuildModelMenu);
    m_closeBtn = new QToolButton(m_header);
    m_closeBtn->setObjectName(QStringLiteral("inlineClose"));
    m_closeBtn->setToolTip(tr("Close (Esc)"));
    m_closeBtn->setAutoRaise(true);
    connect(m_closeBtn, &QToolButton::clicked, this, [this] {
        if (m_hasProposal)
            reject();
        else
            close(true);
    });
    hl->addWidget(icon);
    hl->addWidget(m_title, 1);
    hl->addWidget(m_modelBtn);
    hl->addWidget(m_closeBtn);
    outer->addWidget(m_header);

    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(6);
    m_input = new QPlainTextEdit(this);
    m_input->setObjectName(QStringLiteral("inlineInput"));
    m_input->setTabChangesFocus(true);
    m_input->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_input->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // on once the text outgrows six lines (fitInput)
    m_input->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_input->installEventFilter(this);
    connect(m_input, &QPlainTextEdit::textChanged, this, [this] {
        fitInput();
        m_sendBtn->setEnabled(!m_input->toPlainText().trimmed().isEmpty() && m_state != State::Running);
    });
    m_sendBtn = new QToolButton(this);
    m_sendBtn->setObjectName(QStringLiteral("inlineSend"));
    m_sendBtn->setToolTip(tr("Send (Enter)"));
    m_sendBtn->setEnabled(false);
    connect(m_sendBtn, &QToolButton::clicked, this, &InlineEdit::send);
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_sendBtn, 0, Qt::AlignBottom);
    outer->addLayout(inputRow);

    m_answer = new QTextBrowser(this);
    m_answer->setObjectName(QStringLiteral("inlineAnswer"));
    m_answer->setOpenExternalLinks(true);
    m_answer->hide();
    outer->addWidget(m_answer);

    m_statusRow = new QWidget(this);
    auto *sl = new QHBoxLayout(m_statusRow);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->setSpacing(6);
    m_status = new QLabel(m_statusRow);
    m_status->setObjectName(QStringLiteral("inlineStatus"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_keyBtn = new QPushButton(tr("Set API key…"), m_statusRow);
    m_keyBtn->setObjectName(QStringLiteral("inlineSecondary"));
    connect(m_keyBtn, &QPushButton::clicked, this, [this] {
        AiKeyDialog dialog(AiEnv::providerForModel(m_model), this);
        dialog.exec();
        if (!AiEnv::apiKey(AiEnv::providerForModel(m_model), m_root).isEmpty()) {
            m_keyBtn->hide();
            setStatus(QString());
            m_input->setFocus();
        }
    });
    m_stopBtn = new QPushButton(tr("Stop"), m_statusRow);
    m_stopBtn->setObjectName(QStringLiteral("inlineSecondary"));
    m_stopBtn->setToolTip(tr("Stop the request (Esc)"));
    connect(m_stopBtn, &QPushButton::clicked, m_client, &InlineClient::cancel);
    m_rejectBtn = new QPushButton(tr("Reject"), m_statusRow);
    m_rejectBtn->setObjectName(QStringLiteral("inlineSecondary"));
    m_rejectBtn->setToolTip(tr("Take the proposal out again (Esc)"));
    connect(m_rejectBtn, &QPushButton::clicked, this, &InlineEdit::reject);
    m_acceptBtn = new QPushButton(tr("Accept"), m_statusRow);
    m_acceptBtn->setObjectName(QStringLiteral("inlineAccept"));
    m_acceptBtn->setToolTip(tr("Keep the proposal (Ctrl+Enter)"));
    connect(m_acceptBtn, &QPushButton::clicked, this, &InlineEdit::accept);
    sl->addWidget(m_status, 1);
    sl->addWidget(m_keyBtn);
    sl->addWidget(m_stopBtn);
    sl->addWidget(m_rejectBtn);
    sl->addWidget(m_acceptBtn);
    outer->addWidget(m_statusRow);

    updateTitle();
    updateModelButton();
    fitInput();
}

void InlineEdit::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QColor hover = t.accent;
    hover.setAlpha(t.dark ? 40 : 30);
    setStyleSheet(QStringLiteral(
                      "QFrame#inlineEdit { background: %1; border: 1px solid %3; border-radius: 10px; }"
                      "QFrame#inlineEdit[focused=\"true\"] { border-color: %4; }"
                      "QWidget#inlineHeader { background: transparent; }"
                      "QLabel#inlineTitle { color: %5; background: transparent; }"
                      "QLabel#inlineIcon { background: transparent; }"
                      "QToolButton#inlineModel { color: %2; background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 2px 6px; }"
                      "QToolButton#inlineModel:hover { border-color: %3; }"
                      "QToolButton#inlineModel::menu-indicator { image: none; width: 0px; }"
                      "QToolButton#inlineClose { background: transparent; border: none; border-radius: 4px; padding: 2px; }"
                      "QToolButton#inlineClose:hover { background: %8; }"
                      "QPlainTextEdit#inlineInput { background: %6; color: %2; border: 1px solid %3; border-radius: 6px; padding: 2px 4px;"
                      " selection-background-color: %7; }"
                      "QPlainTextEdit#inlineInput:focus { border-color: %4; }"
                      "QToolButton#inlineSend { background: %4; border: none; border-radius: 13px; min-width: 26px; max-width: 26px;"
                      " min-height: 26px; max-height: 26px; }"
                      "QToolButton#inlineSend:disabled { background: %3; }"
                      "QTextBrowser#inlineAnswer { background: %6; color: %2; border: 1px solid %3; border-radius: 6px; padding: 4px; }"
                      "QLabel#inlineStatus { color: %5; background: transparent; }"
                      "QLabel#inlineStatus[error=\"true\"] { color: %9; }"
                      "QPushButton#inlineSecondary { background: transparent; color: %2; border: 1px solid %3; border-radius: 6px; padding: 3px 10px; }"
                      "QPushButton#inlineSecondary:hover { background: %8; }"
                      "QPushButton#inlineAccept { background: %4; color: %10; border: 1px solid %4; border-radius: 6px; padding: 3px 12px; }"
                      "QPushButton#inlineAccept:hover { background: %11; }")
                      .arg(t.panel.name(), t.editorFg.name(), t.border.name(), t.accent.name(), t.textMuted.name(), t.editorBg.name(),
                           t.selection.name(), hover.name(QColor::HexArgb), t.danger.name(), t.onAccent().name(), t.accent.lighter(112).name()));
    if (auto *icon = findChild<QLabel *>(QStringLiteral("inlineIcon")))
        icon->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/sparkles.svg"), t.accent, 14));
    m_closeBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/x.svg"), t.textMuted));
    m_sendBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/arrow-up.svg"), t.onAccent(), t.textMuted));
    updateModelButton();
    updateMarks();
}

void InlineEdit::updateModelButton()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const AiEnv::Provider p = AiEnv::providerForModel(m_model);
    m_modelBtn->setIcon(Icons::tinted(AiModels::providerIcon(p), AiModels::providerColor(t, p)));
    m_modelBtn->setText(AiModels::label(m_model) + QStringLiteral("  ▾"));
}

void InlineEdit::rebuildModelMenu()
{
    QMenu *menu = m_modelBtn->menu();
    menu->clear();
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    auto *group = new QActionGroup(menu);
    auto add = [&](AiEnv::Provider p, const AiModels::Info *models, int count) {
        menu->addSection(AiEnv::providerName(p));
        const QIcon icon = Icons::tinted(AiModels::providerIcon(p), AiModels::providerColor(t, p));
        for (int i = 0; i < count; ++i) {
            const QString id = QString::fromLatin1(models[i].id);
            QAction *a = menu->addAction(icon, QString::fromLatin1(models[i].label));
            a->setToolTip(id);
            a->setCheckable(true);
            a->setActionGroup(group);
            a->setChecked(id == m_model);
            if (id == m_model) { // the icon takes the place of the check mark
                QFont f = a->font();
                f.setBold(true);
                a->setFont(f);
            }
            connect(a, &QAction::triggered, this, [this, id] {
                m_model = id;
                SettingsManager::instance().setInlineModel(id);
                updateModelButton();
                if (m_keyBtn->isVisible() && !AiEnv::apiKey(AiEnv::providerForModel(id), m_root).isEmpty()) {
                    m_keyBtn->hide();
                    setStatus(QString());
                }
                m_input->setFocus();
            });
        }
    };
    add(AiEnv::Provider::Gemini, AiModels::kGemini, int(std::size(AiModels::kGemini)));
    add(AiEnv::Provider::Claude, AiModels::kClaude, int(std::size(AiModels::kClaude)));
}

void InlineEdit::updateTitle()
{
    if (!m_editor)
        return;
    QTextDocument *td = m_editor->document();
    const int first = td->findBlock(m_start).blockNumber() + 1;
    const int last = td->findBlock(m_start + m_origLen).blockNumber() + 1;
    if (m_insertMode)
        m_title->setText(tr("Ask QODE · insert at line %1").arg(first));
    else if (first == last)
        m_title->setText(tr("Ask QODE · line %1").arg(first));
    else
        m_title->setText(tr("Ask QODE · lines %1–%2").arg(first).arg(last));
}

void InlineEdit::setStatus(const QString &text, bool error)
{
    m_status->setText(text);
    if (m_statusError != error) {
        m_statusError = error;
        m_status->setProperty("error", error);
        m_status->style()->unpolish(m_status);
        m_status->style()->polish(m_status);
    }
    m_statusRow->setVisible(!text.isEmpty() || m_state != State::Idle || m_keyBtn->isVisible());
    reposition();
}

void InlineEdit::setState(State state)
{
    m_state = state;
    const bool running = state == State::Running;
    m_stopBtn->setVisible(running);
    m_acceptBtn->setVisible(state == State::Proposed);
    m_rejectBtn->setVisible(state == State::Proposed);
    m_sendBtn->setEnabled(!running && !m_input->toPlainText().trimmed().isEmpty());
    m_input->setReadOnly(running);
    if (state == State::Idle && !m_asked)
        m_input->setPlaceholderText(m_insertMode ? tr("Describe the code to write here, or ask a question…")
                                                 : tr("Describe the change, or ask a question about these lines…"));
    else
        m_input->setPlaceholderText(tr("Ask for a follow-up change…"));
    if (running) {
        m_clock.start();
        m_activity = tr("Thinking…");
        m_clockTimer->start();
    } else {
        m_clockTimer->stop();
    }
    m_statusRow->setVisible(state != State::Idle || !m_status->text().isEmpty() || m_keyBtn->isVisible());
    reposition();
}

void InlineEdit::fitInput()
{
    const QFontMetrics fm(m_input->font());
    const int lines = qBound(1, int(m_input->document()->documentLayout()->documentSize().height()), 6); // lines, in a plain text layout
    const int h = lines * fm.lineSpacing() + 14;
    m_input->setVerticalScrollBarPolicy(lines >= 6 ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    if (m_input->maximumHeight() != h) { // not height(): an unshown widget may already be that tall
        m_input->setFixedHeight(h);
        reposition();
    }
}

void InlineEdit::reposition()
{
    if (!m_editor)
        return;
    QWidget *vp = m_editor->viewport();
    const QRect area = vp->geometry(); // in editor coordinates
    const int w = qBound(qMin(360, area.width() - 16), 620, area.width() - 24);
    layout()->activate();
    resize(w, layout()->hasHeightForWidth() ? layout()->heightForWidth(w) : sizeHint().height());
    QPoint pos;
    if (m_dragged) {
        pos = m_dragPos;
    } else {
        QTextCursor c(m_editor->document());
        c.setPosition(qMin(m_start + m_origLen + m_newLen, m_editor->document()->characterCount() - 1));
        const QRect below = m_editor->cursorRect(c);
        c.setPosition(m_start);
        const QRect above = m_editor->cursorRect(c);
        const int x = area.left() + 24;
        int y = area.top() + below.bottom() + 6;
        if (y + height() > area.bottom() - 4 && area.top() + above.top() - height() - 6 >= area.top() + 4)
            y = area.top() + above.top() - height() - 6;
        pos = QPoint(x, y);
    }
    pos.setX(qBound(area.left() + 4, pos.x(), qMax(area.left() + 4, area.right() - width() - 4)));
    pos.setY(qBound(area.top() + 4, pos.y(), qMax(area.top() + 4, area.bottom() - height() - 4)));
    move(pos);
    raise();
}

// --- request -------------------------------------------------------------------------------------------------------

QString InlineEdit::firstMessage(const QString &instruction) const
{
    const QString text = m_editor->document()->toPlainText();
    QString before = text.left(m_start);
    QString marked = text.mid(m_start, m_origLen);
    QString after = text.mid(m_start + m_origLen);
    QString note;
    if (text.size() > kMaxFileChars) {
        // Keep kWindowLines on each side.
        const QStringList pre = before.split(QLatin1Char('\n'));
        const QStringList post = after.split(QLatin1Char('\n'));
        const int totalLines = int(text.count(QLatin1Char('\n'))) + 1;
        if (pre.size() > kWindowLines)
            before = pre.mid(pre.size() - kWindowLines).join(QLatin1Char('\n'));
        if (post.size() > kWindowLines)
            after = post.mid(0, kWindowLines).join(QLatin1Char('\n'));
        note = QStringLiteral("(The file has %1 lines; only the part around the marked lines is shown.)\n").arg(totalLines);
    }

    const QString path = m_doc ? m_doc->filePath() : QString();
    QString shown = path.isEmpty() ? QStringLiteral("untitled") : path;
    if (!m_root.isEmpty() && !path.isEmpty() && QDir::cleanPath(path).startsWith(QDir::cleanPath(m_root) + QLatin1Char('/')))
        shown = QDir(m_root).relativeFilePath(path);
    const QString language = m_doc ? m_doc->languageName() : QString();

    QString msg = QStringLiteral("File: %1").arg(shown);
    if (!language.isEmpty())
        msg += QStringLiteral(" (%1)").arg(language);
    msg += QLatin1Char('\n') + note + QStringLiteral("\n<file>\n");
    const int first = m_editor->document()->findBlock(m_start).blockNumber() + 1;
    const int last = m_editor->document()->findBlock(m_start + m_origLen).blockNumber() + 1;
    if (m_insertMode) {
        msg += before + QStringLiteral("<<<INSERT HERE>>>") + after;
        msg += QStringLiteral("\n</file>\n\nThe code goes at <<<INSERT HERE>>> (line %1, an empty line").arg(first);
        if (!marked.isEmpty())
            msg += QStringLiteral(" indented with \"%1\"").arg(marked);
        msg += QStringLiteral(").\n\n");
    } else {
        msg += before + QStringLiteral("<<<MARKED LINES START>>>\n") + marked + QStringLiteral("\n<<<MARKED LINES END>>>") + after;
        msg += QStringLiteral("\n</file>\n\nThe marked lines (%1–%2), which your reply replaces:\n<marked>\n%3\n</marked>\n\n").arg(first).arg(last).arg(marked);
    }
    msg += QStringLiteral("Instruction: ") + instruction;
    return msg;
}

void InlineEdit::send()
{
    if (!m_editor || m_state == State::Running)
        return;
    const QString instruction = m_input->toPlainText().trimmed();
    if (instruction.isEmpty())
        return;
    const AiEnv::Provider provider = AiEnv::providerForModel(m_model);
    const QString key = AiEnv::apiKey(provider, m_root);
    if (key.isEmpty()) {
        m_keyBtn->show();
        setStatus(tr("No %1 API key is set up.").arg(AiEnv::providerName(provider)), true);
        return;
    }
    m_keyBtn->hide();
    if (m_doc && !m_doc->filePath().isEmpty() && AiEnv::isSensitive(m_doc->filePath())) {
        setStatus(tr("%1 may hold secrets, so it is never sent to a model.").arg(QFileInfo(m_doc->filePath()).fileName()), true);
        return;
    }

    if (m_contents.isEmpty()) {
        m_contents.append(QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", firstMessage(instruction)}}}}});
    } else {
        const QString follow = m_hasProposal ? QStringLiteral("%1\n\n(Apply this to your last version. Reply again with the complete new text for the marked "
                                                              "lines, following the same rules.)")
                                             : QStringLiteral("%1\n\n(Follow the same rules: the new text for the marked lines, or @@answer for a question.)");
        m_contents.append(QJsonObject{{"role", "user"}, {"parts", QJsonArray{QJsonObject{{"text", follow.arg(instruction)}}}}});
    }
    m_lastInstruction = instruction;
    m_asked = true;
    m_reply.clear();
    m_answerMode = false;
    if (!m_locked) {
        m_locked = true;
        m_wasReadOnly = m_editor->isReadOnly();
        m_editor->setReadOnly(true);
        if (m_doc)
            m_doc->setSaveBlock(tr("Accept or reject the inline edit in %1 before saving.").arg(m_doc->fileName()));
    }
    m_input->clear();
    m_input->setPlaceholderText(instruction);
    setStatus(QString());
    setState(State::Running);
    m_client->start(key, m_model, systemPrompt(), m_contents);
}

void InlineEdit::onDelta(const QString &text)
{
    m_reply += text;
    m_activity = tr("Writing…");
    if (!m_flushTimer->isActive())
        m_flushTimer->start();
}

void InlineEdit::flush(bool final)
{
    const QString lead = m_reply.trimmed();
    if (!m_answerMode) {
        if (lead.startsWith(kAnswerTag)) {
            m_answerMode = true;
        } else if (!final && QString(kAnswerTag).startsWith(lead)) {
            return; // could still become "@@answer"
        }
    }
    if (m_answerMode) {
        m_answer->setMarkdown(lead.mid(kAnswerTag.size()).trimmed());
        m_answer->show();
        m_answer->document()->setTextWidth(m_answer->viewport()->width() > 50 ? m_answer->viewport()->width() : width() - 40);
        m_answer->setFixedHeight(qMin(220, int(m_answer->document()->size().height()) + 14));
        m_answer->verticalScrollBar()->setValue(m_answer->verticalScrollBar()->maximum());
        reposition();
        return;
    }
    QString text = cleanReply(m_reply, final);
    if (!m_editor->document()->findBlock(m_start).text().trimmed().isEmpty() || m_insertMode)
        while (text.startsWith(QLatin1Char('\n')))
            text.remove(0, 1); // a blank line the model put in front
    showProposal(text);
}

void InlineEdit::showProposal(const QString &text)
{
    if (!m_editor || (m_hasProposal && text == m_shown))
        return;
    if (!m_hasProposal && text.isEmpty() && m_reply.trimmed().isEmpty())
        return;
    QTextCursor c(m_editor->document());
    m_applying = true;
    if (!m_blockOpen) {
        c.beginEditBlock();
        m_blockOpen = true;
    } else {
        c.joinPreviousEditBlock();
    }
    const int at = m_start + m_origLen;
    if (m_hasProposal && !m_shown.isEmpty() && text.startsWith(m_shown)) {
        c.setPosition(at + m_newLen);
        const QString more = text.mid(m_shown.size());
        c.insertText(more);
        m_newLen += int(more.size());
    } else {
        c.setPosition(at);
        c.setPosition(at + m_newLen, QTextCursor::KeepAnchor);
        const QString part = text.isEmpty() ? QString() : QStringLiteral("\n") + text;
        c.insertText(part);
        m_newLen = int(part.size());
    }
    c.endEditBlock();
    m_applying = false;
    m_shown = text;
    m_hasProposal = true;
    takeSnapshot();
    updateMarks();
    reposition();
}

void InlineEdit::onFinished(bool ok, const QString &error, const QString &note)
{
    m_flushTimer->stop();
    if (ok || !m_reply.isEmpty())
        flush(true);
    const double seconds = m_clock.elapsed() / 1000.0;
    if (!m_reply.trimmed().isEmpty()) // also a stopped reply: a follow-up refers to what is shown
        m_contents.append(QJsonObject{{"role", "model"}, {"parts", QJsonArray{QJsonObject{{"text", m_reply}}}}});
    else if (!m_contents.isEmpty())
        m_contents.removeLast(); // nothing came: asking again re-sends the request

    const QString took = QStringLiteral("%1 · %2s").arg(AiModels::label(m_model), QString::number(seconds, 'f', 1));
    if (!ok) {
        if (m_reply.trimmed().isEmpty() && m_input->toPlainText().isEmpty())
            m_input->setPlainText(m_lastInstruction); // nothing came of it: the prompt is back for another try
        setState(m_hasProposal ? State::Proposed : m_answerMode ? State::Answered : State::Idle);
        setStatus(error, error != tr("Stopped."));
    } else if (m_answerMode) {
        setState(m_hasProposal ? State::Proposed : State::Answered);
        setStatus(took);
    } else if (m_reply.trimmed().isEmpty()) {
        setState(m_hasProposal ? State::Proposed : State::Idle);
        setStatus(tr("The model sent an empty reply."), true);
    } else {
        setState(State::Proposed);
        const QString orig = m_editor ? m_editor->document()->toPlainText().mid(m_start, m_origLen) : QString();
        if (m_shown == orig)
            setStatus(tr("%1 — no change proposed").arg(took));
        else
            setStatus(note.isEmpty() ? took : took + QStringLiteral(" — ") + note, !note.isEmpty());
    }
    if (!m_hasProposal)
        releaseEditor(); // an answer or a failure: the lines can be edited again
    m_input->setFocus();
}

// --- the proposal in the document ---------------------------------------------------------------------------------

void InlineEdit::updateMarks()
{
    if (!m_editor)
        return;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QTextDocument *td = m_editor->document();
    QList<QTextEdit::ExtraSelection> marks;
    auto mark = [&](int from, int to, const QColor &color) {
        QTextBlock b = td->findBlock(from);
        const QTextBlock last = td->findBlock(to);
        while (b.isValid()) {
            QTextEdit::ExtraSelection sel;
            sel.format.setBackground(color);
            sel.format.setProperty(QTextFormat::FullWidthSelection, true);
            sel.cursor = QTextCursor(b);
            marks.append(sel);
            if (b == last)
                break;
            b = b.next();
        }
    };
    if (m_hasProposal) {
        if (!m_insertMode)
            mark(m_start, m_start + m_origLen, t.diffDelBg);
        if (m_newLen > 0)
            mark(m_start + m_origLen + 1, m_start + m_origLen + m_newLen, t.diffAddBg);
    } else {
        QColor target = t.accent;
        target.setAlpha(t.dark ? 34 : 26);
        mark(m_start, m_start + m_origLen, target);
    }
    m_editor->setInlineMarks(marks);
}

void InlineEdit::documentChanged(int position, int removed, int added)
{
    if (m_applying || !m_editor)
        return;
    const int end = m_start + m_origLen + m_newLen;
    if (position + removed <= m_start) {
        m_start += added - removed; // before the marked lines
    } else if (position >= end) {
        return; // after them
    } else if (removed == added && m_editor->document()->toPlainText().mid(m_start, end - m_start) == m_snapshot) {
        return; // only formats changed (highlighting reports itself as a change of the same length)
    } else if (!m_hasProposal) {
        m_origLen = qMax(0, m_origLen + added - removed); // only possible before the first request: the editor is locked then
        if (m_restoreAt > m_origLen)
            m_restoreAt = -1;
    } else {
        // Someone else (a tool, a reload) changed the lines under a proposal: leave the text as it is now.
        m_hasProposal = false;
        m_newLen = 0;
        emit statusMessage(tr("The file changed under the inline edit; its proposal was left in the text."));
        close(false);
        return;
    }
    takeSnapshot();
    updateTitle();
    updateMarks();
    reposition();
}

void InlineEdit::takeSnapshot()
{
    if (m_editor)
        m_snapshot = m_editor->document()->toPlainText().mid(m_start, m_origLen + m_newLen);
}

void InlineEdit::accept()
{
    if (!m_editor || !m_hasProposal || m_state == State::Running) {
        return;
    }
    QTextDocument *td = m_editor->document();
    QTextCursor c(td);
    m_applying = true;
    if (m_blockOpen)
        c.joinPreviousEditBlock();
    else
        c.beginEditBlock();
    if (m_newLen > 0) {
        c.setPosition(m_start);
        c.setPosition(m_start + m_origLen + 1, QTextCursor::KeepAnchor); // the old lines and the "\n" in front of the new ones
        c.removeSelectedText();
    } else { // the proposal is to delete the lines
        int from = m_start, to = m_start + m_origLen;
        if (to < td->characterCount() - 1)
            ++to;
        else if (from > 0)
            --from;
        c.setPosition(from);
        c.setPosition(to, QTextCursor::KeepAnchor);
        c.removeSelectedText();
    }
    c.endEditBlock();
    m_applying = false;
    QTextCursor caret(td);
    caret.setPosition(qMin(m_start + qMax(0, m_newLen - 1), td->characterCount() - 1));
    m_hasProposal = false;
    m_newLen = 0;
    emit statusMessage(tr("Inline edit applied (Ctrl+Z undoes it)"));
    CodeEditor *ed = m_editor;
    close(false);
    ed->setTextCursor(caret);
}

void InlineEdit::reject()
{
    if (!m_editor)
        return;
    m_client->disconnect(this);
    m_client->cancel();
    if (m_hasProposal) {
        QTextDocument *td = m_editor->document();
        m_applying = true;
        QTextCursor c(td);
        c.joinPreviousEditBlock();
        c.setPosition(m_start + m_origLen);
        c.setPosition(m_start + m_origLen + m_newLen, QTextCursor::KeepAnchor);
        c.removeSelectedText();
        c.endEditBlock();
        // The step now changes nothing; undoing it leaves the undo history and the modified state as they were.
        td->undo();
        td->clearUndoRedoStacks(QTextDocument::RedoStack);
        m_applying = false;
        m_hasProposal = false;
        m_newLen = 0;
    }
    close(false);
}

void InlineEdit::escape()
{
    if (m_state == State::Running)
        m_client->cancel();
    else if (m_hasProposal)
        reject();
    else
        close(true);
}

void InlineEdit::close(bool restoreTrigger)
{
    m_client->disconnect(this);
    m_client->cancel();
    CodeEditor *ed = m_editor;
    if (ed && restoreTrigger && !m_asked && m_restoreAt >= 0) {
        QTextCursor c(ed->document());
        c.setPosition(qMin(m_start + m_restoreAt, ed->document()->characterCount() - 1));
        m_applying = true;
        c.insertText(QStringLiteral("/?"));
        m_applying = false;
        ed->setTextCursor(c);
    }
    releaseEditor();
    if (ed) {
        ed->setInlineMarks({});
        ed->setFocus();
    }
    hide();
    deleteLater();
}

void InlineEdit::releaseEditor()
{
    if (!m_locked)
        return;
    m_locked = false;
    if (m_editor)
        m_editor->setReadOnly(m_wasReadOnly);
    if (m_doc)
        m_doc->setSaveBlock(QString());
}

// --- events --------------------------------------------------------------------------------------------------------

bool InlineEdit::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::ShortcutOverride || type == QEvent::KeyPress) {
        if (watched != m_input && watched != m_editor)
            return QFrame::eventFilter(watched, event);
        auto *key = static_cast<QKeyEvent *>(event);
        const Qt::KeyboardModifiers mods = key->modifiers() & ~Qt::KeypadModifier;
        const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
        const bool ours = key->key() == Qt::Key_Escape || (enter && mods == Qt::ControlModifier && m_state == State::Proposed) ||
                          (watched == m_input && enter && mods == Qt::NoModifier);
        if (type == QEvent::ShortcutOverride) {
            if (ours) {
                event->accept();
                return true;
            }
            return QFrame::eventFilter(watched, event);
        }
        if (key->key() == Qt::Key_Escape) {
            if (watched == m_editor && (m_editor->completionVisible() || m_editor->signatureVisible()))
                return false;
            escape();
            return true;
        }
        if (enter && mods == Qt::ControlModifier && m_state == State::Proposed) {
            accept();
            return true;
        }
        if (watched == m_input && enter && mods == Qt::NoModifier) {
            send();
            return true;
        }
        return QFrame::eventFilter(watched, event);
    }
    if (watched == m_input && (type == QEvent::FocusIn || type == QEvent::FocusOut)) {
        setProperty("focused", type == QEvent::FocusIn);
        style()->unpolish(this);
        style()->polish(this);
    }
    if (watched == m_editor && type == QEvent::Resize)
        QTimer::singleShot(0, this, &InlineEdit::reposition);
    if (watched == m_header) {
        if (type == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton) {
                m_dragging = true;
                m_dragOffset = me->globalPosition().toPoint() - pos();
                m_header->setCursor(Qt::ClosedHandCursor);
                return true;
            }
        } else if (type == QEvent::MouseMove && m_dragging) {
            auto *me = static_cast<QMouseEvent *>(event);
            m_dragPos = me->globalPosition().toPoint() - m_dragOffset;
            m_dragged = true;
            reposition();
            m_dragPos = pos();
            return true;
        } else if (type == QEvent::MouseButtonRelease && m_dragging) {
            m_dragging = false;
            m_header->setCursor(Qt::OpenHandCursor);
            return true;
        } else if (type == QEvent::MouseButtonDblClick) {
            m_dragged = false; // back to its place under the lines
            reposition();
            return true;
        }
    }
    return QFrame::eventFilter(watched, event);
}
