#pragma once

#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QStringList>
#include <QTextLayout>
#include <QVector>

class QTextCursor;
class QTextDocument;
struct LanguageDefinition;

// "Copy as Image": code drawn into a PNG the way code-sharing images look - the code on a rounded card in an editor
// theme's colours, optional window buttons, file name and line numbers, on a padded background (gradient, colour,
// picture or none) with a soft shadow and a small QODE mark. Painted with QPainter at 1-4x, nothing is captured
// from the screen.
namespace CodeImage {

// What the picture looks like; kept between uses (SettingsManager::codeImage).
struct Style {
    enum Background { Preset, Solid, Gradient, Picture, Transparent };
    Background background = Preset;
    int preset = 0;                  // index into presets()
    QColor color1, color2;           // Solid uses color1; Gradient both
    int angle = 135;                 // Gradient direction in degrees (0 = left to right, 90 = top to bottom)
    QString picture;                 // Picture: an image file, scaled to cover
    int padding = 56;                // around the card, logical px
    int radius = 12;                 // the card's corners
    int fontSize = 15;               // logical px
    int scale = 2;                   // output pixels per logical px
    QString theme;                   // a theme id; "" = the current one
    bool shadow = true, windowButtons = true, title = true, lineNumbers = false, watermark = true;

    QJsonObject toJson() const;
    static Style fromJson(const QJsonObject &o);
    static Style load();
    void save() const;
};

// The gradients offered under Background.
struct Preset {
    const char *name;
    QColor from, to;
};
const QVector<Preset> &presets();

// The code to draw: its lines (indentation common to all of them removed), the first one's line number, and the
// colours. `formats` are copied from an editor's highlighting (semantic colours included) and only used while the
// picture uses `formatsTheme`; otherwise the lines are coloured again with `language` in the chosen theme.
struct Source {
    QStringList lines;
    QVector<QVector<QTextLayout::FormatRange>> formats;
    QString formatsTheme;
    const LanguageDefinition *language = nullptr;
    int firstLine = 1;
    QString title;                   // shown in the title bar (a file name)
    bool truncated = false;          // more than kMaxLines: the rest was left out
};
constexpr int kMaxLines = 400;

// The selection of an editor document (whole lines when it spans several, else just the selected text), or the whole
// document when nothing is selected.
Source fromDocument(QTextDocument *doc, const QTextCursor &selection, const LanguageDefinition *language, const QString &title);
// A code snippet (a chat code box): `language` is a fence id or extension, "" = plain.
Source fromCode(const QString &code, const QString &language, const QString &title = QString());

// The picture at `scaleOverride` (> 0) instead of style.scale (the dialog's preview).
QImage render(const Source &source, const Style &style, qreal scaleOverride = 0);
// The size render() gives at style.scale, without drawing.
QSize measure(const Source &source, const Style &style);

} // namespace CodeImage
