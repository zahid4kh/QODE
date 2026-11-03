#include "ClaudeCodeTasks.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

namespace claudecode {

namespace {
constexpr int kVisibleRows = 7;

QString field(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

// The state of one step: an empty ring (pending), a half-filled ring (in progress) or a filled disc with a tick (done).
class StatusDot : public QWidget
{
public:
    StatusDot(const QString &status, QWidget *parent)
        : QWidget(parent)
        , m_status(status)
    {
        setFixedSize(12, 12);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal w = qMax<qreal>(1.2, width() / 9.0);
        const QRectF r = QRectF(rect()).adjusted(w / 2 + 0.5, w / 2 + 0.5, -w / 2 - 0.5, -w / 2 - 0.5);
        if (m_status == QLatin1String("completed")) {
            p.setPen(Qt::NoPen);
            p.setBrush(t.success);
            p.drawEllipse(r.adjusted(-w / 2, -w / 2, w / 2, w / 2));
            QPainterPath tick;
            tick.moveTo(r.left() + r.width() * 0.25, r.center().y() + r.height() * 0.02);
            tick.lineTo(r.left() + r.width() * 0.43, r.top() + r.height() * 0.70);
            tick.lineTo(r.left() + r.width() * 0.76, r.top() + r.height() * 0.32);
            p.setPen(QPen(t.window, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(tick);
        } else if (m_status == QLatin1String("in_progress")) {
            p.setPen(QPen(t.accent, w));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(r);
            p.setPen(Qt::NoPen);
            p.setBrush(t.accent);
            p.drawPie(r.adjusted(w, w, -w, -w), 90 * 16, 180 * 16); // the left half
        } else {
            p.setPen(QPen(t.textMuted, w));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(r);
        }
    }

private:
    QString m_status;
};
} // namespace

TaskList::TaskList(QWidget *parent)
    : QFrame(parent)
    , m_header(new QPushButton(this))
    , m_body(new QWidget(this))
    , m_rows(new QVBoxLayout(m_body))
{
    setObjectName(QStringLiteral("ccTasks"));
    m_header->setObjectName(QStringLiteral("ccTasksHeader"));
    m_header->setCursor(Qt::PointingHandCursor);
    m_header->setIconSize(QSize(12, 12));
    connect(m_header, &QPushButton::clicked, this, [this] {
        m_collapsed = !m_collapsed;
        m_body->setVisible(!m_collapsed);
        updateHeader();
    });
    m_rows->setContentsMargins(4, 0, 4, 2);
    m_rows->setSpacing(3);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(6, 4, 8, 6);
    lay->setSpacing(4);
    lay->addWidget(m_header);
    lay->addWidget(m_body);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] {
        updateHeader();
        update();
    });
    hide();
}

void TaskList::setTodos(const QJsonArray &todos)
{
    m_todos = todos;
    if (todos.isEmpty()) {
        clear();
        return;
    }
    rebuild();
    m_body->setVisible(!m_collapsed);
    show();
}

void TaskList::clear()
{
    m_todos = QJsonArray();
    hide();
}

void TaskList::updateHeader()
{
    int done = 0;
    QString now;
    for (const QJsonValue &v : std::as_const(m_todos)) {
        const QJsonObject o = v.toObject();
        if (field(o, "status") == QLatin1String("completed"))
            ++done;
        else if (field(o, "status") == QLatin1String("in_progress") && now.isEmpty())
            now = field(o, "activeForm").isEmpty() ? field(o, "content") : field(o, "activeForm");
    }
    QString text = tr("Tasks  ·  %1/%2 done").arg(done).arg(m_todos.size());
    if (m_collapsed && !now.isEmpty())
        text += QStringLiteral("  ·  ") + now.simplified();
    m_header->setText(text);
    m_header->setToolTip(m_collapsed ? tr("Show the steps") : tr("Fold the list"));
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_header->setIcon(Icons::tinted(m_collapsed ? QStringLiteral(":/new-icons/chevron-right.svg") : QStringLiteral(":/new-icons/chevron-down.svg"), t.textMuted));
}

void TaskList::rebuild()
{
    while (QLayoutItem *item = m_rows->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    const int n = int(m_todos.size());
    // The step in progress (else the first one still open) stays in view, with a little of what came before it.
    int current = -1;
    for (int i = 0; i < n && current < 0; ++i)
        if (field(m_todos.at(i).toObject(), "status") == QLatin1String("in_progress"))
            current = i;
    for (int i = 0; i < n && current < 0; ++i)
        if (field(m_todos.at(i).toObject(), "status") != QLatin1String("completed"))
            current = i;
    if (current < 0)
        current = n - 1;
    const int first = n <= kVisibleRows ? 0 : qBound(0, current - 2, n - kVisibleRows);
    const int last = qMin(n, first + kVisibleRows);
    auto more = [this](const QString &text) {
        auto *l = new QLabel(text, m_body);
        l->setObjectName(QStringLiteral("ccTaskMore"));
        l->setIndent(20);
        m_rows->addWidget(l);
    };
    if (first > 0)
        more(first == 1 ? tr("… 1 earlier step") : tr("… %1 earlier steps").arg(first));
    for (int i = first; i < last; ++i) {
        const QJsonObject o = m_todos.at(i).toObject();
        const QString status = field(o, "status");
        auto *row = new QWidget(m_body);
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(8);
        auto *text = new QLabel(field(o, "content"), row);
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        text->setObjectName(status == QLatin1String("completed")     ? QStringLiteral("ccTaskDone")
                            : status == QLatin1String("in_progress") ? QStringLiteral("ccTaskNow")
                                                                     : QStringLiteral("ccTask"));
        l->addWidget(new StatusDot(status, row), 0, Qt::AlignVCenter);
        l->addWidget(text, 1);
        m_rows->addWidget(row);
    }
    if (last < n)
        more(tr("… %1 more").arg(n - last));
    updateHeader();
}

} // namespace claudecode
