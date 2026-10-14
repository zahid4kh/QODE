#include "AgentTurnView.h"

#include "ai/AiChangeCard.h"
#include "media/MarkdownView.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace agent {

namespace {
QString escaped(const QString &s)
{
    return s.toHtmlEscaped();
}
}

AgentTurnView::AgentTurnView(const QString &model, QWidget *parent)
    : QWidget(parent)
    , m_layout(new QVBoxLayout(this))
    , m_flush(new QTimer(this))
{
    m_layout->setContentsMargins(2, 0, 2, 0);
    m_layout->setSpacing(6);
    auto *who = new QLabel(model.isEmpty() ? tr("AGENT") : tr("AGENT · %1").arg(model), this);
    who->setObjectName(QStringLiteral("aiWho"));
    m_layout->addWidget(who);

    m_flush->setSingleShot(true);
    m_flush->setInterval(70); // re-rendering the Markdown on every streamed fragment would be wasteful
    connect(m_flush, &QTimer::timeout, this, &AgentTurnView::flush);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AgentTurnView::applyTheme);
    applyTheme();
}

MarkdownView *AgentTurnView::makeView(QWidget *parent, bool muted)
{
    auto *view = new MarkdownView(parent);
    view->document()->setDocumentMargin(2);
    view->setStyleSheet(muted ? QStringLiteral("QTextBrowser { background: transparent; color: %1; }").arg(Theme::byName(SettingsManager::instance().theme()).textMuted.name())
                              : QStringLiteral("QTextBrowser { background: transparent; }"));
    view->setAutoHeight(true);
    connect(view, &QTextBrowser::anchorClicked, this, [this, view](const QUrl &url) {
        if (url.scheme() == QLatin1String("qode-copy"))
            view->handleCopyLink(url);
        else
            emit linkClicked(url);
    });
    return view;
}

AgentTurnView::Item &AgentTurnView::startItem(Block::Kind kind)
{
    // Whatever came before is finished: a thought folds as soon as something else follows it.
    if (!m_items.isEmpty() && m_items.last().kind == Block::Thought && kind != Block::Thought)
        foldThoughts();

    Item it;
    it.kind = kind;
    if (kind == Block::Text) {
        it.view = makeView(this, false);
        it.widget = it.view;
    } else if (kind == Block::Thought) {
        auto *box = new QWidget(this);
        auto *l = new QVBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
        it.header = new QToolButton(box);
        it.header->setObjectName(QStringLiteral("agentThoughtHeader"));
        it.header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        it.header->setAutoRaise(true);
        it.header->setCursor(Qt::PointingHandCursor);
        l->addWidget(it.header, 0, Qt::AlignLeft);
        it.view = makeView(box, true);
        it.view->setVisible(false);
        l->addWidget(it.view);
        it.widget = box;
        const int index = m_items.size();
        connect(it.header, &QToolButton::clicked, this, [this, index] { setThoughtOpen(index, !m_items.at(index).open); });
    } else {
        auto *row = new QWidget(this);
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(4, 1, 4, 1);
        l->setSpacing(6);
        it.icon = new QLabel(row);
        it.icon->setFixedSize(14, 14);
        it.label = new QLabel(row);
        it.label->setObjectName(QStringLiteral("agentToolText"));
        it.label->setTextFormat(Qt::RichText);
        it.label->setWordWrap(true);
        it.label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->addWidget(it.icon, 0, Qt::AlignTop);
        l->addWidget(it.label, 1);
        it.widget = row;
    }
    m_layout->addWidget(it.widget);
    m_items.append(it);
    return m_items.last();
}

void AgentTurnView::setThoughtOpen(int index, bool open)
{
    Item &it = m_items[index];
    it.open = open;
    if (it.view)
        it.view->setVisible(open);
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    it.header->setIcon(Icons::tinted(open ? QStringLiteral(":/new-icons/chevron-down.svg") : QStringLiteral(":/new-icons/chevron-right.svg"), t.textMuted));
    it.header->setIconSize(QSize(12, 12));
    it.header->setText(m_live && index == m_items.size() - 1 ? tr(" Thinking…") : tr(" Thought"));
    if (open && it.dirty)
        flush();
}

void AgentTurnView::foldThoughts()
{
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).kind == Block::Thought && m_items.at(i).open) {
            m_items[i].open = false;
            m_items[i].view->setVisible(false);
            const Theme t = Theme::byName(SettingsManager::instance().theme());
            m_items[i].header->setIcon(Icons::tinted(QStringLiteral(":/new-icons/chevron-right.svg"), t.textMuted));
            m_items[i].header->setText(tr(" Thought"));
        }
}

