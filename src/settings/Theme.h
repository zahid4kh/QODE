#pragma once

#include <QColor>
#include <QString>

#include "git/GitTypes.h"

// Colour palette shared by the widget stylesheet, editor and highlighter.
struct Theme {
    bool dark = true;
    QColor window, panel, editorBg, editorFg, gutterBg, gutterFg, gutterActiveFg;
    QColor currentLine, selection, border, accent, textMuted;
    QColor keyword, type, string, comment, number, preprocessor, function, tag, attribute;
    QColor termBg, termFg;
    QColor gitAdded, gitModified, gitDeleted, gitUntracked, gitRenamed, gitConflict, gitIgnored;
    QColor diffAddBg, diffDelBg, diffFillBg;

    static Theme dark_();
    static Theme light_();
    static Theme byName(const QString &name);

    QColor gitColor(GitKind kind) const;
    QString styleSheet() const;
};
