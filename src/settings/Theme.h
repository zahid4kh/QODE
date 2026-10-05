#pragma once

#include <QColor>
#include <QJsonObject>
#include <QList>
#include <QString>

#include "git/GitTypes.h"

// Colour palette shared by the widget stylesheet, editor and highlighter.
struct Theme {
    QString name;      // display name
    bool dark = true;  // base kind: picks icon variants and a few contrast tweaks
    QColor frame, window, panel, editorBg, editorFg, gutterBg, gutterFg, gutterActiveFg;
    QColor currentLine, selection, border, accent, textMuted;
    QColor keyword, type, string, comment, number, preprocessor, function, tag, attribute;
    QColor termBg, termFg;
    QColor gitAdded, gitModified, gitDeleted, gitUntracked, gitRenamed, gitConflict, gitIgnored;
    QColor diffAddBg, diffDelBg, diffFillBg;
    QColor success, warning, danger, idle; // status dots, errors
    QColor findMatchBg;
    QColor ansi[16];                       // terminal palette

    // One editable colour: the JSON key, the label and section shown in the theme editor, and its member.
    struct Field {
        const char *key;
        const char *label;
        const char *group;
        QColor &(*ref)(Theme &);
    };
    static const QList<Field> &fields();

    QJsonObject toJson() const;
    // Colours missing from the object come from the base theme ("dark" unless it says "light").
    static Theme fromJson(const QJsonObject &obj);

    static Theme dark_();
    static Theme light_();
    static Theme darcula_();
    static Theme byName(const QString &name);

    QColor gitColor(GitKind kind) const;
    QColor onAccent() const { return dark ? window : QColor(Qt::white); } // text on accent-filled buttons
    QString styleSheet() const;
};
