#pragma once

#include <QFrame>
#include <QIcon>
#include <QList>
#include <QVariant>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;

// A centred, keyboard-driven "type to filter" popup shared by the command palette and Quick Open.
class PalettePopup : public QFrame
{
    Q_OBJECT
public:
    struct Item {
        QString title;
        QString detail; // category (commands) or directory (files)
        QString hint;   // right-aligned, e.g. a keyboard shortcut
        QIcon icon;
        QVariant data;
    };
    enum class Mode { Commands, Paths };

    explicit PalettePopup(QWidget *window);

    void setMode(Mode mode) { m_mode = mode; }
    // Quick Open style: a trailing ":42" (or ":42:7") is not part of the filter.
    void setLineSuffixEnabled(bool on) { m_lineSuffix = on; }
    void setPlaceholder(const QString &text);
    void setEmptyText(const QString &text) { m_emptyText = text; }
    void setItems(const QList<Item> &items); // keeps the current query
    void setStatus(const QString &text);     // small muted line under the list ("Indexing…")
    QString query() const;
    void setQuery(const QString &text);
    void popup(); // shows centred near the top of the parent window

signals:
    void accepted(const QVariant &data, const QString &query);
    // Emitted on every edit so owners can react (e.g. Quick Open's "file:line").
    void queryChanged(const QString &query);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void refilter();
    void activate(QListWidgetItem *item);
    void moveSelection(int delta);
    void updateHeight();

    Mode m_mode = Mode::Commands;
    bool m_lineSuffix = false;
    QLineEdit *m_edit;
    QListWidget *m_list;
    QLabel *m_status;
    QList<Item> m_items;
    QString m_emptyText;
    QWidget *m_window;
};
