#include "LspLogDialog.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSyntaxHighlighter>
#include <QVBoxLayout>

namespace {
// Errors red, warnings amber, everything else as is.
class LogHighlighter : public QSyntaxHighlighter
{
public:
    LogHighlighter(QTextDocument *doc, const Theme &t)
        : QSyntaxHighlighter(doc)
        , m_error(t.gitConflict)
        , m_warn(t.gitModified)
        , m_dim(t.textMuted)
    {
    }

protected:
    void highlightBlock(const QString &text) override
    {
        static const QRegularExpression error(QStringLiteral("\\b(error|exception|fatal|failed|cannot|crash)"), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression warn(QStringLiteral("\\b(warn|warning|deprecated)"), QRegularExpression::CaseInsensitiveOption);
        QTextCharFormat f;
        if (error.match(text).hasMatch())
            f.setForeground(m_error);
        else if (warn.match(text).hasMatch())
            f.setForeground(m_warn);
        else
            return;
        setFormat(0, text.size(), f);
    }

private:
    QColor m_error, m_warn, m_dim;
};
} // namespace

LspLogDialog::LspLogDialog(const QString &serverName, std::function<QStringList()> provider, QWidget *parent)
    : QDialog(parent)
    , m_provider(std::move(provider))
{
    setWindowTitle(tr("%1 — server log").arg(serverName));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    setSizeGripEnabled(true);
    resize(980, 620);
    setMinimumSize(520, 320);

    const Theme t = Theme::byName(SettingsManager::instance().theme());
    setStyleSheet(QStringLiteral(
                      "LspLogDialog { background: %1; }"
                      "QLabel#logTitle { font-size: 12pt; font-weight: 600; color: %2; }"
                      "QLabel#logCount { color: %3; }"
                      "QLineEdit { background: %4; border: 1px solid %5; border-radius: 6px; padding: 5px 9px; color: %2; }"
                      "QLineEdit:focus { border-color: %6; }"
                      "QPlainTextEdit { background: %4; color: %2; border: 1px solid %5; border-radius: 8px; padding: 8px;"
                      " selection-background-color: %7; selection-color: %2; }"
                      "QPushButton { background: %8; color: %2; border: 1px solid %5; border-radius: 6px; padding: 6px 14px; }"
                      "QPushButton:hover { background: %7; }"
                      "QPushButton#primary { background: %6; color: %9; border-color: %6; }"
                      "QPushButton#primary:hover { background: %6; }")
                      .arg(t.window.name(), t.editorFg.name(), t.textMuted.name(), t.editorBg.name(), t.border.name(), t.accent.name(),
                           t.selection.name(), t.panel.name(), t.onAccent().name()));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);

    auto *head = new QHBoxLayout;
    auto *title = new QLabel(serverName, this);
    title->setObjectName(QStringLiteral("logTitle"));
    m_count = new QLabel(this);
    m_count->setObjectName(QStringLiteral("logCount"));
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(m_count);
    layout->addLayout(head);

    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter lines…"));
    m_filter->setClearButtonEnabled(true);
    layout->addWidget(m_filter);

    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true); // selectable and copyable (Ctrl+C, Ctrl+A, right click)
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setFont(SettingsManager::instance().editorFont());
    m_view->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_view->setMaximumBlockCount(0);
    new LogHighlighter(m_view->document(), t);
    layout->addWidget(m_view, 1);

    auto *buttons = new QHBoxLayout;
    auto *refresh = new QPushButton(tr("Refresh"), this);
    auto *copyAll = new QPushButton(tr("Copy All"), this);
    auto *close = new QPushButton(tr("Close"), this);
    close->setObjectName(QStringLiteral("primary"));
    close->setDefault(true);
    buttons->addWidget(refresh);
    buttons->addWidget(copyAll);
    buttons->addStretch(1);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_filter, &QLineEdit::textChanged, this, &LspLogDialog::applyFilter);
    connect(refresh, &QPushButton::clicked, this, &LspLogDialog::reload);
    connect(copyAll, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_lines.join(QLatin1Char('\n'))); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    reload();
}

void LspLogDialog::reload()
{
    m_lines = m_provider ? m_provider() : QStringList();
    applyFilter();
}

void LspLogDialog::applyFilter()
{
    const QString needle = m_filter->text();
    QStringList shown;
    for (const QString &l : std::as_const(m_lines))
        if (needle.isEmpty() || l.contains(needle, Qt::CaseInsensitive))
            shown << l;
    QScrollBar *bar = m_view->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 4;
    m_view->setPlainText(shown.isEmpty() ? (m_lines.isEmpty() ? tr("The server has not written anything to its log yet.") : tr("No line matches the filter."))
                                         : shown.join(QLatin1Char('\n')));
    if (atEnd || bar->maximum() == 0)
        bar->setValue(bar->maximum()); // newest messages are at the bottom
    m_count->setText(needle.isEmpty() ? tr("%n line(s)", nullptr, int(m_lines.size())) : tr("%1 of %n line(s)", nullptr, int(m_lines.size())).arg(shown.size()));
}
