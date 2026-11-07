#pragma once

#include <QList>
#include <QString>

// Everything QODE creates on this machine, in one list, so the user can see it (Help > QODE's Files). Transparency
// about what QODE leaves on a computer is a product goal: anything QODE writes outside AppPaths must be added here.
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

} // namespace QodeFiles
