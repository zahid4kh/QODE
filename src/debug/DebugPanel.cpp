#include "debug/DebugPanel.h"

#include "debug/DebugSession.h"
#include "debug/JdwpWire.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kObjectRole = Qt::UserRole + 1;
constexpr int kLoadedRole = Qt::UserRole + 2;
constexpr int kPathRole = Qt::UserRole + 3;
constexpr int kLineRole = Qt::UserRole + 4;

QLabel *sectionTitle(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("emptyText"));
    l->setContentsMargins(10, 6, 10, 2);
    QFont f = l->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() - 1);
    l->setFont(f);
    return l;
}
} // namespace

DebugPanel::DebugPanel(DebugSession *session, QWidget *parent)
    : QWidget(parent)
    , m_session(session)
{
    auto *title = new QLabel(tr("DEBUG"), this);
    title->setObjectName(QStringLiteral("panelTitle"));
    auto *closeBtn = new QToolButton(this);
    Icons::bind(closeBtn, QStringLiteral(":/new-icons/x.svg"));
    closeBtn->setAutoRaise(true);
    closeBtn->setToolTip(tr("Close the debug panel (the session goes on)"));
    connect(closeBtn, &QToolButton::clicked, this, &DebugPanel::closeRequested);
    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->addWidget(title, 1);
    hl->addWidget(closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#mediaHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#mediaHeader QLabel { background: transparent; border: none; }"));

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setContentsMargins(10, 6, 10, 2);
    m_buttons = new QWidget(this);
    auto *bl = new QHBoxLayout(m_buttons);
    bl->setContentsMargins(6, 0, 6, 0);
    bl->setSpacing(2);
    m_exception = new QLabel(this);
    m_exception->setWordWrap(true);
    m_exception->setContentsMargins(10, 4, 10, 4);
    m_exception->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_exception->hide();

    // Frames
    auto *framesBox = new QWidget(this);
    auto *fl = new QVBoxLayout(framesBox);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setSpacing(0);
    m_threads = new QComboBox(framesBox);
    m_threads->setToolTip(tr("Thread"));
    connect(m_threads, &QComboBox::currentIndexChanged, this, [this](int i) {
        if (m_fillingThreads || i < 0)
            return;
        const quint64 t = m_threads->itemData(i).toULongLong();
        if (t && t != m_thread)
            m_session->selectThread(t);
    });
    m_frames = new QListWidget(framesBox);
    m_frames->setUniformItemSizes(true);
    connect(m_frames, &QListWidget::currentRowChanged, this, &DebugPanel::selectFrame);
    auto *threadRow = new QHBoxLayout;
    threadRow->setContentsMargins(8, 0, 8, 4);
    threadRow->addWidget(m_threads, 1);
    fl->addWidget(sectionTitle(tr("FRAMES"), framesBox));
    fl->addLayout(threadRow);
    fl->addWidget(m_frames, 1);

    // Variables
    auto *varsBox = new QWidget(this);
    auto *vl = new QVBoxLayout(varsBox);
    vl->setContentsMargins(0, 0, 0, 0);
    vl->setSpacing(0);
    m_eval = new QLineEdit(varsBox);
    m_eval->setPlaceholderText(tr("Evaluate: name, a.b, list[0], user.getName()"));
    m_eval->setClearButtonEnabled(true);
    connect(m_eval, &QLineEdit::returnPressed, this, &DebugPanel::evaluate);
    m_vars = new QTreeWidget(varsBox);
    m_vars->setHeaderLabels({tr("Name"), tr("Value")});
    m_vars->setUniformRowHeights(true);
    m_vars->setIndentation(14);
    m_vars->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_vars->header()->resizeSection(0, 130);
    m_vars->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_vars, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *it) {
        if (it->data(0, kLoadedRole).toBool())
            return;
        it->setData(0, kLoadedRole, true);
        m_pending.insert(m_session->expand(it->data(0, kObjectRole).toULongLong()), it);
    });
    connect(m_vars, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *it = m_vars->itemAt(pos);
        if (!it)
            return;
        QMenu menu(this);
        menu.addAction(tr("Copy Value"), this, [it] { QApplication::clipboard()->setText(it->text(1)); });
        menu.addAction(tr("Copy Name"), this, [it] { QApplication::clipboard()->setText(it->text(0)); });
        menu.exec(m_vars->viewport()->mapToGlobal(pos));
    });
    auto *evalRow = new QHBoxLayout;
    evalRow->setContentsMargins(8, 0, 8, 4);
    evalRow->addWidget(m_eval);
    vl->addWidget(sectionTitle(tr("VARIABLES"), varsBox));
    vl->addLayout(evalRow);
    vl->addWidget(m_vars, 1);

    // Breakpoints
    auto *bpBox = new QWidget(this);
    auto *bpl = new QVBoxLayout(bpBox);
    bpl->setContentsMargins(0, 0, 0, 0);
    bpl->setSpacing(0);
    m_breakpoints = new QListWidget(bpBox);
    m_breakpoints->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_breakpoints, &QListWidget::itemActivated, this, [this](QListWidgetItem *it) {
        emit locationRequested(it->data(kPathRole).toString(), it->data(kLineRole).toInt());
    });
    connect(m_breakpoints, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QListWidgetItem *it = m_breakpoints->itemAt(pos);
        QMenu menu(this);
        if (it) {
            menu.addAction(tr("Go to Line"), this, [this, it] { emit locationRequested(it->data(kPathRole).toString(), it->data(kLineRole).toInt()); });
            menu.addAction(tr("Remove Breakpoint"), this, [this, it] {
                emit removeBreakpointRequested(it->data(kPathRole).toString(), it->data(kLineRole).toInt());
            });
        }
        menu.addAction(tr("Remove All Breakpoints"), this, &DebugPanel::clearBreakpointsRequested);
        menu.exec(m_breakpoints->viewport()->mapToGlobal(pos));
    });
    m_uncaught = new QCheckBox(tr("Stop on uncaught exceptions"), bpBox);
    m_uncaught->setChecked(true);
    connect(m_uncaught, &QCheckBox::toggled, this, &DebugPanel::breakOnUncaughtChanged);
    auto *uncaughtRow = new QHBoxLayout;
    uncaughtRow->setContentsMargins(10, 2, 8, 6);
    uncaughtRow->addWidget(m_uncaught);
    bpl->addWidget(sectionTitle(tr("BREAKPOINTS"), bpBox));
    bpl->addWidget(m_breakpoints, 1);
    bpl->addLayout(uncaughtRow);

    auto *split = new QSplitter(Qt::Vertical, this);
    split->addWidget(framesBox);
    split->addWidget(varsBox);
    split->addWidget(bpBox);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 4);
    split->setStretchFactor(2, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_status);
    layout->addWidget(m_buttons);
    layout->addWidget(m_exception);
    layout->addWidget(split, 1);

    connect(session, &DebugSession::stateChanged, this, &DebugPanel::onStateChanged);
    connect(session, &DebugSession::stopped, this, &DebugPanel::onStopped);
    connect(session, &DebugSession::variables, this, &DebugPanel::onVariables);
    connect(session, &DebugSession::evaluated, this, [this](int token, const DebugVariable &v, const QString &error) {
        if (token != m_evalToken)
            return;
        const Theme t = Theme::byName(SettingsManager::instance().theme());
        auto *it = new QTreeWidgetItem;
        if (error.isEmpty()) {
            fillItem(it, v);
        } else {
            it->setText(0, m_eval->text().trimmed());
            it->setText(1, error);
            it->setForeground(1, t.danger);
        }
        it->setIcon(0, Icons::tinted(QStringLiteral(":/new-icons/search.svg"), t.accent));
        m_vars->insertTopLevelItem(0, it);
        m_vars->setCurrentItem(it);
    });
    connect(session, &DebugSession::finished, this, [this](const QString &reason) {
        m_status->setText(reason);
        m_frames->clear();
        m_vars->clear();
        m_threads->clear();
        m_exception->hide();
        emit frameShown(QString(), 0);
    });
    onStateChanged();
}

