#pragma once

#include <QList>
#include <QString>

// Everything QODE creates on this machine, in one list, so the user can see it (Help > QODE's Files) and the Windows
// uninstaller removes exactly that (runIfHelper). Transparency about what QODE leaves on a computer is a product goal:
// anything QODE writes outside AppPaths must be added here.
namespace QodeFiles {

struct Item {
    QString id;            // stable key the uninstaller passes back: config, data, cache, kotlin-link, jdk-1, …
    QString path;          // '/' separators
    QString what;          // one line: what is in it
    bool folder = true;
    bool removeByDefault = false; // the uninstaller ticks it (downloaded JDKs: unticked, projects may use them)
    qint64 size = -1;      // bytes when computed, else -1
};

// What QODE made while it ran (only what exists now): its config / data / cache folders, the Kotlin language server's
// command link, the JDKs it downloaded. With `sizes`, folders are walked (seconds for gigabytes of language servers:
// not on the GUI thread).
QList<Item> created(bool sizes);

// Bytes in a file or folder; links and junctions are counted as themselves, never followed.
qint64 sizeOf(const QString &path);

// Deletes one item of created() (Platform::removeTree for folders: never follows a junction or symlink).
bool remove(const Item &item, QString *error);

// The Windows uninstaller's helper, run before QODE's own files go (main calls this first):
//   qode --qode-files list <report>              writes created() with sizes
//   qode --qode-files remove <report> <id>...    deletes those items, looked up by id in created() (never a path
//                                                from the command line), and writes one result line per id
// The report is UTF-8 with a BOM (Inno Setup reads it), one tab-separated line per item:
//   list:   id, remove by default (1/0), bytes, path (native), what
//   remove: removed|failed|missing, id, path, bytes or error
// True when this process was started for that; main then returns at once.
bool runIfHelper(int argc, char *argv[]);

} // namespace QodeFiles
