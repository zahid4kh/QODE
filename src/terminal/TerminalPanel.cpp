#include "TerminalPanel.h"

#include "Terminal.h"
#include "settings/Icons.h"

#include <QDir>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

TerminalPanel::TerminalPanel(QWidget *parent)
    : QWidget(parent)
{
    m_cwd = QDir::homePath();

    m_tabs = new QTabBar(this);
    m_tabs->setObjectName(QStringLiteral("terminalTabs"));
    m_tabs->setExpanding(false);
    m_tabs->setDrawBase(false);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->setUsesScrollButtons(true);
    m_tabs->setElideMode(Qt::ElideRight);

    auto makeBtn = [this](const QString &text, const QString &tip) {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        return b;
    };
    auto *addBtn = new QToolButton(this);
    Icons::bind(addBtn, QStringLiteral(":/new-icons/plus.svg"));
    addBtn->setToolTip(tr("New terminal (Ctrl+Shift+T)"));
    addBtn->setAutoRaise(true);
    auto *clearBtn = makeBtn(tr("Clear"), tr("Clear terminal (scrollback and screen)"));
    m_restartBtn = makeBtn(tr("Restart"), tr("Terminate the shell and start a new one"));
    auto *closeBtn = new QToolButton(this);
    Icons::bind(closeBtn, QStringLiteral(":/new-icons/x.svg"));
    closeBtn->setToolTip(tr("Hide terminal (Ctrl+J)"));
    closeBtn->setAutoRaise(true);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("terminalHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(4, 0, 4, 0);
    hl->setSpacing(2);
    hl->addWidget(m_tabs, 0);
    hl->addWidget(addBtn);
    hl->addStretch(1);
    hl->addWidget(clearBtn);
    hl->addWidget(m_restartBtn);
    hl->addWidget(closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#terminalHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); border-top: 1px solid palette(shadow); }"
                                         "QWidget#terminalHeader QLabel { background: transparent; }"));

    m_stack = new QStackedWidget(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_stack, 1);

    connect(addBtn, &QToolButton::clicked, this, &TerminalPanel::newSession);
    connect(clearBtn, &QToolButton::clicked, this, &TerminalPanel::clear);
    connect(m_restartBtn, &QToolButton::clicked, this, &TerminalPanel::restart);
    connect(closeBtn, &QToolButton::clicked, this, &TerminalPanel::hideRequested);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
        if (i >= 0 && i < m_stack->count())
            m_stack->setCurrentIndex(i);
        if (Terminal *t = current())
            t->focusTerminal();
    });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, &TerminalPanel::closeSession);
    connect(m_tabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        QWidget *w = m_stack->widget(from);
        m_stack->removeWidget(w);
        m_stack->insertWidget(to, w);
        m_stack->setCurrentIndex(m_tabs->currentIndex());
    });
}

Terminal *TerminalPanel::current() const
{
    return qobject_cast<Terminal *>(m_stack->currentWidget());
}

int TerminalPanel::sessionCount() const
{
    return m_stack->count();
}

Terminal *TerminalPanel::createSession()
{
    auto *t = new Terminal(this);
    t->setHeaderVisible(false);
    t->setWorkingDirectory(m_cwd);
    if (m_startupProvider)
        t->setStartupCommandProvider(m_startupProvider);
    m_stack->addWidget(t);
    m_tabs->blockSignals(true);
    const int i = m_tabs->addTab(tr("Terminal %1").arg(++m_counter));
    m_tabs->setCurrentIndex(i);
    m_tabs->blockSignals(false);
    m_stack->setCurrentWidget(t);
    return t;
}

void TerminalPanel::newSession()
{
    Terminal *t = createSession();
    t->ensureStarted();
    refreshTabs();
    t->focusTerminal();
}

void TerminalPanel::closeSession(int index)
{
    auto *t = qobject_cast<Terminal *>(m_stack->widget(index));
    if (!t)
        return;
    t->stop();
    m_stack->removeWidget(t);
    t->deleteLater();
    m_tabs->blockSignals(true);
    m_tabs->removeTab(index);
    m_tabs->blockSignals(false);
    if (m_stack->count() == 0) {
        m_counter = 0;
        emit hideRequested(); // the next time the panel opens it gets a fresh session
        return;
    }
    m_stack->setCurrentIndex(m_tabs->currentIndex());
    if (Terminal *cur = current())
        cur->focusTerminal();
}

// Tab titles name the shell of each session ("bash 2" when there are several of the same kind).
void TerminalPanel::refreshTabs()
{
    QHash<QString, int> seen;
    for (int i = 0; i < m_stack->count(); ++i) {
        auto *t = qobject_cast<Terminal *>(m_stack->widget(i));
        QString name = t ? t->shellName() : QString();
        if (name.isEmpty())
            name = tr("terminal");
        const int n = ++seen[name];
        m_tabs->setTabText(i, n > 1 ? QStringLiteral("%1 (%2)").arg(name).arg(n) : name);
        m_tabs->setTabToolTip(i, t ? t->workingDirectory() : QString());
    }
}

void TerminalPanel::setWorkingDirectory(const QString &dir)
{
    m_cwd = dir.isEmpty() ? QDir::homePath() : dir;
    for (int i = 0; i < m_stack->count(); ++i)
        if (auto *t = qobject_cast<Terminal *>(m_stack->widget(i)))
            t->setWorkingDirectory(m_cwd);
}

void TerminalPanel::setStartupCommandProvider(std::function<QString(const QString &)> provider)
{
    m_startupProvider = std::move(provider);
    for (int i = 0; i < m_stack->count(); ++i)
        if (auto *t = qobject_cast<Terminal *>(m_stack->widget(i)))
            t->setStartupCommandProvider(m_startupProvider);
}

void TerminalPanel::ensureStarted()
{
    if (m_stack->count() == 0)
        createSession();
    if (Terminal *t = current())
        t->ensureStarted();
    refreshTabs();
}

void TerminalPanel::runCommand(const QString &command)
{
    ensureStarted();
    if (Terminal *t = current())
        t->runCommand(command);
    refreshTabs();
}

void TerminalPanel::restart()
{
    if (Terminal *t = current())
        t->restart();
}

void TerminalPanel::restartAll()
{
    for (int i = 0; i < m_stack->count(); ++i)
        if (auto *t = qobject_cast<Terminal *>(m_stack->widget(i)); t && t->isRunning())
            t->restart();
}

void TerminalPanel::stop()
{
    for (int i = 0; i < m_stack->count(); ++i)
        if (auto *t = qobject_cast<Terminal *>(m_stack->widget(i)))
            t->stop();
}

bool TerminalPanel::isRunning() const
{
    for (int i = 0; i < m_stack->count(); ++i)
        if (auto *t = qobject_cast<Terminal *>(m_stack->widget(i)); t && t->isRunning())
            return true;
    return false;
}

void TerminalPanel::clear()
{
    if (Terminal *t = current())
        t->clear();
}

void TerminalPanel::focusTerminal()
{
    if (Terminal *t = current())
        t->focusTerminal();
}
