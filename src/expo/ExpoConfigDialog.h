#pragma once

#include "ExpoProject.h"
#include "JsonDoc.h"

#include <QDialog>
#include <QJsonObject>

class ExpoContainerNode;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QStackedWidget;

// Form editor for an Expo app.json: the top-level keys become sections (General for the plain values, one page per
// object / array such as ios, android, plugins), objects and arrays nest as cards, and every value has an editor for its
// type (text, number, switch, a list of choices when the Expo schema has an enum). Properties can be added, renamed,
// retyped and removed. Key order and number formatting of the file are kept; Save rewrites only app.json.
// The Expo schema (when cached) supplies tooltips, property-name completion and choices.
class ExpoConfigDialog : public QDialog
{
    Q_OBJECT
public:
    ExpoConfigDialog(const ExpoApp &app, const QJsonObject &schema, QWidget *parent = nullptr);

signals:
    void saved(const QString &path);
    void openInEditorRequested(const QString &path);

protected:
    void reject() override;

private:
    struct Section
    {
        QString key; // "" = General
        QListWidgetItem *item = nullptr;
        QWidget *page = nullptr;
        ExpoContainerNode *node = nullptr;
    };
    void applyTheme();
    bool load();
    void showError(const QString &message);
    Section &addSection(const QString &key, const JsonValue &value);
    void addNavTail();
    void newSection();
    void deleteSection(const QString &key);
    JsonValue collect() const;
    void updateDirty();
    void save();
    QString indentUnit() const;

    ExpoApp m_app;
    QJsonObject m_schema;
    JsonValue m_root;
    bool m_bare = false; // no "expo" key: the file itself is the configuration
    QString m_original, m_originalText;
    QStringList m_originalOrder;
    QList<Section> m_sections;
    QListWidget *m_nav;
    QStackedWidget *m_pages;
    QLabel *m_dirty;
    QPushButton *m_save;
    QPushButton *m_cancel;
};
