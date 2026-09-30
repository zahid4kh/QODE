#pragma once

#include <QColor>
#include <QString>

// Colour palette shared by the widget stylesheet, editor and highlighter.
struct Theme {
    bool dark = true;
    QColor window, panel, editorBg, editorFg, gutterBg, gutterFg, gutterActiveFg;
    QColor currentLine, selection, border, accent, textMuted;
    QColor keyword, type, string, comment, number, preprocessor, function, tag, attribute;
    QColor termBg, termFg;

    static Theme dark_();
    static Theme light_();
    static Theme byName(const QString &name);

    QString styleSheet() const;
};
