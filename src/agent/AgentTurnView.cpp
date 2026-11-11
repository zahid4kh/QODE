#include "AgentTurnView.h"

#include "ai/AiChangeCard.h"
#include "media/CopyButton.h"
#include "media/MarkdownView.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "window/WrapLabel.h"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

namespace agent {

namespace {
QString escaped(const QString &s)
{
    return s.toHtmlEscaped();
}

// A tool row's call: `quoted` parts (commands, patterns) in the editor font instead of showing the backticks, and a
// leading **Name** (after an optional "↳ ") in bold.
QString callHtml(QString call)
{
    QString bold;
    const int start = call.startsWith(QStringLiteral("↳ ")) ? 2 : 0;
    if (call.mid(start, 2) == QLatin1String("**")) {
        const int end = call.indexOf(QStringLiteral("**"), start + 2);
        if (end > start + 2) {
            bold = escaped(call.left(start)) + QStringLiteral("<b>%1</b>&nbsp; ").arg(escaped(call.mid(start + 2, end - start - 2)));
            call = call.mid(end + 2).trimmed();
        }
    }
    const QStringList parts = call.split(QLatin1Char('`'));
    if (parts.size() < 3)
        return bold + escaped(call);
    const QString font = SettingsManager::instance().editorFont().family().toHtmlEscaped();
    QString html;
    for (int i = 0; i < parts.size(); ++i) {
        const bool code = i % 2 == 1 && i + 1 < parts.size(); // an unpaired backtick stays text
        html += code ? QStringLiteral("<span style=\"font-family:'%1'\">%2</span>").arg(font, escaped(parts.at(i)))
                     : (i % 2 == 1 ? QStringLiteral("`") : QString()) + escaped(parts.at(i));
    }
    return bold + html.replace(QStringLiteral(" <span"), QStringLiteral("&nbsp; <span")); // the UI font's narrow space looks cramped before code
}

constexpr int kToolMaxChars = 200000; // of a tool's input or output shown in its folded-out row

// The IN or OUT part of a folded-out tool row: wrapped, in the editor font, at most `maxLines` tall and scrolling beyond.
class ToolTextView : public QPlainTextEdit
{
public:
    ToolTextView(int maxLines, QWidget *parent)
        : QPlainTextEdit(parent)
        , m_maxLines(maxLines)
    {
        setObjectName(QStringLiteral("agentToolBody"));
        setReadOnly(true);
        setFrameShape(QFrame::NoFrame);
        setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        document()->setDocumentMargin(5);
        connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this, [this] { fitHeight(); });
        applyTheme();
    }

    void setBody(const QString &text)
    {
        QString shown = text.size() > kToolMaxChars ? text.left(kToolMaxChars) + QStringLiteral("\n…") : text;
        while (shown.endsWith(QLatin1Char('\n')))
            shown.chop(1);
        if (shown != toPlainText())
            setPlainText(shown);
        fitHeight();
    }

    void applyTheme()
    {
        QFont f = SettingsManager::instance().editorFont();
        f.setPointSizeF(qMax(8.0, f.pointSizeF() - 1));
        setFont(f);
        QPalette pal = palette();
        pal.setColor(QPalette::PlaceholderText, Theme::byName(SettingsManager::instance().theme()).agentView().textMuted);
        setPalette(pal);
        fitHeight();
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QPlainTextEdit::resizeEvent(e);
        fitHeight();
    }

    void wheelEvent(QWheelEvent *e) override
    {
        // It scrolls while it can; at its ends the wheel goes on to the chat.
        QScrollBar *bar = verticalScrollBar();
        const int dy = e->angleDelta().y();
        if (dy > 0 ? bar->value() <= bar->minimum() : bar->value() >= bar->maximum()) {
            e->ignore();
            return;
        }
        QPlainTextEdit::wheelEvent(e);
    }

private:
    void fitHeight()
    {
        // A plain text layout measures its height in lines (wrapped ones included).
        const int lines = qBound(1, int(std::ceil(document()->documentLayout()->documentSize().height())), m_maxLines);
        const int h = int(std::ceil(lines * QFontMetricsF(font()).lineSpacing() + 2 * document()->documentMargin())) + 4;
        if (h != height())
            setFixedHeight(h);
    }

