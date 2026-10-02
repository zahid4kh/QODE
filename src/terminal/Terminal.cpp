#include "Terminal.h"

#include "ShellProcess.h"
#include "TerminalScreen.h"
#include "TerminalView.h"
#include "filesystem/FileManager.h"
#include "settings/Icons.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
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
    connect(m_view, &TerminalView::sizeChanged, m_shell, &ShellProcess::resize);
    connect(m_view, &TerminalView::focusGained, this, &Terminal::focused);
    connect(m_view, &TerminalView::returnPressedWhileInactive, this, &Terminal::restart);
    connect(m_shell, &ShellProcess::output, m_view->screen(), &TerminalScreen::feed);
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

QString Terminal::shellName() const
{
    return m_shell->isRunning() ? QFileInfo(m_shell->shell()).fileName() : QString();
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
    m_pendingTimer->stop();
    m_pendingCommand.clear();
    m_booting = false;
    m_expectedExit = false;
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
            m_pendingCommand = m_startupProvider(QFileInfo(m_shell->shell()).fileName());
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

void Terminal::runCommand(const QString &command)
{
    const bool wasRunning = m_shell->isRunning();
    ensureStarted();
    if (!m_shell->isRunning())
        return;
    m_view->scrollToBottom();
    if (m_booting) { // a fresh shell (or its startup command) is still coming up: run after it
        if (!m_pendingCommand.isEmpty())
            m_pendingCommand += QLatin1Char('\r');
        m_pendingCommand += command;
        return;
    }
    Q_UNUSED(wasRunning)
    m_pendingCommand = command;
    m_shell->write(QByteArray("\x03")); // Ctrl+C: stop a previous run / discard a half-typed line
    m_pendingTimer->start(150);
}

void Terminal::flushPendingCommand()
{
    m_booting = false;
    if (m_pendingCommand.isEmpty() || !m_shell->isRunning())
        return;
    m_shell->write(m_pendingCommand.toUtf8() + '\r'); // '\r' inside it separates queued commands
    m_pendingCommand.clear();
}

void Terminal::clear()
{
    m_view->screen()->clearScrollback();
    m_view->clearSelection();
    if (m_shell->isRunning())
        m_shell->write(QByteArray("\x0c")); // Ctrl+L: shell clears the screen and redraws its prompt
    else
        m_view->screen()->reset();
}

void Terminal::focusTerminal()
{
    m_view->setFocus();
}
