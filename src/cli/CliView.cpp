#include "CliView.h"

#include "Ansi.h"
#include "CliSession.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "terminal/TerminalView.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <algorithm>

namespace {

constexpr int kMaxHistory = 1000;
constexpr int kPopupRows = 8;

QColor cubeColor(int i)
{
    if (i < 232) {
        i -= 16;
        const int r = i / 36, g = (i / 6) % 6, b = i % 6;
        auto lvl = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return QColor(lvl(r), lvl(g), lvl(b));
    }
    const int v = 8 + (i - 232) * 10;
    return QColor(v, v, v);
}

QString formatDuration(qint64 ms)
{
    if (ms < 60000)
        return QStringLiteral("%1s").arg(double(ms) / 1000.0, 0, 'f', ms < 10000 ? 1 : 0);
    const qint64 s = ms / 1000;
    return QStringLiteral("%1m %2s").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

} // namespace

const TerminalScreen::Line *CliView::Block::line(int i) const
{
    if (i < 0)
        return nullptr;
    if (i < head.size())
        return &head.at(i);
    i -= int(head.size());
    const int middle = live ? liveRows : int(body.size());
    if (i < middle) {
        if (live)
            return i < live->totalLines() ? &live->lineAt(i) : nullptr;
        return &body.at(i);
    }
    i -= middle;
    return i < foot.size() ? &foot.at(i) : nullptr;
}

// --- Construction ---------------------------------------------------------------------------

CliView::CliView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->setAutoFillBackground(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    verticalScrollBar()->setSingleStep(1);

    m_session = new CliSession(this);
    connect(m_session, &CliSession::output, this, [this](const QByteArray &data) {
        if (m_running && m_running->raw && m_running->live)
            m_running->live->feed(data);
    });
    connect(m_session, &CliSession::commandFinished, this, [this](int code, qint64 ms) {
        if (m_running && m_running->raw) {
            Block *b = m_running;
            m_running = nullptr;
            freezeBlock(b, code, ms);
        }
        refreshBranch();
    });
    connect(m_session, &CliSession::backgroundOutput, this, &CliView::onBackground);
    connect(m_session, &CliSession::stateChanged, this, &CliView::onSessionState);
    connect(m_session, &CliSession::startupOutput, this, [this](const QString &text) {
        addNotice(Ansi::dim(Ansi::sanitize(text.trimmed())));
    });
    connect(m_session, &CliSession::cwdChanged, this, [this] {
        refreshBranch();
        rebuildInput();
        viewport()->update();
    });
    connect(m_session, &CliSession::exited, this, [this](int code) {
        if (m_running && m_running->raw) {
            Block *b = m_running;
            m_running = nullptr;
            freezeBlock(b, code, 0);
        }
        addNotice(Ansi::paint(QStringLiteral("The shell exited (code %1). Press Enter to start a new one.").arg(code), Ansi::Yellow));
    });

    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int v) {
        m_follow = v >= verticalScrollBar()->maximum();
        viewport()->update();
    });
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, &CliView::applySettings);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { applySettings(); });
    applySettings();
}

CliView::~CliView()
{
    qDeleteAll(m_blocks);
}

void CliView::setToggleShortcut(const QString &text)
{
    m_toggleText = text;
    m_toggleKeys = QKeySequence(text);
}

void CliView::applySettings()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_bg = t.termBg;
    m_fg = t.termFg;
    m_sel = t.selection;
    m_accent = t.accent;
    m_muted = t.textMuted;
    m_panel = t.panel;
    m_border = t.border;
    m_ok = t.success;
    m_warn = t.warning;
    m_bad = t.danger;
    for (int i = 0; i < 16; ++i)
        m_ansi[i] = t.ansi[i];
    QPalette p = palette();
    p.setColor(QPalette::Base, m_bg);
    setPalette(p);
    m_font = SettingsManager::instance().editorFont();
    updateMetrics();
    recalc();
    rebuildInput();
    relayout();
}

void CliView::updateMetrics()
{
    const QFontMetricsF fm(m_font);
    m_cw = fm.horizontalAdvance(QLatin1Char('M'));
    m_ch = qCeil(fm.height());
    m_ascent = qRound(fm.ascent());
    m_barH = int(m_ch) + 10;
}

void CliView::recalc()
{
    const int cols = qMax(20, int((viewport()->width() - 2 * m_padX) / m_cw));
    const int rows = qMax(3, int((viewport()->height() - m_padTop - m_barH) / m_ch));
    if (cols == m_cols && rows == m_visRows)
        return;
    m_cols = cols;
    m_visRows = rows;
    if (m_session->state() != CliSession::State::Stopped)
        m_session->resize(cols, rows);
    for (Block *b : m_blocks)
        if (b->live)
            b->live->resize(cols, rows);
    rebuildInput();
    relayout();
}

void CliView::resizeEvent(QResizeEvent *e)
{
    QAbstractScrollArea::resizeEvent(e);
    recalc();
    relayout();
}

// --- Project and session ---------------------------------------------------------------------------

void CliView::setProject(const QString &root, const QString &name)
{
    if (root == m_root && name == m_name)
        return;
    m_root = root;
    m_name = name;
    m_started = false;
    if (m_call)
        m_call->cancel();
    m_running = nullptr;
    m_clearAfter = false;
    wipe();
    m_input.clear();
    m_cursor = 0;
    m_branch.clear();
    hidePopup();
    loadHistory();
    if (m_session->state() != CliSession::State::Stopped) {
        m_startPending = !root.isEmpty();
        m_session->stop();
    }
    rebuildInput();
    relayout();
}

QString CliView::cwd() const
{
    const QString c = m_session->cwd();
    return c.isEmpty() ? m_root : c;
}

QString CliView::shellName() const
{
    return m_session->shellName().isEmpty() ? QStringLiteral("shell") : m_session->shellName();
}

bool CliView::hasRunningProgram() const
{
    return m_session->state() == CliSession::State::Running;
}

void CliView::activate()
{
    if (m_root.isEmpty())
        return;
    if (!m_started) {
        m_started = true;
        showBanner();
    }
    ensureSession();
    setFocus();
    scrollToEnd();
}

void CliView::ensureSession()
{
    if (m_root.isEmpty() || m_session->state() != CliSession::State::Stopped)
        return;
    if (m_restartPending || m_startPending)
        return; // the old process is still going away; onSessionState starts the new one
    startSession();
}

void CliView::startSession()
{
    QString error;
    if (!m_session->start(m_root, &error)) {
        addNotice(Ansi::paint(error, Ansi::Red));
        return;
    }
    m_session->resize(m_cols, m_visRows);
    m_runStartup = true;
    refreshBranch();
}

void CliView::onSessionState()
{
    if (m_session->state() == CliSession::State::Stopped && (m_restartPending || m_startPending)) {
        m_restartPending = false;
        m_startPending = false;
        if (!m_root.isEmpty())
            startSession();
    }
    if (m_session->state() == CliSession::State::Idle && m_runStartup) {
        m_runStartup = false;
        const QString command = m_startupProvider ? m_startupProvider(shellName()) : QString();
        if (!command.isEmpty())
            m_session->runHidden(command, [](int, const QString &) {});
    }
    rebuildInput();
    relayout();
}