    int m_maxLines;
};

QString tokens(int n)
{
    return n >= 1000 ? QStringLiteral("%1k").arg(n / 1000.0, 0, 'f', n >= 10000 ? 0 : 1) : QString::number(n);
}

QString duration(qint64 ms)
{
    if (ms < 60000)
        return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1 m %2 s").arg(ms / 60000).arg((ms % 60000) / 1000);
}

QString inText(int prompt, int cached)
{
    QString s = AgentTurnView::tr("%1 in").arg(tokens(prompt));
    if (cached > 0)
        s += AgentTurnView::tr(" (%1 cached)").arg(tokens(cached));
    return s;
}

QString outText(int output, int thinking, bool exact)
{
    QString s = (exact ? QString() : QStringLiteral("~")) + AgentTurnView::tr("%1 out").arg(tokens(output));
    if (thinking > 0)
        s += AgentTurnView::tr(" (%1 thinking)").arg(tokens(thinking));
    return s;
}

// "4.2 s · first token 0.8 s · thought 3.1 s · 12k in (11k cached) · ~340 out (200 thinking)"
QString requestText(const RequestStats &r)
{
    QStringList parts{duration(r.ms)};
    if (r.firstMs >= 0)
        parts << AgentTurnView::tr("first token %1").arg(duration(r.firstMs));
    if (r.thinkMs > 0)
        parts << AgentTurnView::tr("thought %1").arg(duration(r.thinkMs));
    if (r.prompt > 0)
        parts << inText(r.prompt, r.cached);
    parts << outText(r.output, r.thinking, r.exact);
    if (r.stopped)
        parts << AgentTurnView::tr("stopped");
    return parts.join(QStringLiteral(" · "));
}

// Every request resends the whole conversation, so the summed input is far bigger than the conversation itself; the
// context (the last request's input) is shown first.
QString contextText(int context)
{
    return AgentTurnView::tr("context %1").arg(tokens(context));
}

QString totalInText(int prompt, int cached)
{
    QString s = AgentTurnView::tr("%1 in total").arg(tokens(prompt));
    if (cached > 0)
        s += AgentTurnView::tr(" (%1 cached)").arg(tokens(cached));
    return s;
}

// Totals of a turn: "3 requests · 24.1 s · thought 8.2 s · context 6.9k · 45k in total (40k cached) · 1.2k out (812 thinking)"
QString totalText(const QVector<RequestStats> &log, qint64 turnMs)
{
    int context = 0;
    for (const RequestStats &r : log)
        if (r.prompt > 0)
            context = r.prompt;
    RequestStats sum;
    for (const RequestStats &r : log) {
        sum.prompt += r.prompt;
        sum.cached += r.cached;
        sum.output += r.output;
        sum.thinking += r.thinking;
        sum.thinkMs += r.thinkMs;
        sum.ms += r.ms;
        sum.exact = sum.exact && r.exact;
    }
    QStringList parts{AgentTurnView::tr("%n request(s)", nullptr, int(log.size())), duration(qMax(turnMs, sum.ms))};
    if (sum.thinkMs > 0)
        parts << AgentTurnView::tr("thought %1").arg(duration(sum.thinkMs));
    if (context > 0)
        parts << contextText(context);
    parts << totalInText(sum.prompt, sum.cached) << outText(sum.output, sum.thinking, sum.exact);
    return parts.join(QStringLiteral(" · "));
}
}

void useTabularDigits(QWidget *w)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    QFont f = w->font();
    f.setFeature(QFont::Tag("tnum"), 1);
    w->setFont(f);
#else
    Q_UNUSED(w)
#endif
}

