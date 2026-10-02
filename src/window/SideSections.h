#pragma once

#include <QList>
#include <QWidget>

class QSplitter;
class QToolButton;

// The left panel: a vertical stack of collapsible sections (Explorer, Source Control, ...). Clicking a header
// expands or collapses its section; the expanded ones share the height.
class SideSections : public QWidget
{
    Q_OBJECT
public:
    explicit SideSections(QWidget *parent = nullptr);

    int addSection(const QString &title, QWidget *body);
    void setTitle(int index, const QString &title);
    bool isExpanded(int index) const;
    void setExpanded(int index, bool expanded);
    QList<bool> expandedStates() const;
    void setExpandedStates(const QList<bool> &states);

signals:
    void expansionChanged();

private:
    struct Section {
        QWidget *frame;
        QToolButton *header;
        QWidget *body;
        QString title;
        bool expanded = true;
        int height = 0; // size while expanded, restored on expand
    };
    void refresh(int index);
    void refreshIcons();

    QSplitter *m_split;
    QList<Section> m_sections;
};