void CliView::restartShell()
{
    if (m_session->state() == CliSession::State::Stopped) {
        startSession();
        return;
    }
    m_restartPending = true;
    m_session->stop();
}

void CliView::changeDirectory(const QString &directory, std::function<void(int, const QString &)> done)
{
    const QString line = directory == QLatin1String("-") ? QStringLiteral("cd -") : QStringLiteral("cd -- ") + CliSession::quote(directory);
    m_session->runHidden(line, std::move(done));
}

void CliView::showBanner()
{
    addNotice(Ansi::paint(QStringLiteral("QODE"), Ansi::Blue, true) + Ansi::dim(QStringLiteral(" terminal mode")) + QStringLiteral("\n") +
              Ansi::dim(QStringLiteral("project ")) + Ansi::paint(m_name, Ansi::Cyan, true) + Ansi::dim(QStringLiteral("  ·  ") + m_root) + QStringLiteral("\n") +
              QStringLiteral("Type ") + Ansi::paint(QStringLiteral("/help"), Ansi::Cyan, true) + QStringLiteral(" for commands and keys, ") +
              Ansi::paint(QStringLiteral("/editor"), Ansi::Cyan, true) + (m_toggleText.isEmpty() ? QString() : Ansi::dim(QStringLiteral(" (%1)").arg(m_toggleText))) +
              QStringLiteral(" to go back.\n") + Ansi::dim(QStringLiteral("Anything else you type runs in your shell, like in any terminal.")));
}

// --- Blocks ------------------------------------------------------------------------------------------

CliView::Block *CliView::addBlock(const QString &headerAnsi, bool raw, bool slash)
{
    if (m_bgBlock) {
        Block *old = m_bgBlock;
        m_bgBlock = nullptr;
        freezeBlock(old, 0, 0);
    }
    auto *b = new Block;
    b->raw = raw;
    b->slash = slash;
    b->head = Ansi::toLines(headerAnsi, m_cols);
    b->live = new TerminalScreen(m_cols, m_visRows, this);
    connect(b->live, &TerminalScreen::changed, this, [this, b] { onLiveChanged(b); });
    if (raw)
        connect(b->live, &TerminalScreen::reply, m_session, &CliSession::write);
    m_blocks.append(b);
    m_follow = true;
    relayout();
    return b;
}

void CliView::addNotice(const QString &ansi)
{
    auto *b = new Block;
    b->body = Ansi::toLines(ansi, m_cols);
    m_blocks.append(b);
    m_follow = true;
    rebuildInput();
    relayout();
}

void CliView::onBackground(const QByteArray &data)
{
    if (QString::fromUtf8(data).trimmed().isEmpty())
        return;
    if (!m_bgBlock || m_blocks.isEmpty() || m_blocks.last() != m_bgBlock)
        m_bgBlock = addBlock(Ansi::dim(QStringLiteral("↳ output from the shell")), false, true);
    if (m_bgBlock->live)
        m_bgBlock->live->feed(data);
}

void CliView::onLiveChanged(Block *b)
{
    TerminalScreen *s = b->live;
    if (!s)
        return;
    const int sb = s->scrollbackSize();
    int g = s->rows() - 1;
    while (g >= 0 && Ansi::isBlank(s->lineAt(sb + g)))
        --g;
    int rows = sb + g + 1;
    if (b->raw && s->cursorVisible())
        rows = qMax(rows, sb + s->cursorRow() + 1);
    b->liveRows = rows;
    relayout();
}

void CliView::freezeBlock(Block *b, int status, qint64 ms)
{
    if (b->live) {
        TerminalScreen *s = b->live;
        if (s->isAlternateScreen()) {
            // Killed inside a full-screen program: only what scrolled off before it is worth keeping.
            for (int i = 0; i < s->scrollbackSize(); ++i)
                b->body.append(s->lineAt(i));
        } else {
            b->body = Ansi::allLines(*s);
        }
        b->live = nullptr;
        delete s;
    }
    b->raw = false;
    if (!b->slash) {
        if (status != 0)
            b->foot = Ansi::toLines(Ansi::paint(QStringLiteral("✗ exit %1").arg(status), Ansi::Red, true) +
                                        (ms >= 1000 ? Ansi::dim(QStringLiteral(" · ") + formatDuration(ms)) : QString()),
                                    m_cols);
        else if (ms >= 1000)
            b->foot = Ansi::toLines(Ansi::paint(QStringLiteral("✓"), Ansi::Green) + Ansi::dim(QStringLiteral(" ") + formatDuration(ms)), m_cols);
    }
    if (m_clearAfter) {
        m_clearAfter = false;
        wipe();
    }
    rebuildInput();
    relayout();
    if (!m_queuedLines.isEmpty() || !m_queuedPartial.isEmpty())
        QTimer::singleShot(0, this, &CliView::drainTypeahead);
}

// Lines typed while a QODE command was busy run one after the other once it is done.
void CliView::drainTypeahead()
{
    if (!inputVisible())
        return; // still busy; the next block to finish tries again
    if (!m_queuedLines.isEmpty()) {
        submit(m_queuedLines.takeFirst());
        return;
    }
    m_input = m_queuedPartial;
    m_cursor = int(m_input.size());
    m_queuedPartial.clear();
    editedInput();
}

void CliView::wipe()
{
    if (m_running) {
        m_clearAfter = true; // a command is still printing: clear when it ends
        return;
    }
    qDeleteAll(m_blocks);
    m_blocks.clear();
    m_bgBlock = nullptr;
    clearSelection();
    m_follow = true;
    relayout();
}

void CliView::clearScreen()
{
    if (m_running) {
        m_clearAfter = true;
        return;
    }
    wipe();
}

QString CliView::cwdShort() const
{
    const QString d = cwd();
    if (d == m_root)
        return m_name;
    if (!m_root.isEmpty() && d.startsWith(m_root + QLatin1Char('/')))
        return m_name + d.mid(m_root.size());
    const QString home = QDir::homePath();
    if (d == home)
        return QStringLiteral("~");
    if (d.startsWith(home + QLatin1Char('/')))
        return QStringLiteral("~") + d.mid(home.size());
    return d;
}