void AgentTurnView::appendThought(const QString &text)
{
    m_live = true;
    if (m_items.isEmpty() || m_items.last().kind != Block::Thought) {
        startItem(Block::Thought);
        setThoughtOpen(m_items.size() - 1, true); // watch it think
    }
    m_items.last().text += text;
    m_items.last().dirty = true;
    m_flush->start();
}

void AgentTurnView::appendText(const QString &text)
{
    m_live = true;
    if (m_items.isEmpty() || m_items.last().kind != Block::Text)
        startItem(Block::Text);
    m_items.last().text += text;
    m_items.last().dirty = true;
    m_flush->start();
}

void AgentTurnView::flush()
{
    for (Item &it : m_items)
        if (it.dirty && it.view) {
            it.view->showMarkdown(it.text);
            it.dirty = false;
        }
}

void AgentTurnView::addTool(const QString &, const QString &call)
{
    m_live = true;
    flush();
    Item &it = startItem(Block::Tool);
    it.call = call;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    it.icon->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/refresh-cw.svg"), t.textMuted, 14));
    it.label->setText(QStringLiteral("<span style='color:%1'>%2…</span>").arg(t.textMuted.name(), escaped(call)));
}

void AgentTurnView::showToolResult(Item &it, bool ok, const QString &summary)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    it.icon->setPixmap(Icons::pixmap(ok ? QStringLiteral(":/new-icons/check.svg") : QStringLiteral(":/new-icons/circle-x.svg"), ok ? t.success : t.danger, 14));
    QString html = QStringLiteral("%1").arg(escaped(it.call));
    if (!summary.isEmpty())
        html += QStringLiteral("  <span style='color:%1'>— %2</span>").arg(ok ? t.textMuted.name() : t.danger.name(), escaped(summary.left(300)));
    it.label->setText(html);
    it.label->setToolTip(summary);
}

void AgentTurnView::finishTool(const ToolResult &r)
{
    int index = -1;
    for (int i = m_items.size() - 1; i >= 0; --i)
        if (m_items.at(i).kind == Block::Tool) {
            index = i;
            break;
        }
    if (index < 0)
        return;
    Item &it = m_items[index];
    if (r.hasChange && r.ok) { // an edit: the diff card replaces the plain row
        auto *card = new AiChangeCard(r.change, this);
        connect(card, &AiChangeCard::openRequested, this, &AgentTurnView::openRequested);
        const int at = m_layout->indexOf(it.widget);
        m_layout->removeWidget(it.widget);
        it.widget->deleteLater();
        m_layout->insertWidget(at, card);
        it.widget = card;
        it.icon = nullptr;
        it.label = nullptr;
        return;
    }
    showToolResult(it, r.ok, r.summary);
}

void AgentTurnView::endLive()
{
    m_live = false;
    m_flush->stop();
    flush();
    foldThoughts();
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).kind == Block::Thought)
            setThoughtOpen(i, m_items.at(i).open);
}

void AgentTurnView::setBlocks(const QVector<Block> &blocks)
{
    for (const Block &b : blocks) {
        if (b.kind == Block::Thought) {
            if (b.text.trimmed().isEmpty())
                continue;
            startItem(Block::Thought);
            m_items.last().text = b.text;
            m_items.last().dirty = true;
        } else if (b.kind == Block::Text) {
            if (b.text.trimmed().isEmpty())
                continue;
            startItem(Block::Text);
            m_items.last().text = b.text;
            m_items.last().dirty = true;
        } else {
            addTool(b.name, b.text);
            ToolResult r;
            r.ok = b.ok;
            r.summary = b.summary;
            r.hasChange = b.hasChange;
            r.change = b.change;
            finishTool(r);
        }
    }
    m_live = false;
    endLive();
}

void AgentTurnView::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    for (int i = 0; i < m_items.size(); ++i) {
        Item &it = m_items[i];
        if (it.kind == Block::Thought) {
            it.header->setIcon(Icons::tinted(it.open ? QStringLiteral(":/new-icons/chevron-down.svg") : QStringLiteral(":/new-icons/chevron-right.svg"), t.textMuted));
            it.view->setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; color: %1; }").arg(t.textMuted.name()));
        }
    }
    setStyleSheet(QStringLiteral("QToolButton#agentThoughtHeader { color: %1; border: none; padding: 2px 4px; background: transparent; }"
                                 "QLabel#agentToolText { background: transparent; }")
                      .arg(t.textMuted.name()));
}

} // namespace agent