void DebugPanel::setActions(const QList<QAction *> &actions)
{
    auto *bl = static_cast<QHBoxLayout *>(m_buttons->layout());
    for (QAction *a : actions) {
        auto *b = new QToolButton(m_buttons);
        b->setDefaultAction(a);
        b->setAutoRaise(true);
        b->setToolButtonStyle(Qt::ToolButtonIconOnly);
        b->setIconSize(QSize(16, 16));
        bl->addWidget(b);
    }
    bl->addStretch(1);
}

void DebugPanel::setTarget(const QString &text)
{
    m_target = text;
    onStateChanged();
}

bool DebugPanel::breakOnUncaught() const
{
    return m_uncaught->isChecked();
}

void DebugPanel::setBreakpoints(const QHash<QString, QList<int>> &all)
{
    // The editor's breakpoint mark: a red disc.
    QPixmap dot(QSize(14, 14) * 2);
    dot.setDevicePixelRatio(2);
    dot.fill(Qt::transparent);
    {
        QPainter p(&dot);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::byName(SettingsManager::instance().theme()).danger);
        p.drawEllipse(QRectF(2, 2, 10, 10));
    }
    const QIcon dotIcon(dot);
    m_breakpoints->clear();
    QStringList paths = all.keys();
    std::sort(paths.begin(), paths.end());
    for (const QString &p : std::as_const(paths))
        for (int line : all.value(p)) {
            auto *it = new QListWidgetItem(QStringLiteral("%1:%2").arg(QFileInfo(p).fileName()).arg(line), m_breakpoints);
            it->setToolTip(p);
            it->setData(kPathRole, p);
            it->setData(kLineRole, line);
            it->setIcon(dotIcon);
        }
    if (m_breakpoints->count() == 0) {
        auto *hint = new QListWidgetItem(tr("Click a line number in a .kt / .java / .kts file to add one (Ctrl+F8)."), m_breakpoints);
        hint->setFlags(Qt::NoItemFlags);
    }
}

