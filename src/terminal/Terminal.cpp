#include "Terminal.h"

#include "ShellProcess.h"
#include "ShellSyntax.h"
#include "TerminalScreen.h"
#include "TerminalView.h"
#include "filesystem/FileManager.h"
#include "platform/Platform.h"
#include "settings/Icons.h"

#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

Terminal::Terminal(QWidget *parent)
    : QWidget(parent)
{
    m_cwd = QDir::homePath();
    m_shell = new ShellProcess(this);
    m_view = new TerminalView(this);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_title->setStyleSheet(QStringLiteral("QLabel#panelTitle { border-bottom: none; }"));

    auto makeBtn = [this](const QString &text, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    auto *clearBtn = makeBtn(tr("Clear"), tr("Clear terminal (scrollback and screen)"));
    m_restartBtn = makeBtn(tr("Restart"), tr("Terminate the shell and start a new one"));
    auto *closeBtn = new QToolButton(this);
    Icons::bind(closeBtn, QStringLiteral(":/new-icons/x.svg"));
    closeBtn->setToolTip(tr("Hide terminal (Ctrl+J)"));
    closeBtn->setAutoRaise(true);

    m_header = new QWidget(this);
    QWidget *header = m_header;
    header->setObjectName(QStringLiteral("terminalHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->setSpacing(2);
    hl->addWidget(m_title, 1);
    hl->addWidget(clearBtn);
    hl->addWidget(m_restartBtn);
    hl->addWidget(closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#terminalHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); border-top: 1px solid palette(shadow); }"
                                         "QWidget#terminalHeader QLabel { background: transparent; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_view, 1);

    connect(m_view, &TerminalView::input, m_shell, &ShellProcess::write);
    // Enter at the shell's prompt starts a new command: the problems of the previous one go.
    connect(m_view, &TerminalView::input, this, [this](const QByteArray &data) {
        if (m_awaitPrompt && !m_inCommand) { // the prompt is drawn: the command line starts at the cursor
            m_awaitPrompt = false;
            m_promptLine = m_view->screen()->cursorStableLine();
            m_promptCol = m_view->screen()->cursorCol();
        }
        if (data.contains('\r') && shellInForeground())
            newCommand();
    });
    connect(m_view, &TerminalView::sizeChanged, m_shell, &ShellProcess::resize);
    connect(m_view, &TerminalView::focusGained, this, &Terminal::focused);
    connect(m_view, &TerminalView::returnPressedWhileInactive, this, &Terminal::restart);
    connect(m_view, &TerminalView::fileLinkActivated, this, &Terminal::openFileRequested);
    // Relative paths in compiler output are relative to where the command ran: the shell's current folder, else the project.
    auto folders = [this] {
        QStringList folders;
        if (const QString cwd = shellDirectory(); !cwd.isEmpty())
            folders << cwd;
        folders << m_cwd;
        return folders;
    };
    m_view->setLinkFolders(folders);
    m_problems.setFolders(folders);
    connect(m_shell, &ShellProcess::output, m_view->screen(), &TerminalScreen::feed);
    connect(m_shell, &ShellProcess::output, this, [this](const QByteArray &data) {
        const QList<BuildProblem> found = m_problems.feed(data);
        if (!found.isEmpty()) {
            m_hasProblems = true;
            if (m_inCommand && m_commandProblems.size() < 50)
                m_commandProblems += found.mid(0, 50 - m_commandProblems.size());
            emit problemsFound(found);
        }
    });
    connect(m_view->screen(), &TerminalScreen::shellMark, this, &Terminal::onShellMark);

    // Shown over the output when the last command failed; the next command takes it away.
    m_fixBtn = new QPushButton(tr("Fix with QODE"), m_view);
    m_fixBtn->setObjectName(QStringLiteral("primaryBtn"));
    m_fixBtn->setCursor(Qt::PointingHandCursor);
    m_fixBtn->hide();
    connect(m_fixBtn, &QPushButton::clicked, this, [this] {
        const FailedCommand failure = m_failure;
        setFailure({});
        emit fixRequested(failure);
    });
    m_view->installEventFilter(this);
    connect(m_shell, &ShellProcess::finished, this, &Terminal::onFinished);
    // A command typed before the shell has set up its line editor loses characters, so wait for startup
    // output to go quiet (or a fallback timeout) before typing it.
    m_pendingTimer = new QTimer(this);
    m_pendingTimer->setSingleShot(true);
    connect(m_pendingTimer, &QTimer::timeout, this, &Terminal::flushPendingCommand);
    connect(m_shell, &ShellProcess::output, this, [this] {
        if (m_booting)
            m_pendingTimer->start(250);
    });
    connect(clearBtn, &QToolButton::clicked, this, &Terminal::clear);
    connect(m_restartBtn, &QToolButton::clicked, this, &Terminal::restart);
    connect(closeBtn, &QToolButton::clicked, this, &Terminal::hideRequested);
    updateHeader();
}

void Terminal::setHeaderVisible(bool on)
{
    m_header->setVisible(on);
}

void Terminal::setShellProgram(const QString &program)
{
    m_program = program;
    m_shell->setLaunch(program, {});
}

QString Terminal::shellName() const
{
    // Never empty: a tab whose shell has not (re)started yet would otherwise flash a placeholder name.
    QString path = m_shell->shell();
    if (path.isEmpty())
        path = m_program.isEmpty() ? ShellProcess::defaultShell() : m_program;
    QString name = QFileInfo(path).fileName();
    if (name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) // Windows: "pwsh", not "pwsh.exe"
        name.chop(4);
    return name;
}

void Terminal::updateHeader()
{
    const QString state = m_shell->isRunning() ? QFileInfo(m_shell->shell()).fileName() : tr("not running");
    m_title->setText(tr("TERMINAL — %1 — %2").arg(state, FileManager::displayPath(m_cwd)));
}

void Terminal::setWorkingDirectory(const QString &dir)
{
    m_cwd = dir.isEmpty() || !QFileInfo(dir).isDir() ? QDir::homePath() : dir;
    updateHeader();
}

bool Terminal::isRunning() const
{
    return m_shell->isRunning();
}

void Terminal::ensureStarted()
{
    if (!m_shell->isRunning())
        startShell();
}

void Terminal::startShell()
{
    TerminalScreen *s = m_view->screen();
    s->reset();
    newCommand();
    m_pendingTimer->stop();
    m_pendingCommand.clear();
    m_booting = false;
    m_expectedExit = false;
    m_inCommand = false;
    m_awaitPrompt = true;
    m_promptLine = -1;
    m_written.clear();
    m_shellCwd.clear();
    QString err;
    m_view->setShellActive(true);
    if (!m_shell->start(m_cwd, s->cols(), s->rows(), &err)) {
        m_view->setShellActive(false);
        s->feed(QStringLiteral("\x1b[31mUnable to start shell: %1\x1b[0m\r\n").arg(err).toUtf8());
    } else {
        // Until the shell's startup output goes quiet it is not ready for input: anything typed (or Ctrl+C) now
        // would be lost or would kill it, so commands wait in m_pendingCommand.
        m_booting = true;
        m_pendingTimer->start(1500); // fallback if the shell prints nothing
        if (m_startupProvider)
            m_pendingCommand = m_startupProvider(shellName());
    }
    updateHeader();
}

void Terminal::onFinished(int exitCode)
{
    m_view->setShellActive(false);
    m_pendingTimer->stop();
    m_pendingCommand.clear();
    m_booting = false;
    if (m_expectedExit) { // stop() / restart(): no goodbye message, the caller decides what happens next
        m_expectedExit = false;
        updateHeader();
        return;
    }
    m_view->screen()->feed(QStringLiteral("\r\n\x1b[2m[Process exited with code %1 — press Enter to restart]\x1b[0m\r\n")
                               .arg(exitCode).toUtf8());
    updateHeader();
    emit shellExited();
}

void Terminal::stop()
{
    if (m_shell->isRunning()) {
        m_expectedExit = true;
        // Detach the output so the goodbye message isn't printed into a new session.
        m_shell->terminate();
    }
}

void Terminal::restart()
{
    if (m_shell->isRunning()) {
        m_expectedExit = true;
        m_shell->terminate();
        // Wait for the old shell to go away before spawning the replacement.
        auto *conn = new QMetaObject::Connection;
        *conn = connect(m_shell, &ShellProcess::finished, this, [this, conn] {
            disconnect(*conn);
            delete conn;
            startShell();
        });
        return;
    }
    startShell();
}

void Terminal::runCommand(const QString &shCommand)
{
    const bool wasRunning = m_shell->isRunning();
    ensureStarted();
    if (!m_shell->isRunning())
        return;
    const QString command = ShellSyntax::translate(shCommand, m_shell->shell()); // for PowerShell / cmd on Windows
    m_view->scrollToBottom();
    if (m_booting) { // a fresh shell (or its startup command) is still coming up: run after it
        if (!m_pendingCommand.isEmpty())
            m_pendingCommand += QLatin1Char('\r');
        m_pendingCommand += command;
        return;
    }
    Q_UNUSED(wasRunning)
    newCommand();
    m_pendingCommand = command;
    m_shell->write(QByteArray("\x03")); // Ctrl+C: stop a previous run / discard a half-typed line
    m_pendingTimer->start(150);
}

void Terminal::sendInput(const QByteArray &data)
{
    if (m_shell->isRunning())
        m_shell->write(data);
}

void Terminal::flushPendingCommand()
{
    m_booting = false;
    if (m_pendingCommand.isEmpty() || !m_shell->isRunning())
        return;
    m_shell->write(m_pendingCommand.toUtf8() + '\r'); // '\r' inside it separates queued commands
    m_written = m_pendingCommand.section(QLatin1Char('\r'), -1);
    m_pendingCommand.clear();
}

void Terminal::newCommand()
{
    setFailure({});
    m_problems.reset();
    if (m_hasProblems) {
        m_hasProblems = false;
        emit problemsCleared();
    }
}

// True while the shell itself owns the terminal (at its prompt), not a program it started.
bool Terminal::shellInForeground() const
{
    return Platform::shellInForeground(m_shell->pid());
}

// The shell's current folder: from the OS where it can tell (Unix), else the last one the shell integration reported
// (OSC 633;P;Cwd=, PowerShell on Windows). Empty when neither knows.
QString Terminal::shellDirectory() const
{
    const QString cwd = Platform::processCwd(m_shell->pid());
    return cwd.isEmpty() ? m_shellCwd : cwd;
}

void Terminal::clear()
{
    newCommand();
    m_view->screen()->clearScrollback();
    m_view->clearSelection();
    if (!m_shell->isRunning())
        m_view->screen()->reset();
    else if (ShellSyntax::dialectOf(m_shell->shell()) == ShellSyntax::Dialect::Cmd && shellInForeground())
        m_shell->write(QByteArray("\x1b" "cls\r")); // cmd has no Ctrl+L: Esc drops a half-typed line, then cls
    else
        m_shell->write(QByteArray("\x0c")); // Ctrl+L: shell clears the screen and redraws its prompt
}

void Terminal::focusTerminal()
{
    m_view->setFocus();
}

QString Terminal::recentOutput(int lines) const
{
    const TerminalScreen *screen = m_view->screen();
    int last = screen->totalLines() - 1;
    while (last >= 0 && screen->textInRange(last, 0, last, screen->cols() - 1).trimmed().isEmpty())
        --last;
    if (last < 0)
        return {};
    const int first = qMax(0, last - qMax(1, lines) + 1);
    return screen->textInRange(first, 0, last, screen->cols() - 1);
}

QString Terminal::currentDirectory() const
{
    const QString cwd = shellDirectory();
    return cwd.isEmpty() ? m_cwd : cwd;
}

// Marks from the shell integration, sent while the screen's cursor is where the shell wrote them: "C" = a command's
// output starts here, "E;<line>" = its command line, "D;<code>" = it ended (also sent at a prompt after an empty line,
// or by prompts with their own integration: only a D after our C counts), "P;Cwd=<folder>" = the shell's folder (sent
// at each prompt where the OS cannot tell it: PowerShell on Windows).
void Terminal::onShellMark(const QString &mark)
{
    if (mark == QLatin1String("C")) {
        setFailure({});
        m_inCommand = true;
        const TerminalScreen *screen = m_view->screen();
        m_commandStart = screen->cursorStableLine();
        m_commandText.clear();
        m_commandProblems.clear();
        m_commandFallback = m_written;
        const int from = screen->lineIndex(m_promptLine), to = screen->lineIndex(m_commandStart) - 1;
        if (m_commandFallback.isEmpty() && m_promptLine >= 0 && from >= 0 && from <= to && to - from < 10)
            m_commandFallback = screen->textInRange(from, m_promptCol, to, screen->cols() - 1).simplified();
        m_written.clear();
        m_promptLine = -1;
    } else if (mark.startsWith(QLatin1String("P;Cwd="))) {
        m_shellCwd = QDir::fromNativeSeparators(mark.mid(6));
    } else if (mark.startsWith(QLatin1String("E;")) && m_inCommand) {
        m_commandText = mark.mid(2).trimmed();
    } else if (mark.startsWith(QLatin1String("D"))) {
        m_awaitPrompt = true; // a prompt follows (also after an empty line or Ctrl+C at the prompt)
        m_promptLine = -1;
        if (!m_inCommand)
            return;
        m_inCommand = false;
        if (m_commandText.isEmpty())
            m_commandText = m_commandFallback;
        bool ok = false;
        const int code = mark.section(QLatin1Char(';'), 1, 1).toInt(&ok);
        commandEnded(ok ? code : 0);
    }
}

void Terminal::commandEnded(int exitCode)
{
    // Not failures worth fixing: Ctrl+C, a kill or Ctrl+Z (-1073741510 = STATUS_CONTROL_C_EXIT, Ctrl+C on Windows), and
    // commands whose non-zero code only answers a question.
    static const QList<int> stopped{130, 137, 143, 146, 148, -1073741510};
    static const QStringList answers{"grep", "egrep", "fgrep", "rg", "ag", "diff", "cmp", "test", "[", "[[", "false", "true", "which", "type", "command", "exit",
                                     "where", "where.exe", "findstr", "fc"};
    const QString first = m_commandText.section(QLatin1Char(' '), 0, 0, QString::SectionSkipEmpty);
    if (exitCode == 0 || stopped.contains(exitCode) || answers.contains(first))
        return;

    const TerminalScreen *screen = m_view->screen();
    const int start = qMax(0, screen->lineIndex(m_commandStart));
    int end = screen->lineIndex(screen->cursorStableLine());
    QStringList lines;
    for (int i = start; i <= end; ++i)
        lines << screen->textInRange(i, 0, i, screen->cols() - 1);
    while (!lines.isEmpty() && lines.constLast().trimmed().isEmpty())
        lines.removeLast();
    while (!lines.isEmpty() && lines.constFirst().trimmed().isEmpty())
        lines.removeFirst();
    if (lines.isEmpty()) // nothing printed: nothing to go on
        return;
    constexpr int kHead = 40, kTail = 120; // the first error and the summary at the end
    if (lines.size() > kHead + kTail) {
        const int skipped = int(lines.size()) - kHead - kTail;
        lines = lines.mid(0, kHead) + QStringList{QStringLiteral("… (%1 lines left out) …").arg(skipped)} + lines.mid(lines.size() - kTail);
    }

    FailedCommand f;
    f.command = m_commandText;
    f.exitCode = exitCode;
    f.output = lines.join(QLatin1Char('\n'));
    f.folder = currentDirectory();
    f.problems = m_commandProblems;
    setFailure(f);
}

void Terminal::setFailure(const FailedCommand &failure)
{
    m_failure = failure;
    m_fixBtn->setVisible(failure.exitCode != 0);
    if (failure.exitCode != 0) {
        m_fixBtn->setToolTip(tr("“%1” failed (exit code %2). Start a QODE chat with its output and the files it names.")
                                 .arg(failure.command.isEmpty() ? tr("The command") : failure.command.left(80))
                                 .arg(failure.exitCode));
        m_fixBtn->adjustSize();
        placeFixButton();
        m_fixBtn->raise();
    }
}

void Terminal::placeFixButton()
{
    const int margin = 10;
    m_fixBtn->move(m_view->width() - m_fixBtn->width() - margin - 12, m_view->height() - m_fixBtn->height() - margin);
}

bool Terminal::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view && event->type() == QEvent::Resize && m_fixBtn->isVisible())
        placeFixButton();
    return QWidget::eventFilter(watched, event);
}
