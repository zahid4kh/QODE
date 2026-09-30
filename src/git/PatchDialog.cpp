#include "PatchDialog.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QPlainTextEdit>
#include <QSyntaxHighlighter>
#include <QVBoxLayout>

namespace {

class PatchHighlighter : public QSyntaxHighlighter
{
public:
    PatchHighlighter(QTextDocument *doc, const Theme &t)
        : QSyntaxHighlighter(doc)
        , m_t(t)
    {
    }

protected:
    void highlightBlock(const QString &line) override
    {
        QTextCharFormat f;
        if (line.startsWith(QLatin1String("+++")) || line.startsWith(QLatin1String("---")) || line.startsWith(QLatin1String("diff --git")) ||
            line.startsWith(QLatin1String("index "))) {
            f.setForeground(m_t.textMuted);
            f.setFontWeight(QFont::Bold);
        } else if (line.startsWith(QLatin1Char('+'))) {
            f.setBackground(m_t.diffAddBg);
        } else if (line.startsWith(QLatin1Char('-'))) {
            f.setBackground(m_t.diffDelBg);
        } else if (line.startsWith(QLatin1String("@@"))) {
            f.setForeground(m_t.accent);
        } else if (line.startsWith(QLatin1String("commit "))) {
            f.setForeground(m_t.gitModified);
            f.setFontWeight(QFont::Bold);
        }
        if (f != QTextCharFormat())
            setFormat(0, line.size(), f);
    }

private:
    Theme m_t;
};

} // namespace

PatchDialog::PatchDialog(const QString &title, const QString &text, QWidget *parent)
    : QDialog(parent, Qt::Window)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(title);
    resize(980, 700);
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    auto *view = new QPlainTextEdit(this);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(SettingsManager::instance().editorFont());
    view->setFrameShape(QFrame::NoFrame);
    QPalette p = view->palette();
    p.setColor(QPalette::Base, t.editorBg);
    p.setColor(QPalette::Text, t.editorFg);
    view->setPalette(p);
    new PatchHighlighter(view->document(), t);
    view->setPlainText(text);
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(view);
}
