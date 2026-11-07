#pragma once

#include "LocalHistory.h"

#include <QDialog>
#include <functional>

class DiffPane;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;

// The local history of a file, or of every file under a folder (deleted ones included): the stored versions, newest
// first, compared with the current text or with the version before; Restore puts the selected version back.
class LocalHistoryDialog : public QDialog
{
    Q_OBJECT
public:
    struct Hooks {
        // The file's text now (the open editor's, else the disk's); *exists false when the file is gone.
        std::function<QString(const QString &path, bool *exists)> currentText;
        // Keeps the current text as a "Before restore" version, then puts the version back: `text` into the editor when
        // the file is open, else `bytes` (the version as stored, in its own encoding) onto the disk. False on failure.
        std::function<bool(const QString &path, const QString &text, const QByteArray &bytes)> restore;
    };
    LocalHistoryDialog(const QString &path, const Hooks &hooks, QWidget *parent = nullptr);

private:
    void loadFiles();
    void showFile(const QString &path);
    void showVersion();
    void restoreSelected();

    QString m_root;   // the folder, or the file itself
    bool m_folder = false;
    Hooks m_hooks;
    QString m_file;   // the file shown
    QVector<LocalHistory::Version> m_versions;
    QLineEdit *m_filter = nullptr;
    QListWidget *m_files = nullptr;
    QListWidget *m_list;
    QLabel *m_title, *m_stats;
    QComboBox *m_compare;
    QPushButton *m_prev, *m_next, *m_restore;
    DiffPane *m_pane;
};