AgentTurnView::AgentTurnView(const QString &model, QWidget *parent, const QString &name)
    : QWidget(parent)
    , m_layout(new QVBoxLayout(this))
    , m_flush(new QTimer(this))
{
    m_layout->setContentsMargins(2, 0, 2, 0);
    m_layout->setSpacing(6);
    m_name = name.isEmpty() ? tr("QODE") : name;
    m_who = new QLabel(this);
    m_who->setObjectName(QStringLiteral("aiWho"));
    m_layout->addWidget(m_who);
    setModel(model);

    m_flush->setSingleShot(true);
    m_flush->setInterval(70); // re-rendering the Markdown on every streamed fragment would be wasteful
    connect(m_flush, &QTimer::timeout, this, &AgentTurnView::flush);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &AgentTurnView::applyTheme);
    applyTheme();
}

void AgentTurnView::setModel(const QString &model)
{
    m_who->setText(model.isEmpty() ? m_name : tr("%1 · %2").arg(m_name, model));
}

MarkdownView *AgentTurnView::makeView(QWidget *parent, bool muted)
{
    auto *view = new MarkdownView(parent);
    view->document()->setDocumentMargin(2);
    view->setStyleSheet(muted ? QStringLiteral("QTextBrowser { background: transparent; color: %1; }").arg(Theme::byName(SettingsManager::instance().theme()).agentView().textMuted.name())
                              : QStringLiteral("QTextBrowser { background: transparent; }"));
    view->setAutoHeight(true);
    view->setChatActions(true);
    view->setZoom(m_zoom);
    connect(view, &MarkdownView::zoomChanged, this, [this, view](int percent) {
        if (percent == m_zoom)
            return;
        m_zoom = percent;
        for (const Item &it : std::as_const(m_items))
            if (it.view && it.view != view)
                it.view->setZoom(percent);
    });
    connect(view, &MarkdownView::askRequested, this, &AgentTurnView::askRequested);
    connect(view, &MarkdownView::quoteRequested, this, &AgentTurnView::quoteRequested);
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
        auto *box = new QWidget(this);
        auto *bl = new QVBoxLayout(box);
        bl->setContentsMargins(0, 0, 0, 0);
        bl->setSpacing(0);
        auto *row = new QFrame(box);
        row->setObjectName(QStringLiteral("agentToolRow"));
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(4, 1, 4, 1);
        l->setSpacing(6);
        it.chevron = new QLabel(row);
        it.chevron->setFixedSize(12, 14);
        it.chevron->setVisible(false);
        it.icon = new QLabel(row);
        it.icon->setFixedSize(14, 14);
        it.label = new WrapLabel(row); // hundreds of rows in a long conversation: a plain wrapped QLabel made resizing crawl
        it.label->setObjectName(QStringLiteral("agentToolText"));
        it.label->setTextFormat(Qt::RichText);
        it.label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->addWidget(it.chevron, 0, Qt::AlignTop);
        l->addWidget(it.icon, 0, Qt::AlignTop);
        l->addWidget(it.label, 1);
        bl->addWidget(row);
        it.widget = box;
    }
    m_layout->insertWidget(m_stats ? m_layout->indexOf(m_stats) : m_layout->count(), it.widget); // the stats stay last
    m_items.append(it);
    return m_items.last();
}

void AgentTurnView::setThoughtOpen(int index, bool open)
{
    Item &it = m_items[index];
    it.open = open;
    if (it.view)
        it.view->setVisible(open);
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
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
            const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
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
    if (!m_copy || m_copy->isEnabled())
        updateCopy(); // there is text now, but it is copied once the turn is over
}

void AgentTurnView::flush()
{
    for (Item &it : m_items)
        if (it.dirty && it.view) {
            it.view->showMarkdown(it.text);
            it.dirty = false;
        }
}

int AgentTurnView::addTool(const QString &, const QString &call, const QString &input)
{
    m_live = true;
    flush();
    Item &it = startItem(Block::Tool);
    const int index = m_items.size() - 1;
    it.call = call;
    it.input = input;
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    it.icon->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/refresh-cw.svg"), t.textMuted, 14));
    it.label->setText(QStringLiteral("<span style='color:%1'>%2…</span>").arg(t.textMuted.name(), callHtml(call)));
    if (!input.isEmpty()) {
        // The whole row folds the details out; the full call is selectable there, so the row text is not.
        QWidget *row = it.label->parentWidget();
        row->setObjectName(QStringLiteral("agentToolRowFold"));
        row->setCursor(Qt::PointingHandCursor);
        row->setProperty("toolIndex", index);
        row->installEventFilter(this);
        it.label->setTextInteractionFlags(Qt::NoTextInteraction);
        it.chevron->setVisible(true);
        setToolOpen(index, false);
    }
    return index;
}

