#include "DiffDialog.h"

#include "GitRepository.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QVBoxLayout>

// --- DiffView: read-only text with a line-number gutter and per-row tinting --------------

class DiffView : public QPlainTextEdit
{
public:
    enum RowKind : char { Equal = 0, Changed = 1, Filler = 2 };

    explicit DiffView(QWidget *parent = nullptr)
        : QPlainTextEdit(parent)
        , m_gutter(new Gutter(this))
    {
        setReadOnly(true);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setFrameShape(QFrame::NoFrame);
        connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect &r, int dy) {
            if (dy)
                m_gutter->scroll(0, dy);
            else
                m_gutter->update(0, r.y(), m_gutter->width(), r.height());
        });
        setViewportMargins(gutterWidth(), 0, 0, 0);
    }

    void setColors(const Theme &t, bool additions)
    {
        m_t = t;
        m_changeBg = additions ? t.diffAddBg : t.diffDelBg;
        m_fillBg = t.diffFillBg;
        QPalette p = palette();
        p.setColor(QPalette::Base, t.editorBg);
        p.setColor(QPalette::Text, t.editorFg);
        setPalette(p);
        m_gutter->update();
    }

    void setRows(const QStringList &lines, const QVector<int> &numbers, const QVector<char> &kinds)
    {
        m_numbers = numbers;
        setFont(SettingsManager::instance().editorFont());
        setPlainText(lines.join(QLatin1Char('\n')));
        setViewportMargins(gutterWidth(), 0, 0, 0);
        QList<QTextEdit::ExtraSelection> extra;
        for (int i = 0; i < kinds.size(); ++i) {
            if (kinds.at(i) == Equal)
                continue;
            QTextEdit::ExtraSelection s;
            s.format.setBackground(kinds.at(i) == Changed ? m_changeBg : m_fillBg);
            s.format.setProperty(QTextFormat::FullWidthSelection, true);
            s.cursor = QTextCursor(document()->findBlockByNumber(i));
            extra.append(s);
        }
        setExtraSelections(extra);
        resizeGutter();
    }

    void goToRow(int row)
    {
        setTextCursor(QTextCursor(document()->findBlockByNumber(row)));
        centerCursor();
    }

    void gutterPaint(QPaintEvent *event)
    {
        QPainter p(m_gutter);
        p.fillRect(event->rect(), m_t.gutterBg);
        p.setPen(m_t.border);
        p.drawLine(m_gutter->width() - 1, event->rect().top(), m_gutter->width() - 1, event->rect().bottom());
        p.setPen(m_t.gutterFg);
        p.setFont(font());
        QTextBlock block = firstVisibleBlock();
        int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
        while (block.isValid() && top <= event->rect().bottom()) {
            const int bottom = top + qRound(blockBoundingRect(block).height());
            const int n = m_numbers.value(block.blockNumber(), 0);
            if (n > 0 && bottom >= event->rect().top())
                p.drawText(0, top, m_gutter->width() - 8, fontMetrics().height(), Qt::AlignRight, QString::number(n));
            block = block.next();
            top = bottom;
        }
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QPlainTextEdit::resizeEvent(e);
        resizeGutter();
    }

private:
    class Gutter : public QWidget
    {
    public:
        explicit Gutter(DiffView *v) : QWidget(v), m_view(v) {}
        QSize sizeHint() const override { return QSize(m_view->gutterWidth(), 0); }

    protected:
        void paintEvent(QPaintEvent *e) override { m_view->gutterPaint(e); }

    private:
        DiffView *m_view;
    };

    int gutterWidth() const
    {
        int max = 1;
        for (int n : m_numbers)
            max = qMax(max, n);
        int digits = 3;
        while (max >= 1000) {
            max /= 10;
            ++digits;
        }
        return 14 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
    }
    void resizeGutter()
    {
        const QRect cr = contentsRect();
        m_gutter->setGeometry(QRect(cr.left(), cr.top(), gutterWidth(), cr.height()));
    }

    Gutter *m_gutter;
    QVector<int> m_numbers;
    Theme m_t;
    QColor m_changeBg, m_fillBg;
};

// --- DiffDialog -------------------------------------------------------------------------------