void CliView::refreshBranch()
{
    if (cwd().isEmpty())
        return;
    const int gen = ++m_branchGeneration;
    auto *p = new QProcess(this);
    p->setWorkingDirectory(cwd());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    p->setProcessEnvironment(env);
    connect(p, &QProcess::finished, this, [this, p, gen](int code, QProcess::ExitStatus) {
        if (gen == m_branchGeneration) {
            QString branch;
            bool dirty = false;
            if (code == 0) {
                for (const QByteArray &l : p->readAllStandardOutput().split('\n')) {
                    if (l.startsWith("# branch.head "))
                        branch = QString::fromUtf8(l.mid(14)).trimmed();
                    else if (!l.isEmpty() && !l.startsWith('#'))
                        dirty = true;
                }
                if (branch == QLatin1String("(detached)"))
                    branch = QStringLiteral("detached");
            }
            if (branch != m_branch || dirty != m_dirty) {
                m_branch = branch;
                m_dirty = dirty;
                rebuildInput();
                viewport()->update();
            }
        }
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, p, [p](QProcess::ProcessError) { p->deleteLater(); });
    p->start(QStringLiteral("git"), {QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("--branch"), QStringLiteral("-uno")});
    QTimer::singleShot(4000, p, [p] {
        if (p->state() != QProcess::NotRunning)
            p->kill();
    });
}

bool CliView::rawMode() const
{
    return m_running && m_running->raw;
}

bool CliView::inputVisible() const
{
    return !m_root.isEmpty() && !m_call && !rawMode();
}

bool CliView::altScreen() const
{
    return rawMode() && m_running->live && m_running->live->isAlternateScreen();
}

// --- Submitting ---------------------------------------------------------------------------------------------

void CliView::submit(const QString &line)
{
    const QString text = line.trimmed();
    if (m_session->state() == CliSession::State::Stopped) {
        m_input.clear();
        m_cursor = 0;
        restartShell();
        return;
    }
    m_input.clear();
    m_cursor = 0;
    m_histPos = -1;
    hidePopup();
    if (text.isEmpty()) {
        rebuildInput();
        viewport()->update();
        return;
    }
    pushHistory(text);
    if (text.startsWith(QLatin1Char('/')))
        runSlash(text);
    else
        runShell(text);
    rebuildInput();
}

static QString headerFor(const QString &prefix, const QString &text, bool slash)
{
    const QString safe = Ansi::sanitize(text);
    QString body;
    if (slash) {
        const int sp = safe.indexOf(QLatin1Char(' '));
        body = Ansi::paint(sp < 0 ? safe : safe.left(sp), Ansi::Cyan, true) + (sp < 0 ? QString() : safe.mid(sp));
    } else {
        body = Ansi::bold() + safe + Ansi::reset();
    }
    return Ansi::dim(prefix) + QLatin1Char(' ') + Ansi::paint(QStringLiteral("❯"), Ansi::Green, true) + QLatin1Char(' ') + body;
}

void CliView::runShell(const QString &text)
{
    Block *b = addBlock(headerFor(cwdShort(), text, false), true, false);
    m_running = b;
    m_session->run(text);
}

void CliView::runSlash(const QString &text)
{
    const QStringList parts = CliCommands::splitArgs(text.mid(1));
    const QString name = parts.value(0);
    Block *b = addBlock(headerFor(cwdShort(), text, true), false, true);
    TerminalScreen *screen = b->live;

    const CliCommand *cmd = name.isEmpty() ? nullptr : CliCommands::instance().find(name);
    if (!cmd) {
        QStringList similar;
        for (const CliCommand &c : CliCommands::instance().all())
            if (!name.isEmpty() && c.name.startsWith(name.left(1)))
                similar << QLatin1Char('/') + c.name;
        screen->feed(Ansi::crlf(Ansi::paint(name.isEmpty() ? QStringLiteral("Type a command after the /. ") : QStringLiteral("Unknown command /%1. ").arg(name), Ansi::Red) +
                                Ansi::dim(QStringLiteral("/help lists them.")) + QLatin1Char('\n'))
                         .toUtf8());
        freezeBlock(b, 1, 0);
        return;
    }

    auto *call = new CliCall(*this, cmd->name, parts.mid(1), this);
    m_call = call;
    m_running = b;
    connect(call, &CliCall::output, this, [this, b](const QString &out) {
        if (b->live)
            b->live->feed(out.toUtf8());
    });
    connect(call, &CliCall::finished, this, [this, b, call](int status) {
        if (m_running == b)
            m_running = nullptr;
        m_call = nullptr;
        call->deleteLater();
        freezeBlock(b, status, 0);
        refreshBranch();
    });
    cmd->run(*call);
    if (!call->isAsync() && !call->isFinished())
        call->done();
}

// --- History ---------------------------------------------------------------------------------------------------

static QString historyFile(const QString &root)
{
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(root.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/QODE/cli-history/") + hash + QStringLiteral(".txt");
}

void CliView::loadHistory()
{
    m_history.clear();
    m_histPos = -1;
    if (m_root.isEmpty())
        return;
    QFile f(historyFile(m_root));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        m_history = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

void CliView::saveHistory() const
{
    if (m_root.isEmpty())
        return;
    const QString path = historyFile(m_root);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        f.write(m_history.join(QLatin1Char('\n')).toUtf8() + '\n');
}

void CliView::pushHistory(const QString &text)
{
    m_history.removeAll(text);
    m_history.append(text);
    while (m_history.size() > kMaxHistory)
        m_history.removeFirst();
    saveHistory();
}

void CliView::clearHistory()
{
    m_history.clear();
    saveHistory();
}

QString CliView::suggestion() const
{
    if (m_input.isEmpty() || m_cursor != m_input.size() || m_searching)
        return {};
    for (int i = int(m_history.size()) - 1; i >= 0; --i)
        if (m_history.at(i).size() > m_input.size() && m_history.at(i).startsWith(m_input))
            return m_history.at(i).mid(m_input.size());
    return {};
}

void CliView::historyStep(int direction)
{
    if (m_history.isEmpty())
        return;
    if (direction < 0) {
        if (m_histPos == -1) {
            m_draft = m_input;
            m_histPos = int(m_history.size()) - 1;
        } else if (m_histPos > 0) {
            --m_histPos;
        } else {
            return;
        }
        m_input = m_history.at(m_histPos);
    } else {
        if (m_histPos == -1)
            return;
        if (m_histPos < m_history.size() - 1) {
            ++m_histPos;
            m_input = m_history.at(m_histPos);
        } else {
            m_histPos = -1;
            m_input = m_draft;
        }
    }
    m_cursor = int(m_input.size());
    hidePopup();
    rebuildInput();
    scrollToEnd();
}

// --- Completion ----------------------------------------------------------------------------------------------------

void CliView::hidePopup()
{
    m_popup = false;
    m_cands.clear();
    m_candSel = -1;
    m_candCurrent.clear();
}

// Slash commands list their matches while you type; shell lines only on Tab.
void CliView::refreshPopup()
{
    if (m_input.startsWith(QLatin1Char('/')) && !m_searching) {
        const CliCompletion c = CliCommands::instance().complete(m_input, m_cursor, cwd());
        m_cands = c.items;
        m_candStart = c.start;
        m_candSel = -1;
        m_candCurrent = m_input.mid(c.start, m_cursor - c.start);
        m_popup = !m_cands.isEmpty();
        // Nothing to add once the word is complete.
        if (m_popup && m_cands.size() == 1 && m_cands.first().text == m_candCurrent)
            m_popup = false;
    } else {
        hidePopup();
    }
}

void CliView::editedInput()
{
    m_histPos = -1;
    refreshPopup();
    rebuildInput();
    scrollToEnd();
}

void CliView::applyCandidate(int index)
{
    const CliCandidate &c = m_cands.at(index);
    m_input.replace(m_candStart, m_candCurrent.size(), c.text);
    m_candCurrent = c.text;
    m_cursor = m_candStart + int(c.text.size());
    m_candSel = index;
}

// Tab: complete; with several matches insert the common start first, then step through them.
void CliView::tabComplete(int direction)
{
    const bool listWasShown = m_popup;
    if (!m_popup) {
        const CliCompletion c = CliCommands::instance().complete(m_input, m_cursor, cwd());
        if (c.items.isEmpty())
            return;
        m_cands = c.items;
        m_candStart = c.start;
        m_candSel = -1;
        m_candCurrent = m_input.mid(c.start, m_cursor - c.start);
        m_popup = true;
    }
    if (m_cands.size() == 1) {
        applyCandidate(0);
        const CliCandidate c = m_cands.first();
        const bool needsSpace = !c.directory && m_cursor == m_input.size() && !m_input.endsWith(QLatin1Char('/'));
        if (needsSpace) {
            m_input.insert(m_cursor, QLatin1Char(' '));
            ++m_cursor;
        }
        hidePopup();
        m_histPos = -1;
        if (c.directory)
            refreshPopup();
        rebuildInput();
        return;
    }
    if (m_candSel < 0 && direction > 0) {
        QString common = m_cands.first().text;
        for (const CliCandidate &c : m_cands) {
            int n = 0;
            while (n < common.size() && n < c.text.size() && common.at(n) == c.text.at(n))
                ++n;
            common.truncate(n);
        }
        if (common.size() > m_candCurrent.size()) {
            m_input.replace(m_candStart, m_candCurrent.size(), common);
            m_candCurrent = common;
            m_cursor = m_candStart + int(common.size());
            rebuildInput();
            return;
        }
    }
    if (!listWasShown && m_candSel < 0) {
        rebuildInput(); // several matches and nothing in common: show them first, the next Tab steps through
        return;
    }
    const int n = int(m_cands.size());
    applyCandidate(m_candSel < 0 ? (direction > 0 ? 0 : n - 1) : (m_candSel + direction + n) % n);
    rebuildInput();
}

// --- Reverse history search -----------------------------------------------------------------------------------------------

void CliView::startSearch()
{
    m_searching = true;
    m_searchQuery.clear();
    m_searchPos = -1;
    m_searchMatch.clear();
    m_searchSavedInput = m_input;
    hidePopup();
    rebuildInput();
}

void CliView::updateSearch(bool older)
{
    int from = older && m_searchPos >= 0 ? m_searchPos - 1 : int(m_history.size()) - 1;
    for (int i = from; i >= 0; --i) {
        if (m_history.at(i).contains(m_searchQuery, Qt::CaseInsensitive)) {
            m_searchPos = i;
            m_searchMatch = m_history.at(i);
            rebuildInput();
            return;
        }
    }
    if (!older || m_searchPos < 0) {
        m_searchPos = -1;
        m_searchMatch.clear();
    }
    rebuildInput();
}

void CliView::leaveSearch(bool accept)
{
    m_searching = false;
    if (accept && !m_searchMatch.isEmpty()) {
        m_input = m_searchMatch;
        m_cursor = int(m_input.size());
    } else {
        m_input = m_searchSavedInput;
        m_cursor = int(m_input.size());
    }
    refreshPopup();
    rebuildInput();
}

// Returns true when the key was consumed by the search prompt.
bool CliView::handleSearchKey(QKeyEvent *e)
{
    const Qt::KeyboardModifiers m = e->modifiers();
    const int key = e->key();
    if (key == Qt::Key_Escape || (m == Qt::ControlModifier && (key == Qt::Key_C || key == Qt::Key_G))) {
        leaveSearch(false);
        return true;
    }
    if (key == Qt::Key_Return || key == Qt::Key_Enter) {
        leaveSearch(true);
        return true;
    }
    if (m == Qt::ControlModifier && key == Qt::Key_R) {
        updateSearch(true);
        return true;
    }
    if (key == Qt::Key_Backspace) {
        if (!m_searchQuery.isEmpty())
            m_searchQuery.chop(1);
        updateSearch(false);
        return true;
    }
    const QString text = e->text();
    if (!(m & (Qt::ControlModifier | Qt::AltModifier)) && !text.isEmpty() && text.at(0).isPrint()) {
        m_searchQuery += text;
        updateSearch(false);
        return true;
    }
    // Any other key (arrows, Home…) accepts the match and then acts on it.
    leaveSearch(true);
    return false;
}

// --- Editing the input line ----------------------------------------------------------------------------------------------------

void CliView::insertText(const QString &text)
{
    if (text.isEmpty())
        return;
    m_input.insert(m_cursor, text);
    m_cursor += int(text.size());
    editedInput();
}

void CliView::deleteRange(int from, int to)
{
    if (from >= to)
        return;
    m_input.remove(from, to - from);
    m_cursor = from;
    editedInput();
}

void CliView::moveWord(int direction)
{
    int i = m_cursor;
    if (direction < 0) {
        while (i > 0 && !isWordChar(m_input.at(i - 1)))
            --i;
        while (i > 0 && isWordChar(m_input.at(i - 1)))
            --i;
    } else {
        while (i < m_input.size() && !isWordChar(m_input.at(i)))
            ++i;
        while (i < m_input.size() && isWordChar(m_input.at(i)))
            ++i;
    }
    m_cursor = i;
    hidePopup();
    rebuildInput();
}

void CliView::pasteIntoInput()
{
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    QString t = QApplication::clipboard()->text();
    if (t.isEmpty() && mime && mime->hasUrls()) {
        QStringList paths;
        for (const QUrl &u : mime->urls())
            paths << (u.isLocalFile() ? u.toLocalFile() : u.toString());
        t = paths.join(QLatin1Char(' '));
    }
    t.replace(QRegularExpression(QStringLiteral("\\s*[\\r\\n]+\\s*")), QStringLiteral(" "));
    insertText(t.trimmed());
}

// --- Keys ----------------------------------------------------------------------------------------------------------------------

bool CliView::event(QEvent *e)
{
    if (e->type() == QEvent::ShortcutOverride) {
        auto *k = static_cast<QKeyEvent *>(e);
        const bool passThrough = (k->key() == Qt::Key_Q && k->modifiers() == Qt::ControlModifier) || k->key() == Qt::Key_F11 ||
                                 (!m_toggleKeys.isEmpty() && QKeySequence(k->keyCombination()) == m_toggleKeys);
        if (!passThrough) {
            e->accept();
            return true;
        }
    } else if (e->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(e);
        if (k->key() == Qt::Key_Tab || k->key() == Qt::Key_Backtab) {
            keyPressEvent(k);
            return true;
        }
    }
    return QAbstractScrollArea::event(e);
}

// Scrolling and copy, valid in every mode. Returns true when handled.
bool CliView::handleCommonKey(QKeyEvent *e)
{
    const Qt::KeyboardModifiers m = e->modifiers();
    const int key = e->key();
    QScrollBar *sb = verticalScrollBar();
    if (m == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_C) {
        copySelection();
        clearSelection();
        return true;
    }
    if (m == Qt::ShiftModifier && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        sb->setValue(sb->value() + (key == Qt::Key_PageUp ? -1 : 1) * m_visRows / 2);
        return true;
    }
    if (!rawMode() && m == Qt::NoModifier && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        sb->setValue(sb->value() + (key == Qt::Key_PageUp ? -1 : 1) * qMax(1, m_visRows - 2));
        return true;
    }
    if (!rawMode() && m == Qt::ControlModifier && (key == Qt::Key_Home || key == Qt::Key_End)) {
        sb->setValue(key == Qt::Key_Home ? 0 : sb->maximum());
        return true;
    }
    return false;
}

void CliView::keyPressEvent(QKeyEvent *e)
{
    if (rawMode()) {
        handleRawKey(e);
        return;
    }
    if (m_call || !inputVisible()) {
        // A QODE command is running (or the shell is busy with a hidden command): only Ctrl+C and scrolling.
        if (e->modifiers() == Qt::ControlModifier && e->key() == Qt::Key_C && !selectionActive()) {
            m_queuedLines.clear();
            m_queuedPartial.clear();
            if (m_call)
                m_call->cancel();
            else
                m_session->interrupt();
            return;
        }
        if (handleCommonKey(e))
            return;
        if (m_call && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
                if (!m_queuedPartial.trimmed().isEmpty())
                    m_queuedLines << m_queuedPartial;
                m_queuedPartial.clear();
            } else if (e->key() == Qt::Key_Backspace) {
                m_queuedPartial.chop(1);
            } else if (!e->text().isEmpty() && e->text().at(0).isPrint()) {
                m_queuedPartial += e->text();
            }
        }
        return;
    }
    if (m_searching && handleSearchKey(e))
        return;
    handleInputKey(e);
}

void CliView::handleRawKey(QKeyEvent *e)
{
    const Qt::KeyboardModifiers m = e->modifiers();
    const int key = e->key();
    TerminalScreen *screen = m_running->live;
    if (m == Qt::ControlModifier && key == Qt::Key_C && selectionActive()) {
        copySelection();
        clearSelection();
        return;
    }
    if (handleCommonKey(e))
        return;
    if ((m == (Qt::ControlModifier | Qt::ShiftModifier) && key == Qt::Key_V) || (m == Qt::ControlModifier && key == Qt::Key_V) ||
        (m == Qt::ShiftModifier && key == Qt::Key_Insert)) {
        if (m == Qt::ControlModifier && TerminalView::clipboardIsImageOnly()) {
            m_session->write(QByteArrayLiteral("\x16")); // Claude Code reads the image itself on Ctrl+V
        } else {
            const QByteArray data = TerminalView::pasteBytes(screen && screen->bracketedPaste());
            if (!data.isEmpty())
                m_session->write(data);
        }
        scrollToEnd();
        return;
    }
    const QByteArray bytes = TerminalView::encodeKey(e, screen && screen->applicationCursorKeys());
    if (bytes.isEmpty()) {
        QAbstractScrollArea::keyPressEvent(e);
        return;
    }
    clearSelection();
    scrollToEnd();
    m_session->write(bytes);
}

void CliView::handleInputKey(QKeyEvent *e)
{
    const Qt::KeyboardModifiers m = e->modifiers();
    const int key = e->key();
    const bool ctrl = m & Qt::ControlModifier;
    const bool alt = m & Qt::AltModifier;
    const bool shift = m & Qt::ShiftModifier;

    if (handleCommonKey(e))
        return;

    // Clipboard
    if ((ctrl && shift && key == Qt::Key_V) || (m == Qt::ControlModifier && key == Qt::Key_V) || (m == Qt::ShiftModifier && key == Qt::Key_Insert)) {
        pasteIntoInput();
        return;
    }
    if (m == Qt::ControlModifier && key == Qt::Key_C) {
        if (selectionActive()) {
            copySelection();
            clearSelection();
        } else {
            m_input.clear();
            m_cursor = 0;
            m_histPos = -1;
            hidePopup();
            rebuildInput();
            scrollToEnd();
        }
        return;
    }

    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (!shift)
            submit(m_input);
        return;
    case Qt::Key_Tab:
        tabComplete(+1);
        scrollToEnd();
        return;
    case Qt::Key_Backtab:
        tabComplete(-1);
        scrollToEnd();
        return;
    case Qt::Key_Escape:
        hidePopup();
        rebuildInput();
        return;
    case Qt::Key_Up:
        if (m_popup && m_cands.size() > 1)
            tabComplete(-1);
        else
            historyStep(-1);
        return;
    case Qt::Key_Down:
        if (m_popup && m_cands.size() > 1)
            tabComplete(+1);
        else
            historyStep(+1);
        return;
    case Qt::Key_Left:
        if (ctrl || alt)
            moveWord(-1);
        else if (m_cursor > 0) {
            --m_cursor;
            hidePopup();
            rebuildInput();
        }
        return;
    case Qt::Key_Right:
    case Qt::Key_End:
        if (ctrl && key == Qt::Key_Right) {
            moveWord(+1);
            return;
        }
        if (m_cursor == m_input.size()) {
            const QString s = suggestion();
            if (!s.isEmpty()) {
                insertText(s);
                return;
            }
        }
        if (key == Qt::Key_Right && m_cursor < m_input.size())
            ++m_cursor;
        else if (key == Qt::Key_End)
            m_cursor = int(m_input.size());
        hidePopup();
        rebuildInput();
        return;
    case Qt::Key_Home:
        m_cursor = 0;
        hidePopup();
        rebuildInput();
        return;
    case Qt::Key_Backspace:
        if (ctrl || alt) {
            int i = m_cursor;
            while (i > 0 && !isWordChar(m_input.at(i - 1)))
                --i;
            while (i > 0 && isWordChar(m_input.at(i - 1)))
                --i;
            deleteRange(i, m_cursor);
        } else if (m_cursor > 0) {
            deleteRange(m_cursor - 1, m_cursor);
        }
        return;
    case Qt::Key_Delete:
        deleteRange(m_cursor, qMin(int(m_input.size()), m_cursor + 1));
        return;
    default:
        break;
    }

    if (ctrl && !alt) {
        switch (key) {
        case Qt::Key_A: m_cursor = 0; hidePopup(); rebuildInput(); return;
        case Qt::Key_E: m_cursor = int(m_input.size()); hidePopup(); rebuildInput(); return;
        case Qt::Key_B: if (m_cursor > 0) --m_cursor; rebuildInput(); return;
        case Qt::Key_F:
            if (m_cursor < m_input.size())
                ++m_cursor;
            rebuildInput();
            return;
        case Qt::Key_U: deleteRange(0, m_cursor); return;
        case Qt::Key_K: deleteRange(m_cursor, int(m_input.size())); return;
        case Qt::Key_W: {
            int i = m_cursor;
            while (i > 0 && m_input.at(i - 1).isSpace())
                --i;
            while (i > 0 && !m_input.at(i - 1).isSpace())
                --i;
            deleteRange(i, m_cursor);
            return;
        }
        case Qt::Key_D:
            if (!m_input.isEmpty())
                deleteRange(m_cursor, qMin(int(m_input.size()), m_cursor + 1));
            return;
        case Qt::Key_L:
            wipe();
            return;
        case Qt::Key_R:
            startSearch();
            return;
        case Qt::Key_P:
            historyStep(-1);
            return;
        case Qt::Key_N:
            historyStep(+1);
            return;
        default:
            return;
        }
    }
    if (alt && !ctrl) {
        if (key == Qt::Key_B) { moveWord(-1); return; }
        if (key == Qt::Key_F) { moveWord(+1); return; }
        return;
    }

    const QString text = e->text();
    if (!text.isEmpty() && text.at(0).isPrint())
        insertText(text);
}

void CliView::inputMethodEvent(QInputMethodEvent *e)
{
    const QString commit = e->commitString();
    if (commit.isEmpty())
        return;
    if (rawMode())
        m_session->write(commit.toUtf8());
    else if (inputVisible() && !m_searching)
        insertText(commit);
}

// --- Input line rendering ----------------------------------------------------------------------------------------------------------

void CliView::rebuildInput()
{
    using Cell = TerminalScreen::Cell;
    m_inputLines.clear();
    Line cur;
    auto newLine = [&] {
        m_inputLines.append(cur);
        cur = Line();
    };
    auto put = [&](const QString &s, quint32 fg, quint8 attr = 0, quint32 bg = 0) {
        for (char32_t ch : s.toUcs4()) {
            if (cur.size() >= m_cols)
                newLine();
            Cell c;
            c.ch = ch;
            c.fg = fg;
            c.bg = bg;
            c.attr = attr;
            cur.append(c);
        }
    };
    const quint32 accent = packed(m_accent), muted = packed(m_muted);

    // Prompt information line
    switch (m_session->state()) {
    case CliSession::State::Stopped:
        put(QStringLiteral("shell not running — press Enter to start a new one"), packed(m_warn));
        break;
    case CliSession::State::Booting:
        put(QStringLiteral("starting shell…"), muted);
        break;
    default:
        put(cwdShort(), accent, TerminalScreen::Bold);
        if (!m_branch.isEmpty()) {
            put(QStringLiteral("  ⎇ ") + m_branch, muted);
            if (m_dirty)
                put(QStringLiteral(" ●"), packed(m_warn));
        }
        break;
    }
    newLine();

    // The line being edited
    int cursorRow = -1, cursorCol = 0;
    auto markCursor = [&] {
        if (cur.size() >= m_cols)
            newLine();
        cursorRow = int(m_inputLines.size());
        cursorCol = int(cur.size());
    };
    if (m_searching) {
        put(QStringLiteral("(search) "), packed(m_warn), TerminalScreen::Bold);
        put(m_searchQuery, 0, TerminalScreen::Underline);
        markCursor();
        put(QStringLiteral(": "), muted);
        if (!m_searchMatch.isEmpty())
            put(m_searchMatch, 0);
        else if (!m_searchQuery.isEmpty())
            put(QStringLiteral("no match"), packed(m_bad));
    } else {
        put(QStringLiteral("❯ "), packed(m_session->lastExitCode() == 0 ? m_ok : m_bad), TerminalScreen::Bold);
        int idx = 0;
        for (char32_t ch : m_input.toUcs4()) {
            if (idx == m_cursor)
                markCursor();
            put(QString::fromUcs4(&ch, 1), 0);
            idx += ch > 0xFFFF ? 2 : 1;
        }
        if (idx <= m_cursor)
            markCursor();
        put(suggestion(), muted);
    }
    newLine();

    // Candidates
    if (m_popup && !m_cands.isEmpty() && !m_searching) {
        int labelWidth = 0;
        for (const CliCandidate &c : m_cands)
            labelWidth = qMax(labelWidth, int(c.label.size()));
        const int total = int(m_cands.size());
        const int shown = qMin(total, kPopupRows);
        int first = 0;
        if (m_candSel >= shown)
            first = qMin(m_candSel - shown + 1, total - shown);
        for (int i = first; i < first + shown; ++i) {
            const CliCandidate &c = m_cands.at(i);
            const bool selected = i == m_candSel;
            const quint32 bg = selected ? packed(m_sel) : 0;
            auto clipped = [&](const QString &s, quint32 fg, quint8 attr) {
                for (char32_t ch : s.toUcs4()) {
                    if (cur.size() >= m_cols - 1)
                        return;
                    Cell cell;
                    cell.ch = ch;
                    cell.fg = fg;
                    cell.bg = bg;
                    cell.attr = attr;
                    cur.append(cell);
                }
            };
            clipped(QStringLiteral("  "), 0, 0);
            clipped(c.label + QString(labelWidth - int(c.label.size()), QLatin1Char(' ')), c.directory ? packed(m_ansi[Ansi::Blue]) : accent, TerminalScreen::Bold);
            if (!c.detail.isEmpty()) {
                clipped(QStringLiteral("  "), 0, 0);
                clipped(c.detail, muted, 0);
            }
            if (selected)
                while (cur.size() < m_cols) {
                    Cell cell;
                    cell.bg = bg;
                    cur.append(cell);
                }
            newLine();
        }
        if (total > shown) {
            put(QStringLiteral("  … %1 more · Tab / Shift+Tab to step through").arg(total - shown), muted);
            newLine();
        }
    }
    m_cursorRow = qMax(0, cursorRow);
    m_cursorCol = cursorCol;
}

// --- Layout -------------------------------------------------------------------------------------------------------------------------

void CliView::relayout()
{
    m_starts.resize(m_blocks.size());
    int y = 0;
    for (int i = 0; i < m_blocks.size(); ++i) {
        m_starts[i] = y;
        y += m_blocks.at(i)->rows() + 1;
    }
    m_blockRows = y;
    const int total = y + (inputVisible() ? int(m_inputLines.size()) : 0);
    const int maxValue = qMax(0, total - m_visRows);
    QScrollBar *sb = verticalScrollBar();
    const bool follow = m_follow;
    sb->blockSignals(true);
    sb->setRange(0, maxValue);
    sb->setPageStep(m_visRows);
    if (follow)
        sb->setValue(maxValue);
    sb->blockSignals(false);
    m_follow = follow;
    viewport()->update();
}

void CliView::scrollToEnd()
{
    m_follow = true;
    relayout();
}

int CliView::blockAt(int row) const
{
    // Last block whose start is <= row.
    auto it = std::upper_bound(m_starts.begin(), m_starts.end(), row);
    return int(it - m_starts.begin()) - 1;
}

const TerminalScreen::Line *CliView::flatLine(int row) const
{
    if (row < 0)
        return nullptr;
    if (row >= m_blockRows) {
        if (!inputVisible())
            return nullptr;
        const int i = row - m_blockRows;
        return i < m_inputLines.size() ? &m_inputLines.at(i) : nullptr;
    }
    const int bi = blockAt(row);
    if (bi < 0)
        return nullptr;
    return m_blocks.at(bi)->line(row - m_starts.at(bi));
}

QColor CliView::resolve(quint32 c, bool fg) const
{
    if (c == 0)
        return fg ? m_fg : m_bg;
    if (c & 0x01000000u)
        return QColor((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
    const int idx = int(c & 0xff);
    return idx < 16 ? m_ansi[idx] : cubeColor(idx);
}

void CliView::paintLine(QPainter &p, const Line &line, qreal y, int row, bool selectable)
{
    const bool hasSel = selectable && selectionActive();
    QPoint a = m_selA, b = m_selB;
    if (hasSel && qMakePair(a.y(), a.x()) > qMakePair(b.y(), b.x()))
        qSwap(a, b);
    for (int c = 0; c < line.size(); ++c) {
        const TerminalScreen::Cell &cell = line.at(c);
        QColor fg = resolve(cell.fg, true), bg = resolve(cell.bg, false);
        if (cell.attr & TerminalScreen::Inverse)
            qSwap(fg, bg);
        if ((cell.attr & TerminalScreen::Bold) && (cell.fg & 0x02000000u) && (cell.fg & 0xff) < 8)
            fg = m_ansi[(cell.fg & 0xff) + 8];
        if (cell.attr & TerminalScreen::Dim)
            fg.setAlphaF(0.6);
        bool selected = false;
        if (hasSel) {
            const QPair<int, int> pos(row, c);
            selected = pos >= qMakePair(a.y(), a.x()) && pos <= qMakePair(b.y(), b.x());
        }
        const QRectF rect(m_padX + c * m_cw, y, m_cw, m_ch);
        if (selected)
            p.fillRect(rect, m_sel);
        else if (bg != m_bg)
            p.fillRect(rect, bg);
        if (cell.ch != U' ') {
            QFont f = m_font;
            if (cell.attr & TerminalScreen::Bold) f.setBold(true);
            if (cell.attr & TerminalScreen::Italic) f.setItalic(true);
            if (cell.attr & TerminalScreen::Underline) f.setUnderline(true);
            if (f != m_font)
                p.setFont(f);
            p.setPen(fg);
            p.drawText(QPointF(rect.left(), y + m_ascent), QString::fromUcs4(&cell.ch, 1));
            if (f != m_font)
                p.setFont(m_font);
        } else if (cell.attr & TerminalScreen::Underline) {
            p.setPen(fg);
            p.drawLine(QPointF(rect.left(), y + m_ch - 1.5), QPointF(rect.right(), y + m_ch - 1.5));
        }
    }
}

void CliView::paintStatusBar(QPainter &p)
{
    const QRect bar(0, viewport()->height() - m_barH, viewport()->width(), m_barH);
    p.fillRect(bar, m_panel);
    p.setPen(m_border);
    p.drawLine(bar.topLeft(), bar.topRight());
    p.setFont(m_font);
    const qreal base = bar.top() + (m_barH - m_ch) / 2 + m_ascent;

    QColor dot = m_ok;
    QString state;
    switch (m_session->state()) {
    case CliSession::State::Stopped: dot = m_bad; state = QStringLiteral("shell stopped"); break;
    case CliSession::State::Booting: dot = m_warn; state = QStringLiteral("starting…"); break;
    case CliSession::State::Running:
        dot = m_warn;
        state = m_call ? QStringLiteral("running · Ctrl+C cancels") : (rawMode() ? QStringLiteral("running · Ctrl+C interrupts") : QStringLiteral("working…"));
        break;
    default: state = shellName(); break;
    }
    qreal x = m_padX;
    p.setBrush(dot);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(x + 4, bar.center().y() + 0.5), 4, 4);
    x += 16;
    p.setPen(m_fg);
    QFont bold = m_font;
    bold.setBold(true);
    p.setFont(bold);
    p.drawText(QPointF(x, base), m_name);
    x += QFontMetricsF(bold).horizontalAdvance(m_name) + m_cw;
    p.setFont(m_font);
    p.setPen(m_muted);
    const QString left = QStringLiteral("·  ") + state + (m_branch.isEmpty() ? QString() : QStringLiteral("  ·  ⎇ ") + m_branch);
    p.drawText(QPointF(x, base), left);

    QString hint = QStringLiteral("/help");
    if (!m_toggleText.isEmpty())
        hint += QStringLiteral("   ·   %1 editor").arg(m_toggleText);
    p.drawText(QPointF(viewport()->width() - m_padX - QFontMetricsF(m_font).horizontalAdvance(hint), base), hint);
}

void CliView::paintEvent(QPaintEvent *)
{
    QPainter p(viewport());
    p.fillRect(viewport()->rect(), m_bg);
    p.setFont(m_font);

    if (altScreen()) {
        // A full-screen program (vim, less, htop…) owns the whole view.
        TerminalScreen *s = m_running->live;
        const int sb = s->scrollbackSize();
        for (int r = 0; r < s->rows() && r < m_visRows; ++r)
            paintLine(p, s->lineAt(sb + r), m_padTop + r * m_ch, -1, false);
        if (s->cursorVisible()) {
            const QRectF cr(m_padX + s->cursorCol() * m_cw, m_padTop + s->cursorRow() * m_ch, m_cw, m_ch);
            if (m_focused)
                p.fillRect(cr, m_fg);
            else {
                p.setPen(m_fg);
                p.drawRect(cr.adjusted(0.5, 0.5, -0.5, -0.5));
            }
        }
        paintStatusBar(p);
        return;
    }

    const int first = verticalScrollBar()->value();
    for (int r = 0; r < m_visRows + 1; ++r) {
        const int row = first + r;
        if (const Line *line = flatLine(row))
            paintLine(p, *line, m_padTop + r * m_ch, row, true);
    }

    // Cursor: in the running program's output, or in the input line.
    int cursorRow = -1, cursorCol = 0;
    const Line *under = nullptr;
    if (rawMode() && m_running->live && m_running->live->cursorVisible()) {
        const int bi = int(m_blocks.indexOf(m_running));
        TerminalScreen *s = m_running->live;
        cursorRow = m_starts.value(bi) + int(m_running->head.size()) + s->scrollbackSize() + s->cursorRow();
        cursorCol = s->cursorCol();
    } else if (inputVisible() && !m_searching) {
        cursorRow = m_blockRows + m_cursorRow;
        cursorCol = m_cursorCol;
    } else if (inputVisible() && m_searching) {
        cursorRow = m_blockRows + m_cursorRow;
        cursorCol = m_cursorCol;
    }
    if (cursorRow >= first && cursorRow <= first + m_visRows) {
        under = flatLine(cursorRow);
        const QRectF cr(m_padX + cursorCol * m_cw, m_padTop + (cursorRow - first) * m_ch, m_cw, m_ch);
        if (m_focused) {
            p.fillRect(cr, m_fg);
            if (under && cursorCol < under->size() && under->at(cursorCol).ch != U' ') {
                p.setPen(m_bg);
                p.drawText(QPointF(cr.left(), cr.top() + m_ascent), QString::fromUcs4(&under->at(cursorCol).ch, 1));
            }
        } else {
            p.setPen(m_fg);
            p.drawRect(cr.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }

    // Hide anything that scrolled under the status bar, then draw it.
    p.fillRect(QRect(0, viewport()->height() - m_barH - 0, viewport()->width(), m_barH), m_bg);
    paintStatusBar(p);
}

// --- Focus, mouse, selection -----------------------------------------------------------------------------------------------------------

void CliView::focusInEvent(QFocusEvent *e)
{
    m_focused = true;
    QAbstractScrollArea::focusInEvent(e);
    viewport()->update();
}

void CliView::focusOutEvent(QFocusEvent *e)
{
    m_focused = false;
    QAbstractScrollArea::focusOutEvent(e);
    viewport()->update();
}

QPoint CliView::cellAt(const QPoint &pos) const
{
    const int col = qMax(0, int((pos.x() - m_padX) / m_cw));
    const int row = verticalScrollBar()->value() + qMax(0, int((pos.y() - m_padTop) / m_ch));
    return QPoint(col, row);
}

void CliView::clearSelection()
{
    m_selA = m_selB = QPoint(0, 0);
    viewport()->update();
}

QString CliView::selectedText() const
{
    if (!selectionActive())
        return {};
    QPoint a = m_selA, b = m_selB;
    if (qMakePair(a.y(), a.x()) > qMakePair(b.y(), b.x()))
        qSwap(a, b);
    QStringList lines;
    for (int r = a.y(); r <= b.y(); ++r) {
        const Line *line = flatLine(r);
        QString text = line ? Ansi::plainText(*line) : QString();
        const int from = r == a.y() ? a.x() : 0;
        const int to = r == b.y() ? b.x() + 1 : int(text.size());
        lines << text.mid(from, to - from);
    }
    return lines.join(QLatin1Char('\n'));
}

void CliView::copySelection()
{
    const QString t = selectedText();
    if (!t.isEmpty())
        QApplication::clipboard()->setText(t);
}

void CliView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_selecting = true;
        m_selA = m_selB = cellAt(e->pos());
        viewport()->update();
    } else if (e->button() == Qt::MiddleButton) {
        const QString t = QApplication::clipboard()->text(QClipboard::Selection);
        if (!t.isEmpty()) {
            if (rawMode())
                m_session->write(TerminalView::textPasteBytes(t, m_running->live && m_running->live->bracketedPaste()));
            else if (inputVisible()) {
                QString flat = t;
                flat.replace(QRegularExpression(QStringLiteral("\\s*[\\r\\n]+\\s*")), QStringLiteral(" "));
                insertText(flat.trimmed());
            }
        }
    }
    setFocus();
}

void CliView::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_selecting)
        return;
    m_selB = cellAt(e->pos());
    QScrollBar *sb = verticalScrollBar();
    if (e->pos().y() < 0)
        sb->setValue(sb->value() - 1);
    else if (e->pos().y() > viewport()->height() - m_barH)
        sb->setValue(sb->value() + 1);
    viewport()->update();
}