bool AgentTurnView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
        const QVariant index = watched->property("toolIndex");
        if (index.isValid() && index.toInt() < m_items.size() && m_items.at(index.toInt()).chevron) {
            setToolOpen(index.toInt(), !m_items.at(index.toInt()).open);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AgentTurnView::setToolOpen(int index, bool open)
{
    Item &it = m_items[index];
    if (!it.chevron)
        return;
    it.open = open;
    if (open)
        fillToolDetails(it);
    if (it.details)
        it.details->setVisible(open);
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    it.chevron->setPixmap(Icons::pixmap(open ? QStringLiteral(":/new-icons/chevron-down.svg") : QStringLiteral(":/new-icons/chevron-right.svg"), t.textMuted, 12));
    it.label->parentWidget()->setToolTip(open ? tr("Hide the input and output") : tr("Show the input and output"));
    if (!open)
        return;
    // Bring the folded-out part on screen once the layout has made room for it.
    QTimer::singleShot(0, this, [this, index] {
        if (index >= m_items.size() || !m_items.at(index).open || !m_items.at(index).details)
            return;
        QWidget *w = parentWidget();
        while (w && !qobject_cast<QScrollArea *>(w))
            w = w->parentWidget();
        if (auto *area = qobject_cast<QScrollArea *>(w))
            area->ensureWidgetVisible(m_items.at(index).details, 0, 8);
    });
}

void AgentTurnView::fillToolDetails(Item &it)
{
    if (!it.details) {
        it.details = new QWidget(it.widget);
        auto *outer = new QVBoxLayout(it.details);
        outer->setContentsMargins(22, 2, 4, 6); // under the row's text
        auto *frame = new QFrame(it.details);
        frame->setObjectName(QStringLiteral("agentToolDetails"));
        outer->addWidget(frame);
        auto *grid = new QGridLayout(frame);
        grid->setContentsMargins(1, 1, 1, 1);
        grid->setSpacing(0);
        grid->setColumnStretch(1, 1);
        auto part = [&](int row, const QString &tag, int maxLines) {
            auto *label = new QLabel(tag, frame);
            label->setObjectName(QStringLiteral("agentToolTag"));
            label->setContentsMargins(10, 6, 6, 0);
            QFont f = SettingsManager::instance().editorFont();
            f.setPointSizeF(qMax(7.0, f.pointSizeF() - 2));
            label->setFont(f);
            auto *view = new ToolTextView(maxLines, frame);
            grid->addWidget(label, row, 0, Qt::AlignTop);
            grid->addWidget(view, row, 1);
            return view;
        };
        it.inView = part(0, tr("IN"), 8);
        auto *rule = new QFrame(frame);
        rule->setObjectName(QStringLiteral("agentToolRule"));
        rule->setFixedHeight(1);
        grid->addWidget(rule, 1, 0, 1, 2);
        it.outView = part(2, tr("OUT"), 16);
        static_cast<QVBoxLayout *>(it.widget->layout())->addWidget(it.details);
    }
    static_cast<ToolTextView *>(it.inView)->setBody(it.input);
    auto *out = static_cast<ToolTextView *>(it.outView);
    out->setBody(it.output);
    out->setPlaceholderText(it.finished ? tr("No output") : tr("Running…"));
    const QString name = it.finished && !it.ok ? QStringLiteral("agentToolBodyFailed") : QStringLiteral("agentToolBody");
    if (out->objectName() != name) {
        out->setObjectName(name);
        out->style()->unpolish(out);
        out->style()->polish(out);
    }
}

void AgentTurnView::showToolResult(Item &it, bool ok, const QString &summary)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    it.icon->setPixmap(Icons::pixmap(ok ? QStringLiteral(":/new-icons/check.svg") : QStringLiteral(":/new-icons/circle-x.svg"), ok ? t.success : t.danger, 14));
    QString html = callHtml(it.call);
    // One short line after the call; the tooltip has all of it.
    QString note = summary.simplified();
    if (note.size() > 120)
        note = note.left(120) + QStringLiteral("…");
    if (!note.isEmpty())
        html += QStringLiteral("  <span style='color:%1'>— %2</span>").arg(ok ? t.textMuted.name() : t.danger.name(), escaped(note));
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
    finishToolAt(index, r);
}