DiffDialog::DiffDialog(GitRepository *repo, const QString &absPath, GitDiffMode mode, QWidget *parent)
    : QDialog(parent, Qt::Window)
    , m_repo(repo)
    , m_path(absPath)
    , m_mode(mode)
{
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1180, 720);

    m_title = new QLabel(this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_stats = new QLabel(this);
    m_stats->setObjectName(QStringLiteral("emptyText"));
    m_prev = new QPushButton(tr("▲ Previous"), this);
    m_next = new QPushButton(tr("▼ Next"), this);
    m_action = new QPushButton(this);
    m_action->setVisible(mode != GitDiffMode::Head);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(8, 6, 8, 6);
    bar->addWidget(m_stats, 1);
    bar->addWidget(m_prev);
    bar->addWidget(m_next);
    bar->addWidget(m_action);

    m_oldLabel = new QLabel(this);
    m_newLabel = new QLabel(this);
    m_oldLabel->setObjectName(QStringLiteral("panelTitle"));
    m_newLabel->setObjectName(QStringLiteral("panelTitle"));
    m_left = new DiffView(this);
    m_right = new DiffView(this);

    auto column = [](QLabel *label, DiffView *view) {
        auto *w = new QWidget;
        auto *l = new QVBoxLayout(w);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(0);
        l->addWidget(label);
        l->addWidget(view, 1);
        return w;
    };
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->addWidget(column(m_oldLabel, m_left));
    split->addWidget(column(m_newLabel, m_right));
    split->setChildrenCollapsible(false);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(m_title);
    root->addLayout(bar);
    root->addWidget(split, 1);

    // Keep both panes scrolled together.
    auto link = [](QScrollBar *a, QScrollBar *b) {
        QObject::connect(a, &QScrollBar::valueChanged, b, &QScrollBar::setValue);
        QObject::connect(b, &QScrollBar::valueChanged, a, &QScrollBar::setValue);
    };
    link(m_left->verticalScrollBar(), m_right->verticalScrollBar());
    link(m_left->horizontalScrollBar(), m_right->horizontalScrollBar());

    connect(m_prev, &QPushButton::clicked, this, [this] { gotoChange(false); });
    connect(m_next, &QPushButton::clicked, this, [this] { gotoChange(true); });
    connect(m_action, &QPushButton::clicked, this, [this] {
        if (m_mode == GitDiffMode::Staged)
            m_repo->unstage({m_path});
        else
            m_repo->stage({m_path});
    });
    connect(m_repo, &GitRepository::statusChanged, this, &DiffDialog::load);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &DiffDialog::applyTheme);

    const QString name = QFileInfo(m_path).fileName();
    const QString modeText = mode == GitDiffMode::Staged ? tr("Staged Changes") : mode == GitDiffMode::Unstaged ? tr("Changes") : tr("Changes vs HEAD");
    setWindowTitle(tr("%1 (%2)").arg(name, modeText));
    m_title->setText(tr("%1  —  %2").arg(m_repo->relativePath(m_path), modeText));
    m_action->setText(mode == GitDiffMode::Staged ? tr("Unstage File") : tr("Stage File"));
    m_oldLabel->setText(mode == GitDiffMode::Unstaged ? tr("INDEX") : tr("HEAD"));
    m_newLabel->setText(mode == GitDiffMode::Staged ? tr("INDEX (STAGED)") : tr("WORKING TREE"));
    applyTheme();
    load();
}

void DiffDialog::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_left->setColors(t, false);
    m_right->setColors(t, true);
}

void DiffDialog::load()
{
    const QString rel = m_repo->relativePath(m_path);
    const GitFileChange *c = m_repo->changeFor(m_path);
    const QString headRel = c && !c->origRelPath.isEmpty() ? c->origRelPath : rel;
    const bool conflict = c && c->conflicted;

    QString oldSpec, newSpec;
    switch (m_mode) {
    case GitDiffMode::Unstaged: oldSpec = conflict ? QStringLiteral("HEAD:") + rel : QStringLiteral(":") + rel; break;
    case GitDiffMode::Staged:   oldSpec = QStringLiteral("HEAD:") + headRel; newSpec = QStringLiteral(":") + rel; break;
    case GitDiffMode::Head:     oldSpec = QStringLiteral("HEAD:") + headRel; break;
    }

    m_pending = 2;
    m_binary = false;
    auto done = [this] {
        if (--m_pending == 0)
            populate(m_oldText, m_newText);
    };
    auto decode = [this](const QByteArray &b, QString &out) {
        if (b.left(8000).contains('\0'))
            m_binary = true;
        out = QString::fromUtf8(b);
    };
    m_oldText.clear();
    m_newText.clear();
    m_repo->readBlob(oldSpec, this, [=](const GitRepository::Result &r) {
        if (r.ok())
            decode(r.out, m_oldText);
        done();
    });
    if (newSpec.isEmpty()) {
        QFile f(m_path);
        if (f.open(QIODevice::ReadOnly))
            decode(f.readAll(), m_newText);
        done();
    } else {
        m_repo->readBlob(newSpec, this, [=](const GitRepository::Result &r) {
            if (r.ok())
                decode(r.out, m_newText);
            done();
        });
    }
}

