#include "SideSections.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QSplitter>
#include <QToolButton>
#include <QVBoxLayout>

SideSections::SideSections(QWidget *parent)
    : QWidget(parent)
{
    m_split = new QSplitter(Qt::Vertical, this);
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(4);
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_split);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this] { refreshIcons(); });
}

int SideSections::addSection(const QString &title, QWidget *body)
{
    Section s;
    s.title = title;
    s.body = body;
    s.frame = new QWidget(m_split);
    s.header = new QToolButton(s.frame);
    s.header->setObjectName(QStringLiteral("sectionHeader"));
    s.header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    s.header->setCursor(Qt::PointingHandCursor);
    s.header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *lay = new QVBoxLayout(s.frame);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addWidget(s.header);
    lay->addWidget(body, 1);
    m_split->addWidget(s.frame);
    const int index = m_sections.size();
    m_sections.append(s);
    connect(s.header, &QToolButton::clicked, this, [this, index] { setExpanded(index, !m_sections[index].expanded); });
    refresh(index);
    return index;
}

void SideSections::setTitle(int index, const QString &title)
{
    m_sections[index].title = title;
    refresh(index);
}

bool SideSections::isExpanded(int index) const
{
    return m_sections.value(index).expanded;
}

void SideSections::setExpanded(int index, bool expanded)
{
    if (index < 0 || index >= m_sections.size() || m_sections[index].expanded == expanded)
        return;
    Section &s = m_sections[index];
    const int frameIdx = m_split->indexOf(s.frame);
    if (!expanded)
        s.height = m_split->sizes().value(frameIdx);
    s.expanded = expanded;
    refresh(index);
    if (expanded && s.height > 0) {
        // Take the height back from the other open sections.
        QList<int> sizes = m_split->sizes();
        const int want = s.height - sizes[frameIdx];
        sizes[frameIdx] = s.height;
        int take = want;
        for (int i = sizes.size() - 1; i >= 0 && take > 0; --i) {
            if (i == frameIdx || !m_sections[i].expanded)
                continue;
            const int give = qMin(take, qMax(0, sizes[i] - 80));
            sizes[i] -= give;
            take -= give;
        }
        m_split->setSizes(sizes);
    }
    emit expansionChanged();
}

QList<bool> SideSections::expandedStates() const
{
    QList<bool> out;
    for (const Section &s : m_sections)
        out.append(s.expanded);
    return out;
}

void SideSections::setExpandedStates(const QList<bool> &states)
{
    for (int i = 0; i < m_sections.size() && i < states.size(); ++i)
        setExpanded(i, states[i]);
}

void SideSections::refresh(int index)
{
    Section &s = m_sections[index];
    s.header->setText(s.title.toUpper());
    s.body->setVisible(s.expanded);
    // A collapsed section is exactly as tall as its header, so the splitter gives the space to open ones.
    const int h = s.header->sizeHint().height();
    s.frame->setMaximumHeight(s.expanded ? QWIDGETSIZE_MAX : h);
    // An open section never gets smaller than its body needs (e.g. the commit box must not be squashed).
    s.frame->setMinimumHeight(s.expanded ? h + qMax(60, s.body->minimumSizeHint().height()) : h);
    refreshIcons();
}

void SideSections::refreshIcons()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    for (const Section &s : m_sections)
        s.header->setIcon(Icons::tinted(s.expanded ? QStringLiteral(":/new-icons/chevron-down.svg")
                                                   : QStringLiteral(":/new-icons/chevron-right.svg"),
                                        t.textMuted));
}