void AgentTurnView::finishToolAt(int index, const ToolResult &r)
{
    if (index < 0 || index >= m_items.size() || m_items.at(index).kind != Block::Tool)
        return;
    Item &it = m_items[index];
    it.finished = true;
    it.ok = r.ok;
    it.output = r.output;
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
        it.chevron = nullptr;
        it.details = nullptr;
        it.inView = it.outView = nullptr;
        return;
    }
    showToolResult(it, r.ok, r.summary);
    if (it.details)
        fillToolDetails(it);
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
    updateCopy();
}

QString AgentTurnView::replyText() const
{
    QStringList parts;
    for (const Item &it : m_items)
        if (it.kind == Block::Text && !it.text.trimmed().isEmpty())
            parts << it.text.trimmed();
    return parts.join(QStringLiteral("\n\n"));
}

void AgentTurnView::setCopyPinned(bool pinned)
{
    m_copyPinned = pinned;
    if (m_copy)
        m_copy->setPinned(pinned);
}

void AgentTurnView::updateCopy()
{
    const bool any = !replyText().isEmpty();
    if (!any && !m_copy)
        return;
    ensureStats();
    m_copy->setAvailable(any);
    m_copy->setEnabled(!m_live);
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

void AgentTurnView::ensureStats()
{
    if (m_stats)
        return;
    m_stats = new QWidget(this);
    auto *l = new QVBoxLayout(m_stats);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    m_statsHeader = new QToolButton(m_stats);
    m_statsHeader->setObjectName(QStringLiteral("agentStatsHeader"));
    m_statsHeader->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_statsHeader->setAutoRaise(true);
    m_statsHeader->setCursor(Qt::PointingHandCursor);
    m_statsHeader->setToolTip(tr("Time and tokens of every request. Each request resends the whole conversation: \"context\" is the size of "
                                 "the last one, \"in total\" the sum over all requests (cached tokens are billed at a discount). "
                                 "~ = estimated while streaming."));
    m_statsHeader->setVisible(false); // until there are stats
    // The footer row: the stats, and the reply's Copy button at its end.
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    row->addWidget(m_statsHeader, 0, Qt::AlignLeft);
    row->addStretch(1);
    m_copy = new CopyButton(m_stats);
    m_copy->setSource([this] { return replyText(); });
    m_copy->setAvailable(false);
    m_copy->setPinned(m_copyPinned);
    m_copy->revealOnHover(this);
    row->addWidget(m_copy);
    l->addLayout(row);
    m_statsRows = new QLabel(m_stats);
    m_statsRows->setObjectName(QStringLiteral("agentStatsRows"));
    m_statsRows->setTextFormat(Qt::RichText);
    m_statsRows->setWordWrap(true);
    m_statsRows->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statsRows->setContentsMargins(20, 0, 4, 2);
    m_statsRows->setVisible(false);
    l->addWidget(m_statsRows);
    connect(m_statsHeader, &QToolButton::clicked, this, [this] { setStatsOpen(!m_statsOpen); });
    m_layout->addWidget(m_stats);
    setStatsOpen(false);
}

void AgentTurnView::setStatsOpen(bool open)
{
    m_statsOpen = open;
    m_statsRows->setVisible(open && !m_statsRows->text().isEmpty());
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    m_statsHeader->setIcon(Icons::tinted(open ? QStringLiteral(":/new-icons/chevron-down.svg") : QStringLiteral(":/new-icons/chevron-right.svg"), t.textMuted));
    m_statsHeader->setIconSize(QSize(12, 12));
}

void AgentTurnView::showStats(const QString &summary, const QStringList &rows)
{
    ensureStats();
    m_statsHeader->setText(QLatin1Char(' ') + summary);
    m_statsHeader->setVisible(true);
    QString html;
    for (int i = 0; i < rows.size(); ++i)
        html += QStringLiteral("%1<b>#%2</b>&nbsp;&nbsp;%3").arg(i ? QStringLiteral("<br>") : QString()).arg(i + 1).arg(escaped(rows.at(i)));
    m_statsRows->setText(html);
    m_statsRows->setVisible(m_statsOpen && !html.isEmpty());
}

void AgentTurnView::setLiveStats(const QVector<RequestStats> &done, const RequestStats &current, bool streaming, qint64 turnMs)
{
    QStringList rows;
    for (const RequestStats &r : done)
        rows << requestText(r);
    if (!streaming) { // between requests (tools running)
        if (!done.isEmpty())
            showStats(totalText(done, turnMs), rows);
        return;
    }
    rows << requestText(current);
    QString summary = tr("Request %1 · %2").arg(done.size() + 1).arg(requestText(current));
    if (!done.isEmpty())
        summary += tr("   ·   turn %1").arg(formatElapsed(turnMs));
    showStats(summary, rows);
}

void AgentTurnView::setStats(const Message &m)
{
    if (!m.requestLog.isEmpty()) {
        QStringList rows;
        for (const RequestStats &r : m.requestLog)
            rows << requestText(r);
        showStats(totalText(m.requestLog, m.ms), rows);
    } else if (m.requests > 0) { // saved before per-request stats existed
        QStringList parts{tr("%n request(s)", nullptr, m.requests)};
        if (m.contextTokens > 0)
            parts << contextText(m.contextTokens);
        parts << totalInText(m.promptTokens, m.cachedTokens) << outText(m.outputTokens, 0, true);
        showStats(parts.join(QStringLiteral(" · ")), {});
    }
}

void AgentTurnView::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme()).agentView();
    for (int i = 0; i < m_items.size(); ++i) {
        Item &it = m_items[i];
        if (it.kind == Block::Thought) {
            it.header->setIcon(Icons::tinted(it.open ? QStringLiteral(":/new-icons/chevron-down.svg") : QStringLiteral(":/new-icons/chevron-right.svg"), t.textMuted));
            it.view->setStyleSheet(QStringLiteral("QTextBrowser { background: transparent; color: %1; }").arg(t.textMuted.name()));
        } else if (it.kind == Block::Tool && it.chevron) {
            setToolOpen(i, it.open);
            if (it.details) {
                static_cast<ToolTextView *>(it.inView)->applyTheme();
                static_cast<ToolTextView *>(it.outView)->applyTheme();
            }
        }
    }
    if (m_stats)
        setStatsOpen(m_statsOpen);
    setStyleSheet(QStringLiteral("QToolButton#agentThoughtHeader, QToolButton#agentStatsHeader { color: %1; border: none; padding: 2px 4px; background: transparent; }"
                                 "QLabel#agentStatsRows { color: %1; background: transparent; }"
                                 "QLabel#agentToolText { background: transparent; }"
                                 "QFrame#agentToolRowFold { border-radius: 4px; }"
                                 "QFrame#agentToolRowFold:hover { background: %2; }"
                                 "QFrame#agentToolDetails { background: %3; border: 1px solid %4; border-radius: 6px; }"
                                 "QFrame#agentToolRule { background: %4; border: none; }"
                                 "QLabel#agentToolTag { color: %1; background: transparent; }"
                                 "QPlainTextEdit#agentToolBody, QPlainTextEdit#agentToolBodyFailed { background: transparent; border: none; color: %5; }"
                                 "QPlainTextEdit#agentToolBodyFailed { color: %6; }")
                      .arg(t.textMuted.name(), t.currentLine.name(), t.editorBg.name(), t.border.name(), t.editorFg.name(), t.danger.name()));
}

} // namespace agent
