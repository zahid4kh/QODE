#include "TerminalPanel.h"

#include "Terminal.h"
#include "settings/Icons.h"

#include <QAction>
#include <QDir>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
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
    m_tabs->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tabs->installEventFilter(this); // double click renames
    m_tabs->setToolTip(QString());

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

    auto *splitBtn = makeBtn(tr("Split"), tr("Split the current terminal"));
    splitBtn->setPopupMode(QToolButton::InstantPopup);
    auto *splitMenu = new QMenu(splitBtn);
    connect(splitMenu->addAction(tr("Split Right")), &QAction::triggered, this, [this] { splitCurrent(Qt::Horizontal); });
    connect(splitMenu->addAction(tr("Split Down")), &QAction::triggered, this, [this] { splitCurrent(Qt::Vertical); });
    splitBtn->setMenu(splitMenu);

    auto *clearBtn = makeBtn(tr("Clear"), tr("Clear terminal (scrollback and screen)"));
    auto *restartBtn = makeBtn(tr("Restart"), tr("Terminate the shell and start a new one"));
    m_maxBtn = new QToolButton(this);
    Icons::bind(m_maxBtn, QStringLiteral(":/new-icons/arrow-up.svg"));
    m_maxBtn->setCheckable(true);
    m_maxBtn->setAutoRaise(true);
    m_maxBtn->setToolTip(tr("Maximize the terminal panel / restore its size"));
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
    hl->addWidget(splitBtn);
    hl->addWidget(clearBtn);
    hl->addWidget(restartBtn);
    hl->addWidget(m_maxBtn);
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
    connect(restartBtn, &QToolButton::clicked, this, &TerminalPanel::restart);
    connect(closeBtn, &QToolButton::clicked, this, &TerminalPanel::hideRequested);
    connect(m_maxBtn, &QToolButton::toggled, this, &TerminalPanel::maximizeToggled);
    connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
        if (i < 0 || i >= m_stack->count())
            return;
        m_stack->setCurrentIndex(i);
        if (Terminal *t = current()) {
            t->ensureStarted(); // restored tabs start when first shown
            t->focusTerminal();
        }
        refreshTabs();
    });
    connect(m_tabs, &QTabBar::tabCloseRequested, this, &TerminalPanel::closeSession);
    connect(m_tabs, &QTabBar::tabMoved, this, [this](int from, int to) {
        QWidget *w = m_stack->widget(from);
        m_stack->removeWidget(w);
        m_stack->insertWidget(to, w);
        m_stack->setCurrentIndex(m_tabs->currentIndex());
        notifyTabs();
    });
    connect(m_tabs, &QWidget::customContextMenuRequested, this, &TerminalPanel::tabMenu);
}