void DebugPanel::onStateChanged()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    QString text;
    QColor color = t.textMuted;
    switch (m_session->state()) {
    case DebugSession::State::Idle:
        if (m_status->text().isEmpty())
            text = tr("Not debugging. Start with Debug (Shift+F9) or the ▶ next to a main function.");
        else
            return; // keep the reason the last session ended with
        break;
    case DebugSession::State::Connecting:
        text = tr("Waiting for %1 to start (it builds first)…").arg(m_target.isEmpty() ? tr("the program") : m_target);
        break;
    case DebugSession::State::Running:
        text = tr("● Running %1").arg(m_target);
        color = t.success;
        m_exception->hide();
        break;
    case DebugSession::State::Suspended:
        text = tr("❚❚ Paused in %1").arg(m_target);
        color = t.warning;
        break;
    }
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
    m_status->setText(text);
}

void DebugPanel::onStopped(const DebugStop &stop)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_thread = stop.thread;
    m_frameData = stop.frames;
    m_fillingThreads = true;
    m_threads->clear();
    for (const auto &th : stop.threads) {
        m_threads->addItem(th.second, QVariant::fromValue<qulonglong>(th.first));
        if (th.first == stop.thread)
            m_threads->setCurrentIndex(m_threads->count() - 1);
    }
    m_fillingThreads = false;
    if (stop.reason == QLatin1String("exception")) {
        m_exception->setStyleSheet(QStringLiteral("color: %1;").arg(t.danger.name()));
        m_exception->setText(tr("Uncaught exception: %1").arg(stop.exception));
        m_exception->show();
    } else if (stop.reason != QLatin1String("thread")) {
        m_exception->hide();
    }
    m_frames->blockSignals(true);
    m_frames->clear();
    int first = -1;
    for (int i = 0; i < m_frameData.size(); ++i) {
        DebugFrame &f = m_frameData[i];
        if (m_resolve && !f.sourceName.isEmpty())
            f.path = m_resolve(f.pkgPath, f.sourceName);
        const QString cls = f.className.section(QLatin1Char('.'), -1);
        const QString where = f.sourceName.isEmpty() ? tr("no source") : QStringLiteral("%1:%2").arg(f.sourceName).arg(f.line);
        auto *it = new QListWidgetItem(QStringLiteral("%1.%2  —  %3").arg(cls, f.method, where), m_frames);
        it->setToolTip(f.className + QLatin1Char('.') + f.method + (f.path.isEmpty() ? QString() : QLatin1Char('\n') + f.path));
        if (f.path.isEmpty()) {
            it->setForeground(t.textMuted);
        } else {
            QFont bold = it->font();
            bold.setBold(true);
            it->setFont(bold);
            if (first < 0)
                first = i;
        }
    }
    m_frames->blockSignals(false);
    // The first frame in the project's own code (library frames above it are where it was paused, not what to show).
    const int pick = stop.reason == QLatin1String("pause") || stop.reason == QLatin1String("thread") ? qMax(0, first) : 0;
    if (m_frames->count() > 0)
        m_frames->setCurrentRow(pick);
    else
        m_vars->clear();
}

