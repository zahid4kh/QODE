#pragma once

#include <QWidget>

class QTabWidget;

// One set of editor tabs. The editor area is a tree of splitters whose leaves are groups, so files can sit
// side by side or stacked. Tabs are dragged between groups (or onto a group's edge to split it) with a QDrag
// that carries no data of its own: EditorManager knows which document is being dragged.
class EditorGroup : public QWidget
{
    Q_OBJECT
public:
    enum Zone { Center, Left, Right, Top, Bottom };

    explicit EditorGroup(QWidget *parent = nullptr);

    QTabWidget *tabs() const { return m_tabs; }
    // While a tab is being dragged an overlay covers the group, showing where the tab would land.
    void setDragActive(bool active);

    static QString mimeType();

signals:
    void tabDragStarted(int index);
    void tabContextMenuRequested(int index, const QPoint &globalPos);
    void tabDropped(EditorGroup::Zone zone);

private:
    class Overlay;
    QTabWidget *m_tabs;
    Overlay *m_overlay;
};