void DiffDialog::populate(const QString &oldText, const QString &newText)
{
    if (m_haveShown && oldText == m_shownOld && newText == m_shownNew)
        return;
    m_haveShown = true;
    m_shownOld = oldText;
    m_shownNew = newText;

    const int keep = m_left->verticalScrollBar()->value();
    m_hunkRows.clear();
    if (m_binary) {
        m_left->setRows({tr("Binary file")}, {}, {});
        m_right->setRows({tr("Binary file")}, {}, {});
        m_stats->setText(tr("Binary files cannot be compared."));
        m_prev->setEnabled(false);
        m_next->setEnabled(false);
        return;
    }

    // An absent file is an empty file, not a file with one empty line.
    // The empty "line" after a final newline is not a real line; don't show or number it.
    auto lines = [](const QString &text) {
        QStringList l = text.isEmpty() ? QStringList() : GitDiff::splitLines(text);
        if (!l.isEmpty() && l.last().isEmpty())
            l.removeLast();
        return l;
    };
    const QStringList a = lines(oldText);
    const QStringList b = lines(newText);
    const QVector<GitDiff::Hunk> hunks = GitDiff::compute(a, b);

    QStringList L, R;
    QVector<int> ln, rn;
    QVector<char> lk, rk;
    int i = 0, j = 0, added = 0, removed = 0;
    auto equal = [&](int count) {
        for (int k = 0; k < count; ++k, ++i, ++j) {
            L << a.at(i); ln << i + 1; lk << DiffView::Equal;
            R << b.at(j); rn << j + 1; rk << DiffView::Equal;
        }
    };
    for (const GitDiff::Hunk &h : hunks) {
        equal(h.oldStart - i);
        m_hunkRows << L.size();
        const int pairs = qMin(h.oldCount, h.newCount);
        for (int k = 0; k < pairs; ++k, ++i, ++j) {
            L << a.at(i); ln << i + 1; lk << DiffView::Changed;
            R << b.at(j); rn << j + 1; rk << DiffView::Changed;
        }
        for (int k = pairs; k < h.oldCount; ++k, ++i) {
            L << a.at(i); ln << i + 1; lk << DiffView::Changed;
            R << QString(); rn << 0; rk << DiffView::Filler;
        }
        for (int k = pairs; k < h.newCount; ++k, ++j) {
            L << QString(); ln << 0; lk << DiffView::Filler;
            R << b.at(j); rn << j + 1; rk << DiffView::Changed;
        }
        removed += h.oldCount;
        added += h.newCount;
    }
    equal(a.size() - i);
    m_left->setRows(L, ln, lk);
    m_right->setRows(R, rn, rk);
    m_left->verticalScrollBar()->setValue(keep);

    m_stats->setText(hunks.isEmpty() ? tr("No differences.") : tr("%n change(s):  +%1  −%2", nullptr, hunks.size()).arg(added).arg(removed));
    m_prev->setEnabled(!hunks.isEmpty());
    m_next->setEnabled(!hunks.isEmpty());
}

void DiffDialog::gotoChange(bool next)
{
    if (m_hunkRows.isEmpty())
        return;
    const int cur = m_right->textCursor().blockNumber();
    int target = -1;
    if (next) {
        for (int r : std::as_const(m_hunkRows))
            if (r > cur) {
                target = r;
                break;
            }
        if (target < 0)
            target = m_hunkRows.first();
    } else {
        for (int k = m_hunkRows.size() - 1; k >= 0; --k)
            if (m_hunkRows.at(k) < cur) {
                target = m_hunkRows.at(k);
                break;
            }
        if (target < 0)
            target = m_hunkRows.last();
    }
    m_left->goToRow(target);
    m_right->goToRow(target);
}