void DebugPanel::selectFrame(int index)
{
    if (index < 0 || index >= m_frameData.size())
        return;
    const DebugFrame &f = m_frameData.at(index);
    emit frameShown(f.path, f.line);
    m_vars->clear();
    m_pending.clear();
    auto *loading = new QTreeWidgetItem(m_vars, {tr("loading…")});
    loading->setFlags(Qt::NoItemFlags);
    m_frameToken = m_session->selectFrame(m_thread, index);
}

void DebugPanel::fillItem(QTreeWidgetItem *item, const DebugVariable &v)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    item->setText(0, v.name);
    item->setText(1, v.value);
    item->setToolTip(0, v.type.isEmpty() ? v.name : v.name + QStringLiteral(" : ") + v.type);
    item->setToolTip(1, v.value.size() > 60 ? v.value : (v.type.isEmpty() ? v.value : v.type));
    if (v.tag == 's' || v.value.startsWith(QLatin1Char('"')))
        item->setForeground(1, t.string);
    else if (v.tag && !Jdwp::isReferenceTag(v.tag))
        item->setForeground(1, t.number);
    else if (v.value == QLatin1String("null"))
        item->setForeground(1, t.keyword);
    if (v.objectId) {
        item->setData(0, kObjectRole, QVariant::fromValue<qulonglong>(v.objectId));
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
}

void DebugPanel::onVariables(int token, const QList<DebugVariable> &vars)
{
    QTreeWidgetItem *parent = nullptr;
    if (token == m_frameToken) {
        m_vars->clear();
    } else {
        parent = m_pending.take(token);
        if (!parent)
            return;
        qDeleteAll(parent->takeChildren());
        if (vars.isEmpty())
            parent->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicator);
    }
    for (const DebugVariable &v : vars) {
        auto *it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_vars);
        fillItem(it, v);
    }
    if (!parent && vars.isEmpty()) {
        auto *none = new QTreeWidgetItem(m_vars, {tr("no variables")});
        none->setFlags(Qt::NoItemFlags);
    }
}

void DebugPanel::evaluate()
{
    const QString expr = m_eval->text().trimmed();
    if (expr.isEmpty() || m_session->state() != DebugSession::State::Suspended)
        return;
    m_evalToken = m_session->evaluate(m_thread, qMax(0, m_frames->currentRow()), expr);
}
