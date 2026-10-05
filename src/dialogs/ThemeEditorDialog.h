#pragma once

#include <QDialog>
#include <QHash>
#include <QList>

#include "settings/Theme.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class QWidget;

// Edits a theme's colours. The window behind it follows every change (ThemeManager preview); Save writes the
// theme to <config>/themes/<id>.json and makes it the active theme. Built-in themes are saved as a copy.
class ThemeEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit ThemeEditorDialog(QWidget *parent = nullptr);

protected:
    void reject() override;

private:
    struct Row {
        int field = 0;
        QWidget *widget = nullptr;
        QPushButton *swatch = nullptr;
        QLineEdit *hex = nullptr;
        QLabel *label = nullptr;
    };

    void rebuildThemeList(const QString &select);
    void buildRows();
    void load(const QString &id);
    void setColor(int row, const QColor &c);
    void syncRow(int row);
    void touched();
    void applyFilter(const QString &text);
    void save(bool asNew);
    void removeCurrent();
    bool confirmDiscard();
    void finish();
    void updateButtons();

    QComboBox *m_themes;
    QComboBox *m_base;
    QLineEdit *m_filter;
    QWidget *m_rowHost;
    QPushButton *m_save, *m_saveAs, *m_delete, *m_revert;
    QLabel *m_status;
    QList<Row> m_rows;
    QList<QWidget *> m_headers;
    QHash<QWidget *, QString> m_headerGroup;

    Theme m_working;
    QString m_id;
    bool m_builtin = true;
    bool m_dirty = false;
    bool m_loading = false;
};
