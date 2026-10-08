#pragma once

#include "settings/Theme.h"

#include <QPersistentModelIndex>
#include <QPoint>
#include <QStringList>
#include <QTreeView>

class QTimer;

// The explorer's tree: besides the usual browsing it lets files and folders be dragged onto a folder. While dragging
// it highlights where the items would land, explains in a small pill next to the cursor what will happen (or why a
// drop is not possible), opens folders you hover over and scrolls near the edges. The move itself is requested through
// moveRequested(); this widget never touches the file system.
class ExplorerTree : public QTreeView
{
    Q_OBJECT
public:
    explicit ExplorerTree(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root) { m_root = root; }

    // What a drop at a position would do (also used by tests).
    struct Verdict {
        enum Kind { None, Ok, SameFolder, IntoItself, NameTaken, ProjectRoot } kind = None;
        QString targetDir;
        QString text;      // the pill's headline
        QString name;      // the folder shown in bold inside the text
        QStringList items; // the paths that would move
        bool copy = false; // files dragged in from another application: copied, not moved
    };
    Verdict verdictAt(const QPoint &viewportPos) const;
    QStringList draggedPaths() const { return m_dragPaths; }
    QStringList selectedPaths() const { return topLevelSelection(); }
    // Shows the drop feedback for `paths` hovering at `viewportPos` without a real drag (screenshots, tests); empty = off.
    void previewDrag(const QStringList &paths, const QPoint &viewportPos);

public slots:
    // Reached through Edit > Copy / Paste (Ctrl+C / Ctrl+V) while the tree has focus.
    void copy() { emit copyShortcut(); }
    void paste() { emit pasteShortcut(); }

signals:
    void moveRequested(const QStringList &sources, const QString &targetDir);
    void copyRequested(const QStringList &sources, const QString &targetDir); // files dropped in from outside
    void copyShortcut();
    void pasteShortcut();

protected:
    void startDrag(Qt::DropActions supported) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dragLeaveEvent(QDragLeaveEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void paintEvent(QPaintEvent *e) override;

private:
    QStringList topLevelSelection() const; // selected paths without those inside another selected folder
    QString pathOf(const QModelIndex &idx) const;
    QModelIndex lastVisibleDescendant(const QModelIndex &idx) const;
    QPixmap dragPixmap(const QStringList &paths) const;
    void endDrag();
    void autoScrollStep();
    void paintHighlight(QPainter &p) const;
    void paintHint(QPainter &p) const;
    QString folderLabel(const QString &dir) const;

    QString m_root;
    QStringList m_dragPaths;
    bool m_dragging = false;
    bool m_external = false; // the drag comes from another application
    Verdict m_verdict;
    QPoint m_cursor;
    QPersistentModelIndex m_hoverDir; // folder under the cursor, for spring-loading
    QTimer *m_spring;
    QTimer *m_scroll;
    Theme m_theme;
};
