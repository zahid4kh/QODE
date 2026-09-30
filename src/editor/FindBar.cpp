#include "FindBar.h"

#include "CodeEditor.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

FindBar::FindBar(QWidget *parent)
    : QFrame(parent)
{
    setObjectName(QStringLiteral("findBar"));
    setFrameShape(QFrame::NoFrame);

    m_find = new QLineEdit(this);
    m_find->setPlaceholderText(tr("Find"));
    m_find->setClearButtonEnabled(true);
    m_count = new QLabel(this);
    m_count->setMinimumWidth(54);
    m_count->setAlignment(Qt::AlignCenter);

    m_case = new QToolButton(this);
    m_case->setObjectName(QStringLiteral("findBtn"));
    m_case->setText(QStringLiteral("Aa"));
    m_case->setCheckable(true);
    m_case->setToolTip(tr("Match case"));

    auto *prev = new QToolButton(this);
    prev->setObjectName(QStringLiteral("findBtn"));
    prev->setText(QStringLiteral("↑"));
    prev->setToolTip(tr("Previous match (Shift+Enter)"));
    auto *nextBtn = new QToolButton(this);
    nextBtn->setObjectName(QStringLiteral("findBtn"));
    nextBtn->setText(QStringLiteral("↓"));
    nextBtn->setToolTip(tr("Next match (Enter)"));
    auto *close = new QToolButton(this);
    close->setObjectName(QStringLiteral("findBtn"));
    close->setIcon(QIcon(QStringLiteral(":/icons/close.svg")));
    close->setToolTip(tr("Close (Esc)"));

    auto *findRow = new QHBoxLayout;
    findRow->setContentsMargins(0, 0, 0, 0);
    findRow->addWidget(new QLabel(tr("Find:"), this));
    findRow->addWidget(m_find, 1);
    findRow->addWidget(m_count);
    findRow->addWidget(m_case);
    findRow->addWidget(prev);
    findRow->addWidget(nextBtn);
    findRow->addWidget(close);

    m_replace = new QLineEdit(this);
    m_replace->setPlaceholderText(tr("Replace with"));
    m_replaceOne = new QPushButton(tr("Replace Next"), this);
    m_replaceAll = new QPushButton(tr("Replace All"), this);
    m_replaceOne->setAutoDefault(false);
    m_replaceAll->setAutoDefault(false);

    m_replaceRow = new QWidget(this);
    auto *replRow = new QHBoxLayout(m_replaceRow);
    replRow->setContentsMargins(0, 0, 0, 0);
    auto *replLabel = new QLabel(tr("Replace:"), m_replaceRow);
    replLabel->setMinimumWidth(m_count->fontMetrics().horizontalAdvance(tr("Find:")) + 2);
    replRow->addWidget(replLabel);
    replRow->addWidget(m_replace, 1);
    replRow->addWidget(m_replaceOne);
    replRow->addWidget(m_replaceAll);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 5, 8, 5);
    layout->setSpacing(4);
    layout->addLayout(findRow);
    layout->addWidget(m_replaceRow);

    m_find->installEventFilter(this);
    m_replace->installEventFilter(this);

    connect(m_find, &QLineEdit::textChanged, this, &FindBar::onTextChanged);
    connect(m_case, &QToolButton::toggled, this, &FindBar::onTextChanged);
    connect(nextBtn, &QToolButton::clicked, this, &FindBar::next);
    connect(prev, &QToolButton::clicked, this, &FindBar::previous);
    connect(close, &QToolButton::clicked, this, &FindBar::closeBar);
    connect(m_replaceOne, &QPushButton::clicked, this, [this] {
        if (m_editor && m_editor->replaceCurrent(m_replace->text()))
            updateCount();
    });
    connect(m_replaceAll, &QPushButton::clicked, this, [this] {
        if (!m_editor)
            return;
        const int n = m_editor->replaceAll(m_replace->text());
        m_count->setText(tr("%1 replaced").arg(n));
    });
    hide();
}

void FindBar::setEditor(CodeEditor *editor)
{
    if (m_editor == editor)
        return;
    if (m_editor) {
        disconnect(m_editor, nullptr, this, nullptr);
        m_editor->setSearchTerm({}, false);
    }
    m_editor = editor;
    if (!m_editor) {
        hide();
        return;
    }
    connect(m_editor, &CodeEditor::searchResultsChanged, this, &FindBar::updateCount);
    if (isVisible())
        onTextChanged();
}

void FindBar::show_(bool replace)
{
    m_replaceRow->setVisible(replace);
    show();
    if (m_editor) {
        const QString sel = m_editor->textCursor().selectedText();
        if (!sel.isEmpty() && !sel.contains(QChar::ParagraphSeparator))
            m_find->setText(sel);
    }
    m_find->setFocus();
    m_find->selectAll();
    onTextChanged();
}

void FindBar::showFind() { show_(false); }
void FindBar::showReplace() { show_(true); }

void FindBar::onTextChanged()
{
    if (!m_editor)
        return;
    m_editor->setSearchTerm(m_find->text(), m_case->isChecked());
    if (!m_find->text().isEmpty() && m_editor->matchCount() > 0) {
        // Incremental search: jump to the first match at/after where the search started.
        QTextCursor c = m_editor->textCursor();
        c.setPosition(c.selectionStart());
        m_editor->setTextCursor(c);
        m_editor->findNext();
    }
    updateCount();
}

void FindBar::updateCount()
{
    if (!m_editor || m_find->text().isEmpty()) {
        m_count->clear();
        return;
    }
    const int total = m_editor->matchCount();
    m_count->setText(total == 0 ? tr("No results") : QStringLiteral("%1/%2").arg(m_editor->currentMatchIndex()).arg(total));
}

void FindBar::next()
{
    if (m_editor)
        m_editor->findNext(false);
    updateCount();
}

void FindBar::previous()
{
    if (m_editor)
        m_editor->findNext(true);
    updateCount();
}

void FindBar::closeBar()
{
    hide();
    if (m_editor) {
        m_editor->setSearchTerm({}, false);
        m_editor->setFocus();
    }
}

bool FindBar::eventFilter(QObject *obj, QEvent *ev)
{
    if (ev->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent *>(ev);
        if (k->key() == Qt::Key_Escape) {
            closeBar();
            return true;
        }
        if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) {
            if (obj == m_find) {
                (k->modifiers() & Qt::ShiftModifier) ? previous() : next();
            } else if (m_editor) {
                m_editor->replaceCurrent(m_replace->text());
                updateCount();
            }
            return true;
        }
    }
    return QFrame::eventFilter(obj, ev);
}