bool TerminalPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tabs && event->type() == QEvent::MouseButtonDblClick) {
        const int i = m_tabs->tabAt(static_cast<QMouseEvent *>(event)->pos());
        if (i >= 0) {
            QTimer::singleShot(0, this, [this, i] { renameTab(i); });
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QSplitter *TerminalPanel::pageAt(int index) const
{
    return qobject_cast<QSplitter *>(m_stack->widget(index));
}

// The tab page (top-level splitter) a pane or nested splitter lives in.
QSplitter *TerminalPanel::pageOf(QWidget *w) const
{
    while (w && w->parentWidget() != m_stack)
        w = w->parentWidget();
    return qobject_cast<QSplitter *>(w);
}

QList<Terminal *> TerminalPanel::panesOf(QSplitter *page) const
{
    return page ? page->findChildren<Terminal *>() : QList<Terminal *>();
}

Terminal *TerminalPanel::current() const
{
    QSplitter *page = pageAt(m_stack->currentIndex());
    if (!page)
        return nullptr;
    Terminal *t = m_active.value(page);
    return t ? t : (panesOf(page).isEmpty() ? nullptr : panesOf(page).first());
}

int TerminalPanel::sessionCount() const
{
    return m_stack->count();
}

Terminal *TerminalPanel::createPane()
{
    auto *t = new Terminal(this);
    t->setHeaderVisible(false);
    t->setWorkingDirectory(m_cwd);
    if (m_startupProvider)
        t->setStartupCommandProvider(m_startupProvider);
    connect(t, &Terminal::focused, this, [this, t] {
        if (QSplitter *page = pageOf(t))
            m_active.insert(page, t);
    });
    // `exit` in one pane of a split closes that pane; a lone pane keeps its "press Enter to restart" message.
    connect(t, &Terminal::shellExited, this, [this, t] {
        if (panesOf(pageOf(t)).size() > 1)
            QTimer::singleShot(0, this, [this, t] {
                if (t)
                    closePane(t);
            });
    });
    return t;
}

int TerminalPanel::createSession(const QString &customName)
{
    auto *page = new QSplitter(Qt::Horizontal, m_stack);
    page->setChildrenCollapsible(false);
    page->setHandleWidth(1);
    Terminal *t = createPane();
    page->addWidget(t);
    m_active.insert(page, t);
    m_stack->addWidget(page);
    m_tabs->blockSignals(true);
    const int i = m_tabs->addTab(QString());
    m_tabs->setTabData(i, customName);
    m_tabs->setCurrentIndex(i);
    m_tabs->blockSignals(false);
    m_stack->setCurrentWidget(page);
    return i;
}

void TerminalPanel::newSession()
{
    createSession(QString());
    if (Terminal *t = current()) {
        t->ensureStarted();
        t->focusTerminal();
    }
    refreshTabs();
    notifyTabs();
}

void TerminalPanel::splitCurrent(Qt::Orientation orientation)
{
    Terminal *t = current();
    auto *parent = t ? qobject_cast<QSplitter *>(t->parentWidget()) : nullptr;
    if (!parent)
        return;
    Terminal *fresh = createPane();
    if (parent->orientation() == orientation || parent->count() == 1) {
        parent->setOrientation(orientation);
        parent->insertWidget(parent->indexOf(t) + 1, fresh);
        parent->setSizes(QList<int>(parent->count(), 1));
    } else {
        // Split across the other direction: wrap this pane and the new one in a nested splitter.
        const int idx = parent->indexOf(t);
        const QList<int> sizes = parent->sizes();
        auto *nested = new QSplitter(orientation);
        nested->setChildrenCollapsible(false);
        nested->setHandleWidth(1);
        parent->insertWidget(idx, nested);
        nested->addWidget(t);
        nested->addWidget(fresh);
        nested->setSizes({1, 1});
        parent->setSizes(sizes);
    }
    fresh->show();
    fresh->ensureStarted();
    fresh->focusTerminal();
    refreshTabs();
}

void TerminalPanel::closePane(Terminal *t)
{
    auto *parent = qobject_cast<QSplitter *>(t->parentWidget());
    QSplitter *page = pageOf(t);
    if (!parent || !page || panesOf(page).size() < 2)
        return;
    t->stop();
    t->hide();
    t->setParent(nullptr);
    t->deleteLater();
    // A nested splitter left with one child is replaced by that child.
    if (parent != page && parent->count() == 1) {
        auto *grand = qobject_cast<QSplitter *>(parent->parentWidget());
        QWidget *only = parent->widget(0);
        if (grand) {
            const QList<int> sizes = grand->sizes();
            const int idx = grand->indexOf(parent);
            grand->insertWidget(idx, only);
            parent->hide();
            parent->setParent(nullptr);
            parent->deleteLater();
            grand->setSizes(sizes);
        }
    }
    m_active.remove(page);
    if (Terminal *cur = current())
        cur->focusTerminal();
}

void TerminalPanel::closeSession(int index)
{
    QSplitter *page = pageAt(index);
    if (!page)
        return;
    for (Terminal *t : panesOf(page))
        t->stop();
    m_active.remove(page);
    m_stack->removeWidget(page);
    page->deleteLater();
    m_tabs->blockSignals(true);
    m_tabs->removeTab(index);
    m_tabs->blockSignals(false);
    notifyTabs();
    if (m_stack->count() == 0) {
        emit hideRequested(); // the next time the panel opens it gets a fresh session
        return;
    }
    m_stack->setCurrentIndex(m_tabs->currentIndex());
    if (Terminal *cur = current()) {
        cur->ensureStarted();
        cur->focusTerminal();
    }
    refreshTabs();
}

void TerminalPanel::renameTab(int index)
{
    if (index < 0 || index >= m_tabs->count())
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Terminal"), tr("Tab name (leave empty for the automatic name):"),
                                               QLineEdit::Normal, m_tabs->tabData(index).toString(), &ok)
                             .trimmed();
    if (!ok)
        return;
    m_tabs->setTabData(index, name);
    refreshTabs();
    notifyTabs();
}

void TerminalPanel::tabMenu(const QPoint &pos)
{
    const int i = m_tabs->tabAt(pos);
    if (i < 0)
        return;
    QMenu menu(this);
    connect(menu.addAction(tr("Rename…")), &QAction::triggered, this, [this, i] { renameTab(i); });
    menu.addSeparator();
    connect(menu.addAction(tr("Split Right")), &QAction::triggered, this, [this, i] {
        m_tabs->setCurrentIndex(i);
        splitCurrent(Qt::Horizontal);
    });
    connect(menu.addAction(tr("Split Down")), &QAction::triggered, this, [this, i] {
        m_tabs->setCurrentIndex(i);
        splitCurrent(Qt::Vertical);
    });
    menu.addSeparator();
    connect(menu.addAction(tr("Close")), &QAction::triggered, this, [this, i] { closeSession(i); });
    menu.exec(m_tabs->mapToGlobal(pos));
}

// A tab shows the name the user gave it, otherwise the shell of its pane ("bash (2)" for several of a kind).
void TerminalPanel::refreshTabs()
{
    QHash<QString, int> seen;
    for (int i = 0; i < m_stack->count(); ++i) {
        const QString custom = m_tabs->tabData(i).toString();
        Terminal *first = panesOf(pageAt(i)).value(0);
        QString name = custom;
        if (name.isEmpty()) {
            name = first ? first->shellName() : QString();
            if (name.isEmpty())
                name = tr("terminal");
            const int n = ++seen[name];
            if (n > 1)
                name = QStringLiteral("%1 (%2)").arg(name).arg(n);
        }
        m_tabs->setTabText(i, name);
        m_tabs->setTabToolTip(i, tr("%1 — double-click to rename").arg(first ? first->workingDirectory() : QString()));
    }
}

QStringList TerminalPanel::tabNames() const
{
    QStringList out;
    for (int i = 0; i < m_tabs->count(); ++i)
        out << m_tabs->tabData(i).toString();
    return out;
}

void TerminalPanel::notifyTabs()
{
    if (!m_restoring)
        emit tabsChanged(tabNames());
}

void TerminalPanel::setWorkingDirectory(const QString &dir)
{
    m_cwd = dir.isEmpty() ? QDir::homePath() : dir;
    for (int i = 0; i < m_stack->count(); ++i)
        for (Terminal *t : panesOf(pageAt(i)))
            t->setWorkingDirectory(m_cwd);
}

void TerminalPanel::setStartupCommandProvider(std::function<QString(const QString &)> provider)
{
    m_startupProvider = std::move(provider);
    for (int i = 0; i < m_stack->count(); ++i)
        for (Terminal *t : panesOf(pageAt(i)))
            t->setStartupCommandProvider(m_startupProvider);
}

void TerminalPanel::ensureStarted()
{
    if (m_stack->count() == 0) {
        m_restoring = true;
        const QStringList names = m_saved.isEmpty() ? QStringList{QString()} : m_saved;
        for (const QString &n : names)
            createSession(n);
        m_tabs->blockSignals(true);
        m_tabs->setCurrentIndex(0);
        m_tabs->blockSignals(false);
        m_stack->setCurrentIndex(0);
        m_restoring = false;
    }
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
        for (Terminal *t : panesOf(pageAt(i)))
            if (t->isRunning())
                t->restart();
}

void TerminalPanel::stop()
{
    for (int i = 0; i < m_stack->count(); ++i)
        for (Terminal *t : panesOf(pageAt(i)))
            t->stop();
}

bool TerminalPanel::isRunning() const
{
    for (int i = 0; i < m_stack->count(); ++i)
        for (Terminal *t : panesOf(pageAt(i)))
            if (t->isRunning())
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
