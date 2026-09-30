#pragma once

#include <QHash>
#include <QWidget>

class CodeEditor;
class Document;
class FindBar;
class QFileSystemWatcher;
class QStackedWidget;
class QTabWidget;
class QTimer;

// Owns the open documents and their tabs; drives save/close/reload flows.
class EditorManager : public QWidget
{
    Q_OBJECT
public:
    explicit EditorManager(QWidget *parent = nullptr);

    bool openFile(const QString &path);
    // Opens `path` and puts the cursor on 1-based `line` (and `column` when > 0).
    bool openFileAt(const QString &path, int line, int column = 0);
    void gotoLine(int line, int column = 0);
    void newUntitled();

    Document *currentDocument() const;
    CodeEditor *currentEditor() const;
    CodeEditor *editorFor(Document *doc) const;
    QList<Document *> documents() const;
    QList<Document *> modifiedDocuments() const;
    QStringList openFilePaths() const;
    int count() const;

    bool saveCurrent();
    bool saveCurrentAs();
    bool saveAll();
    bool closeCurrent();
    bool closeDocument(Document *doc); // prompts if modified
    // Prompt once for every modified document, save/discard per the answer, then close everything.
    bool closeAll();
    // Ask about unsaved changes without closing anything.
    bool confirmDiscardOrSaveAll();

    void closeDocumentsUnder(const QString &path); // no prompt; file/folder was deleted
    void pathRenamed(const QString &oldPath, const QString &newPath);

    void nextTab();
    void previousTab();
    void showFind();
    void showReplace();
    void focusEditor();

signals:
    void currentChanged();       // active document/editor changed
    void documentStateChanged(); // modified flag / title changed
    void cursorInfoChanged();    // cursor position or overwrite mode
    void countChanged(int count);
    void documentAdded(Document *doc);       // a tab was created for this document
    void documentPathChanged(Document *doc); // saved as / renamed
    // Empty-state (welcome page) buttons
    void newProjectRequested();
    void openProjectRequested();
    void newFileRequested();
    void openFileRequested();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    struct Entry {
        Document *doc;
        CodeEditor *editor;
    };

    int indexOf(Document *doc) const;
    Entry entryAt(int index) const;
    Entry addDocument(Document *doc);
    void removeAt(int index);
    void updateTabTitle(Document *doc);
    void onTabCloseRequested(int index);
    void onCurrentTabChanged(int index);
    bool saveDocument(Document *doc);
    bool saveDocumentAs(Document *doc);
    bool maybeSave(Document *doc);
    void updateStack();

    void watch(const QString &path);
    void unwatch(const QString &path);
    void onWatchedFileChanged(const QString &path);
    void processPendingChanges();

    QStackedWidget *m_stack;
    QTabWidget *m_tabs;
    FindBar *m_find;
    QFileSystemWatcher *m_watcher;
    QTimer *m_changeTimer;
    QStringList m_pendingChanges;
    bool m_prompting = false;
    QHash<CodeEditor *, Document *> m_docForEditor;
};
