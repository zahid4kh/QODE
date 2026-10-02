#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLabel;
class QTableWidget;

// Edits the .env files next to the server's package.json as a KEY / VALUE table. Saving rewrites only the lines that
// changed: comments, blank lines, `export` prefixes and untouched entries stay exactly as they were.
class EnvDialog : public QDialog
{
    Q_OBJECT
public:
    // `serverActive` offers to restart the running server after saving (most frameworks read .env only at start).
    EnvDialog(const QString &dir, bool serverActive, QWidget *parent = nullptr);

signals:
    void openFileRequested(const QString &path);
    void saved(bool restartServer);

private:
    struct Entry
    {
        QString key, value;
    };
    void loadFile(const QString &name);
    bool save();
    bool confirmDiscard();
    void addRow(const QString &key = {}, const QString &value = {});
    QString currentName() const;
    QString pathOf(const QString &name) const;

    QString m_dir;
    QComboBox *m_files;
    QTableWidget *m_table;
    QCheckBox *m_restart;
    QLabel *m_note;
    QString m_loaded;       // file name shown in the table
    QStringList m_rawLines; // the file as read, for in-place rewriting
    QList<Entry> m_original;
    bool m_dirty = false;
    bool m_loading = false;
};
