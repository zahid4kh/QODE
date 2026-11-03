// MainWindow's inline edit box: Ctrl+K, "/?" typed in the text, and "Ask about this to make a change" at the top of the
// editor's context menu while text is selected. The box itself is ai/InlineEdit.
#include "MainWindow.h"

#include "ai/InlineEdit.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "project/ProjectManager.h"
#include "settings/Icons.h"

#include <QAction>
#include <QFileInfo>
#include <QStatusBar>
#include <QTextBlock>

void MainWindow::createInlineActions()
{
    m_inlineEditAct = new QAction(tr("Inline Edit…"), this);
    m_inlineEditAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    m_inlineEditAct->setToolTip(tr("Ask QODE to change the selected lines (or the caret line), right in the editor"));
    Icons::bind(m_inlineEditAct, QStringLiteral(":/new-icons/sparkles.svg"));
    connect(m_inlineEditAct, &QAction::triggered, this, [this] { openInlineEdit(m_editors->currentDocument(), m_editors->currentEditor(), -1); });

    // The context menu shows Ctrl+K through the tab column; two actions with one shortcut would be ambiguous.
    m_askChangeAct = new QAction(tr("Ask about this to make a change") + QStringLiteral("\tCtrl+K"), this);
    Icons::bind(m_askChangeAct, QStringLiteral(":/new-icons/sparkles.svg"));
    connect(m_askChangeAct, &QAction::triggered, this, [this] { openInlineEdit(m_editors->currentDocument(), m_editors->currentEditor(), -1); });
}

void MainWindow::wireInline(Document *doc, CodeEditor *ed)
{
    ed->setSelectionContextActions({m_askChangeAct});
    QPointer<CodeEditor> guard(ed);
    QPointer<Document> docGuard(doc);
    connect(ed, &CodeEditor::inlineAskRequested, this, [this, guard, docGuard](int position) {
        if (guard && docGuard)
            openInlineEdit(docGuard, guard, position);
    });
}

void MainWindow::openInlineEdit(Document *doc, CodeEditor *ed, int triggerPosition)
{
    if (!ed || !doc) {
        statusBar()->showMessage(tr("Open a file to edit it inline"), 4000);
        return;
    }
    QTextCursor c = ed->textCursor();
    int first = c.blockNumber(), last = first;
    if (c.hasSelection()) {
        QTextDocument *td = ed->document();
        const QTextBlock a = td->findBlock(c.selectionStart());
        const QTextBlock b = td->findBlock(c.selectionEnd());
        first = a.blockNumber();
        last = b.blockNumber();
        if (last > first && c.selectionEnd() == b.position()) // a selection of whole lines ends at the next line's start
            --last;
    }
    const QString root = m_projects->hasProject() ? m_projects->root() : QFileInfo(doc->filePath()).absolutePath();
    bool created = false;
    InlineEdit *box = InlineEdit::open(ed, doc, root, first, last, triggerPosition, &created);
    if (box && created)
        connect(box, &InlineEdit::statusMessage, this, [this](const QString &text) { statusBar()->showMessage(text, 5000); });
}
