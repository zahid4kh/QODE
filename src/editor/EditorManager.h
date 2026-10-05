#pragma once

#include <functional>

#include "EditorGroup.h"

#include <QHash>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>

class CodeEditor;
class Document;
class WelcomePage;
class FindBar;
class QFileSystemWatcher;
class QSplitter;
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
    // Writes a document that a refactoring changed: no formatting, trimming or other save actions.
    bool saveQuietly(Document *doc);
    // Fallback when no formatter program exists for a file: asks the language server (set by MainWindow). Returns true when
    // the server formatted the document (edits applied, possibly none).
    void setLspFormatter(std::function<bool(Document *, CodeEditor *)> f) { m_lspFormatter = std::move(f); }
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

    // Split editor: the current file moves into a new group beside / below its own.
    void splitCurrent(Qt::Orientation orientation);
    int groupCount() const;
    QJsonObject layoutState() const;              // groups, their tabs and splitter sizes
    void restoreLayout(const QJsonObject &state); // after the files are open

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
    struct Loc {
        EditorGroup *group = nullptr;
        int index = -1;
    };

    QList<EditorGroup *> groups() const; // in on-screen order
    Loc locate(Document *doc) const;
    Entry entryIn(EditorGroup *group, int index) const;
    EditorGroup *createGroup();
    void setActiveGroup(EditorGroup *group);
    void activate(Document *doc);
    void moveDocument(Document *doc, EditorGroup *target, EditorGroup::Zone zone);
    void removeGroupIfEmpty(EditorGroup *group);
    void normalizeRoot();
    void startTabDrag(EditorGroup *group, int index);
    void showTabMenu(EditorGroup *group, int index, const QPoint &globalPos);
    QWidget *buildLayout(const QJsonObject &node, QHash<QString, Document *> &docs);
    Entry addDocument(Document *doc);
    void removeDocument(Document *doc);
    void updateTabTitle(Document *doc);
    void onActiveTabChanged();
    void updateGroupMarkers(); // accent bar on the active group, dimming on the rest
    bool saveDocument(Document *doc);
    bool saveDocumentAs(Document *doc);
    bool maybeSave(Document *doc);
    // Format / trim / final newline as configured, as one undo step. `automatic` = triggered by auto save.
    std::function<bool(Document *, CodeEditor *)> m_lspFormatter;
    bool formatWithLsp(Document *doc);
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
    QSplitter *m_root; // tree of splitters; the leaves are EditorGroups
    EditorGroup *m_active;
    QPointer<Document> m_dragDoc;
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
