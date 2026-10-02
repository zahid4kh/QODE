#pragma once

#include <QHash>
#include <QPointer>
#include <QWidget>

class CodeEditor;
class Document;
class WelcomePage;
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
    // `length` > 0 selects that many characters from the position (used by search results).
    bool openFileAt(const QString &path, int line, int column = 0, int length = 0);
    void gotoLine(int line, int column = 0, int length = 0);
    Document *documentForPath(const QString &path) const; // nullptr when not open
    void newUntitled();

    Document *currentDocument() const;
    static bool isPreviewable(const Document *doc); // Markdown and SVG
    CodeEditor *currentEditor() const;
    CodeEditor *editorFor(Document *doc) const;
    QList<Document *> documents() const;
    QList<Document *> modifiedDocuments() const;
    QStringList openFilePaths() const;
    int count() const;

    bool saveCurrent();
    bool saveCurrentAs();
    bool saveAll();
    bool formatCurrent(); // Format Document: runs the installed formatter for the file type
    bool closeCurrent();
    bool closeDocument(Document *doc); // prompts if modified
    // Prompt once for every modified document, save/discard per the answer, then close everything.
    bool closeAll();
    // Ask about unsaved changes without closing anything.
    bool confirmDiscardOrSaveAll();

    void closeDocumentsUnder(const QString &path); // no prompt; file/folder was deleted
    void pathRenamed(const QString &oldPath, const QString &newPath);

    void setProjectRoot(const QString &root); // breadcrumbs show paths relative to it
    void setRecentProjects(const QStringList &paths); // listed on the welcome page

    void nextTab();
    void previousTab();
    void showFind();
    void showReplace();
    void focusEditor();

signals:
    void statusMessage(const QString &text); // auto save / format results for the status bar
    void currentChanged();       // active document/editor changed
    void documentStateChanged(); // modified flag / title changed
    void cursorInfoChanged();    // cursor position or overwrite mode
    void countChanged(int count);
    void documentAdded(Document *doc);       // a tab was created for this document
    void documentPathChanged(Document *doc); // saved as / renamed
    void previewRequested();                       // the Preview button of the active Markdown / SVG file
    void blameCommitRequested(const QString &hash); // gutter blame column clicked
    void documentSaved(Document *doc);       // written to disk (manual or auto save)
    // Empty-state (welcome page) buttons
    void newProjectRequested();
    void openProjectRequested();
    void newFileRequested();
    void openFileRequested();
    void mediaRequested(const QString &path); // image/video: shown in the media panel, not a tab
    void openRecentProjectRequested(const QString &path);
    void removeRecentProjectRequested(const QString &path);
    void clearRecentProjectsRequested();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void syncFileClaims();
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
    // Format / trim / final newline as configured, as one undo step. `automatic` = triggered by auto save.
    void prepareForSave(Document *doc, bool automatic, const QString &path);
    bool autoSaveDocument(Document *doc);
    void autoSaveAll();
    void applySaveSettings();
    void updateStack();
    void updateCrumbs(Document *doc);
    void updateAllCrumbs();

    void watch(const QString &path);
    void unwatch(const QString &path);
    void onWatchedFileChanged(const QString &path);
    void processPendingChanges();

    WelcomePage *m_welcome;
    QStackedWidget *m_stack;
    QTabWidget *m_tabs;
    FindBar *m_find;
    QFileSystemWatcher *m_watcher;
    QTimer *m_changeTimer;
    QStringList m_pendingChanges;
    bool m_prompting = false;
    QString m_projectRoot;
    QTimer *m_autoSaveTimer;
    QPointer<Document> m_lastDoc; // for "save on focus change"
    QHash<CodeEditor *, Document *> m_docForEditor;
};