void CliView::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_selecting = false;
    if (selectionActive() && QApplication::clipboard()->supportsSelection())
        QApplication::clipboard()->setText(selectedText(), QClipboard::Selection);
}

void CliView::mouseDoubleClickEvent(QMouseEvent *e)
{
    const QPoint cell = cellAt(e->pos());
    const Line *line = flatLine(cell.y());
    if (!line)
        return;
    const QString text = Ansi::plainText(*line);
    int s = cell.x(), t = cell.x();
    if (s >= text.size())
        return;
    while (s > 0 && !text.at(s - 1).isSpace())
        --s;
    while (t < text.size() - 1 && !text.at(t + 1).isSpace())
        ++t;
    m_selA = QPoint(s, cell.y());
    m_selB = QPoint(t, cell.y());
    viewport()->update();
}

void CliView::wheelEvent(QWheelEvent *e)
{
    const int dy = e->angleDelta().y();
    if (dy == 0) {
        e->accept();
        return;
    }
    if (altScreen()) {
        // Same as the terminal panel: mouse reports if the program asked for them, else arrow keys.
        TerminalScreen *s = m_running->live;
        m_wheelAcc += dy;
        const int steps = m_wheelAcc / 40;
        m_wheelAcc -= steps * 40;
        const bool up = steps > 0;
        const QPoint cell = cellAt(e->position().toPoint());
        const int col = qBound(1, cell.x() + 1, 223), row = qBound(1, cell.y() - verticalScrollBar()->value() + 1, 223);
        QByteArray out;
        for (int i = 0; i < qAbs(steps); ++i) {
            if (s->mouseReporting()) {
                const int btn = up ? 64 : 65;
                if (s->sgrMouse())
                    out += QByteArray("\x1b[<") + QByteArray::number(btn) + ';' + QByteArray::number(col) + ';' + QByteArray::number(row) + 'M';
                else
                    out += QByteArray("\x1b[M") + char(32 + btn) + char(32 + col) + char(32 + row);
            } else {
                out += QByteArray(s->applicationCursorKeys() ? "\x1bO" : "\x1b[") + (up ? 'A' : 'B');
            }
        }
        if (!out.isEmpty())
            m_session->write(out);
        e->accept();
        return;
    }
    m_wheelAcc += dy;
    const int steps = m_wheelAcc / 40;
    m_wheelAcc -= steps * 40;
    QScrollBar *sb = verticalScrollBar();
    sb->setValue(sb->value() - steps * 3);
    e->accept();
}

void CliView::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    QAction *copy = menu.addAction(tr("Copy"), this, &CliView::copySelection);
    copy->setEnabled(selectionActive());
    QAction *paste = menu.addAction(tr("Paste"), this, [this] {
        if (rawMode()) {
            const QByteArray data = TerminalView::pasteBytes(m_running->live && m_running->live->bracketedPaste());
            if (!data.isEmpty())
                m_session->write(data);
        } else if (inputVisible()) {
            pasteIntoInput();
        }
    });
    paste->setEnabled(rawMode() || inputVisible());
    menu.addSeparator();
    menu.addAction(tr("Clear Screen"), this, [this] { wipe(); });
    menu.addAction(tr("Help"), this, [this] {
        if (inputVisible())
            submit(QStringLiteral("/help"));
    });
    menu.exec(e->globalPos());
}
