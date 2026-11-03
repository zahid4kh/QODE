#include "CodeImage.h"

#include "editor/HoverRender.h"
#include "editor/Language.h"
#include "editor/SyntaxHighlighter.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QFontMetricsF>
#include <QIcon>
#include <QJsonDocument>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QtMath>

#include <algorithm>
#include <memory>
#include <vector>

namespace CodeImage {

const QVector<Preset> &presets()
{
    // Picture backgrounds, not interface colours: they are the same in every theme.
    static const QVector<Preset> list = {
        {"Violet", QColor(0xf4, 0x72, 0xb6), QColor(0x8b, 0x5c, 0xf6)},
        {"Ocean", QColor(0x0e, 0xa5, 0xe9), QColor(0x63, 0x66, 0xf1)},
        {"Sunset", QColor(0xf9, 0x73, 0x16), QColor(0xdb, 0x27, 0x77)},
        {"Mint", QColor(0x34, 0xd3, 0x99), QColor(0x0e, 0xa5, 0xe9)},
        {"Peach", QColor(0xfd, 0xba, 0x74), QColor(0xfd, 0xa4, 0xaf)},
        {"Midnight", QColor(0x1e, 0x29, 0x3b), QColor(0x4c, 0x1d, 0x95)},
        {"Forest", QColor(0x14, 0x53, 0x2d), QColor(0x65, 0xa3, 0x0d)},
        {"Graphite", QColor(0x52, 0x52, 0x5b), QColor(0x18, 0x18, 0x1b)},
    };
    return list;
}

// --- Style ----------------------------------------------------------------------------------------------------------

QJsonObject Style::toJson() const
{
    static const char *kinds[] = {"preset", "solid", "gradient", "picture", "transparent"};
    return QJsonObject{
        {QStringLiteral("background"), QLatin1String(kinds[background])},
        {QStringLiteral("preset"), preset},
        {QStringLiteral("color1"), color1.isValid() ? color1.name() : QString()},
        {QStringLiteral("color2"), color2.isValid() ? color2.name() : QString()},
        {QStringLiteral("angle"), angle},
        {QStringLiteral("picture"), picture},
        {QStringLiteral("padding"), padding},
        {QStringLiteral("radius"), radius},
        {QStringLiteral("fontSize"), fontSize},
        {QStringLiteral("scale"), scale},
        {QStringLiteral("theme"), theme},
        {QStringLiteral("shadow"), shadow},
        {QStringLiteral("windowButtons"), windowButtons},
        {QStringLiteral("title"), title},
        {QStringLiteral("lineNumbers"), lineNumbers},
        {QStringLiteral("watermark"), watermark},
    };
}

Style Style::fromJson(const QJsonObject &o)
{
    Style s;
    const QString kind = o.value(QStringLiteral("background")).toString();
    if (kind == QLatin1String("solid"))
        s.background = Solid;
    else if (kind == QLatin1String("gradient"))
        s.background = Gradient;
    else if (kind == QLatin1String("picture"))
        s.background = Picture;
    else if (kind == QLatin1String("transparent"))
        s.background = Transparent;
    s.preset = qBound(0, o.value(QStringLiteral("preset")).toInt(0), int(presets().size()) - 1);
    s.color1 = QColor(o.value(QStringLiteral("color1")).toString());
    s.color2 = QColor(o.value(QStringLiteral("color2")).toString());
    if (!s.color1.isValid())
        s.color1 = presets().at(s.preset).from;
    if (!s.color2.isValid())
        s.color2 = presets().at(s.preset).to;
    s.angle = o.value(QStringLiteral("angle")).toInt(s.angle) % 360;
    s.picture = o.value(QStringLiteral("picture")).toString();
    s.padding = qBound(0, o.value(QStringLiteral("padding")).toInt(s.padding), 160);
    s.radius = qBound(0, o.value(QStringLiteral("radius")).toInt(s.radius), 40);
    s.fontSize = qBound(8, o.value(QStringLiteral("fontSize")).toInt(s.fontSize), 40);
    s.scale = qBound(1, o.value(QStringLiteral("scale")).toInt(s.scale), 4);
    s.theme = o.value(QStringLiteral("theme")).toString();
    s.shadow = o.value(QStringLiteral("shadow")).toBool(s.shadow);
    s.windowButtons = o.value(QStringLiteral("windowButtons")).toBool(s.windowButtons);
    s.title = o.value(QStringLiteral("title")).toBool(s.title);
    s.lineNumbers = o.value(QStringLiteral("lineNumbers")).toBool(s.lineNumbers);
    s.watermark = o.value(QStringLiteral("watermark")).toBool(s.watermark);
    return s;
}

Style Style::load()
{
    return fromJson(SettingsManager::instance().codeImage());
}

void Style::save() const
{
    SettingsManager::instance().setCodeImage(toJson());
}

// --- Source ---------------------------------------------------------------------------------------------------------

namespace {

using Formats = QVector<QTextLayout::FormatRange>;

// Only what changes how a character looks in the picture: colour, weight, slant. Highlighter formats can also carry
// letter spacing (room for inlay hints) and private properties.
Formats keepLook(const QList<QTextLayout::FormatRange> &in, int from, int to)
{
    Formats out;
    for (const QTextLayout::FormatRange &r : in) {
        const int start = qMax(r.start, from), end = qMin(r.start + r.length, to);
        if (end <= start)
            continue;
        QTextCharFormat f;
        if (r.format.foreground().style() != Qt::NoBrush)
            f.setForeground(r.format.foreground());
        if (r.format.hasProperty(QTextFormat::FontWeight))
            f.setFontWeight(r.format.fontWeight());
        if (r.format.hasProperty(QTextFormat::FontItalic))
            f.setFontItalic(r.format.fontItalic());
        if (f.properties().isEmpty())
            continue;
        out.append({start - from, end - start, f});
    }
    return out;
}

// Blank lines at either end go, the indentation all lines share goes, and very long code is cut.
void tidy(Source &s)
{
    s.formats.resize(s.lines.size());
    while (!s.lines.isEmpty() && s.lines.first().trimmed().isEmpty()) {
        s.lines.removeFirst();
        s.formats.removeFirst();
        ++s.firstLine;
    }
    while (!s.lines.isEmpty() && s.lines.last().trimmed().isEmpty()) {
        s.lines.removeLast();
        s.formats.removeLast();
    }
    if (s.lines.size() > kMaxLines) {
        s.lines = s.lines.mid(0, kMaxLines);
        s.formats.resize(kMaxLines);
        s.truncated = true;
    }
    QString common;
    bool first = true;
    for (const QString &l : std::as_const(s.lines)) {
        if (l.trimmed().isEmpty())
            continue;
        int n = 0;
        while (n < l.size() && (l[n] == QLatin1Char(' ') || l[n] == QLatin1Char('\t')))
            ++n;
        if (first) {
            common = l.left(n);
            first = false;
        } else {
            int k = 0;
            while (k < common.size() && k < n && common[k] == l[k])
                ++k;
            common.truncate(k);
        }
    }
    const int cut = int(common.size());
    if (cut == 0)
        return;
    for (int i = 0; i < s.lines.size(); ++i) {
        QString &l = s.lines[i];
        const int n = qMin(cut, int(l.size())); // blank lines may be shorter
        l.remove(0, n);
        Formats shifted;
        for (QTextLayout::FormatRange r : std::as_const(s.formats[i])) {
            const int end = r.start + r.length - n;
            r.start = qMax(0, r.start - n);
            r.length = end - r.start;
            if (r.length > 0)
                shifted.append(r);
        }
        s.formats[i] = shifted;
    }
}

} // namespace

Source fromDocument(QTextDocument *doc, const QTextCursor &selection, const LanguageDefinition *language, const QString &title)
{
    Source s;
    s.language = language;
    s.title = title;
    s.formatsTheme = SettingsManager::instance().theme();
    QTextBlock a = doc->firstBlock(), b = doc->lastBlock();
    int fromCol = -1, toCol = -1;
    if (selection.hasSelection()) {
        a = doc->findBlock(selection.selectionStart());
        b = doc->findBlock(selection.selectionEnd());
        if (a == b) {
            fromCol = selection.selectionStart() - a.position();
            toCol = selection.selectionEnd() - a.position();
        } else if (selection.selectionEnd() == b.position()) {
            b = b.previous(); // the selection stops at the start of a line: that line is not part of it
        }
    }
    s.firstLine = a.blockNumber() + 1;
    for (QTextBlock it = a; it.isValid() && it.blockNumber() <= b.blockNumber(); it = it.next()) {
        const QString text = it.text();
        const int from = fromCol >= 0 ? fromCol : 0, to = toCol >= 0 ? toCol : int(text.size());
        s.lines << text.mid(from, to - from);
        s.formats << keepLook(it.layout()->formats(), from, to);
        if (s.lines.size() > kMaxLines) // tidy() cuts the rest; no need to copy a huge file first
            break;
    }
    tidy(s);
    return s;
}

Source fromCode(const QString &code, const QString &language, const QString &title)
{
    Source s;
    const QString ext = HoverRender::extensionFor(language);
    s.language = ext.isEmpty() ? nullptr : Languages::forFile(QStringLiteral("x.") + ext);
    s.title = title;
    QString text = code;
    text.remove(QLatin1Char('\r'));
    s.lines = text.split(QLatin1Char('\n'));
    tidy(s);
    return s;
}

// --- Rendering ------------------------------------------------------------------------------------------------------

namespace {

// The lines coloured by QODE's highlighter in `theme`.
QVector<Formats> colour(const QStringList &lines, const LanguageDefinition *language, const Theme &theme)
{
    QVector<Formats> out(lines.size());
    if (!language)
        return out;
    QTextDocument doc;
    auto *hl = new SyntaxHighlighter(&doc);
    hl->setTheme(theme);
    hl->setLanguage(language);
    doc.setPlainText(lines.join(QLatin1Char('\n')));
    hl->rehighlight();
    int i = 0;
    for (QTextBlock b = doc.begin(); b.isValid() && i < out.size(); b = b.next(), ++i)
        out[i] = keepLook(b.layout()->formats(), 0, int(b.text().size()));
    return out;
}

// A soft shadow: the alpha channel of a black premultiplied image blurred by three box passes (close to a Gaussian).
void blur(QImage &img, int radius)
{
    if (radius < 1)
        return;
    const int w = img.width(), h = img.height();
    std::vector<int> a(size_t(w) * h), tmp(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < w; ++x)
            a[size_t(y) * w + x] = qAlpha(line[x]);
    }
    const int r = qMax(1, radius / 2);
    auto pass = [&](std::vector<int> &src, std::vector<int> &dst, bool horizontal) {
        const int outer = horizontal ? h : w, len = horizontal ? w : h;
        const size_t step = horizontal ? 1 : size_t(w);
        const int span = 2 * r + 1;
        for (int o = 0; o < outer; ++o) {
            const size_t base = horizontal ? size_t(o) * w : size_t(o);
            auto at = [&](int i) { return src[base + size_t(qBound(0, i, len - 1)) * step]; };
            int sum = 0;
            for (int i = -r; i <= r; ++i)
                sum += at(i);
            for (int i = 0; i < len; ++i) {
                dst[base + size_t(i) * step] = sum / span;
                sum += at(i + r + 1) - at(i - r);
            }
        }
    };
    for (int k = 0; k < 3; ++k) {
        pass(a, tmp, true);
        pass(tmp, a, false);
    }
    for (int y = 0; y < h; ++y) {
        QRgb *line = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < w; ++x)
            line[x] = qRgba(0, 0, 0, a[size_t(y) * w + x]); // premultiplied black is just the alpha
    }
}

qreal luminance(const QColor &c)
{
    return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF();
}

} // namespace

namespace {

// render() and measure(): with `measured` set, only the size is worked out.
QImage draw(const Source &source, const Style &style, qreal k, QSize *measured)
{
    auto px = [k](qreal v) { return v * k; };
    const QString themeId = style.theme.isEmpty() ? SettingsManager::instance().theme() : style.theme;
    const Theme t = Theme::byName(themeId);
    const QVector<Formats> formats = (themeId == source.formatsTheme && source.formats.size() == source.lines.size())
                                         ? source.formats
                                         : colour(source.lines, source.language, t);

    QFont font = SettingsManager::instance().editorFont();
    font.setPixelSize(qMax(1, qRound(px(style.fontSize))));
    const QFontMetricsF fm(font);
    const qreal lineH = std::ceil(fm.height() * 1.35);
    const qreal charW = fm.horizontalAdvance(QLatin1Char('M'));
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    option.setTabStopDistance(charW * SettingsManager::instance().tabSize());

    std::vector<std::unique_ptr<QTextLayout>> layouts;
    qreal textW = 0;
    const QStringList lines = source.lines.isEmpty() ? QStringList{QString()} : source.lines;
    for (int i = 0; i < lines.size(); ++i) {
        auto l = std::make_unique<QTextLayout>(lines[i], font);
        l->setTextOption(option);
        l->setFormats(formats.value(i));
        l->beginLayout();
        QTextLine line = l->createLine();
        if (line.isValid()) {
            line.setLineWidth(1e7);
            line.setPosition(QPointF(0, 0));
            textW = qMax(textW, line.naturalTextWidth());
        }
        l->endLayout();
        layouts.push_back(std::move(l));
    }

    QFont ui = QApplication::font();
    ui.setPixelSize(qMax(1, qRound(px(style.fontSize * 0.87))));
    const QFontMetricsF uiFm(ui);
    const QString title = style.title ? source.title : QString();
    const bool bar = style.windowButtons || !title.isEmpty();
    const qreal gutterW = style.lineNumbers ? fm.horizontalAdvance(QString::number(source.firstLine + lines.size() - 1)) + charW * 2 : 0;
    const qreal padX = px(24), barH = bar ? px(40) : 0, padTop = bar ? px(6) : px(22), padBottom = px(22);
    const qreal pad = px(style.padding);
    // Too little room around the card for the mark: it gets a strip of its own inside the card.
    const bool markInside = style.watermark && style.padding < 32;
    const qreal footerH = markInside ? px(30) : 0;
    const qreal dotsW = style.windowButtons ? px(20 + 3 * 20) : 0;
    const qreal titleW = title.isEmpty() ? 0 : uiFm.horizontalAdvance(title) + 2 * dotsW + px(24);
    const qreal cardW = std::ceil(std::max({padX * 2 + gutterW + textW, px(280), titleW}));
    const qreal cardH = std::ceil(barH + padTop + lineH * lines.size() + padBottom + footerH);
    const int imgW = int(std::ceil(cardW + 2 * pad)), imgH = int(std::ceil(cardH + 2 * pad));

    // A very long or wide picture at 4x would need gigabytes: shrink the scale until it fits.
    constexpr qreal kMaxSide = 16000, kMaxPixels = 60e6;
    if ((imgW > kMaxSide || imgH > kMaxSide || qreal(imgW) * imgH > kMaxPixels) && k > 0.3) {
        const qreal f = std::min({kMaxSide / imgW, kMaxSide / imgH, std::sqrt(kMaxPixels / (qreal(imgW) * imgH))});
        return draw(source, style, qMax(0.25, k * f * 0.98), measured);
    }
    if (measured) {
        *measured = QSize(imgW, imgH);
        return {};
    }

    QImage img(imgW, imgH, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    const QRectF all(0, 0, imgW, imgH);

    // Background, and the colour it mostly has (the mark is drawn to stand out from it).
    QColor behind = t.window;
    auto gradient = [&](const QColor &from, const QColor &to) {
        const qreal a = qDegreesToRadians(qreal(style.angle));
        const QPointF d(std::cos(a), std::sin(a));
        const qreal half = (std::abs(imgW * d.x()) + std::abs(imgH * d.y())) / 2;
        QLinearGradient g(all.center() - d * half, all.center() + d * half);
        g.setColorAt(0, from);
        g.setColorAt(1, to);
        p.fillRect(all, g);
        behind = QColor::fromRgbF((from.redF() + to.redF()) / 2, (from.greenF() + to.greenF()) / 2, (from.blueF() + to.blueF()) / 2);
    };
    // Without padding there is no background to show: only the card's rounded corners would be filled with it.
    Style::Background bg = pad > 0 ? style.background : Style::Transparent;
    QImage picture;
    if (bg == Style::Picture) {
        picture = QImage(style.picture);
        if (picture.isNull())
            bg = Style::Preset;
    }
    switch (bg) {
    case Style::Preset: {
        const Preset &pr = presets().at(qBound(0, style.preset, int(presets().size()) - 1));
        gradient(pr.from, pr.to);
        break;
    }
    case Style::Gradient:
        gradient(style.color1, style.color2);
        break;
    case Style::Solid:
        p.fillRect(all, style.color1);
        behind = style.color1;
        break;
    case Style::Picture: {
        const QImage cover = picture.scaled(imgW, imgH, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        p.drawImage(QPointF((imgW - cover.width()) / 2.0, (imgH - cover.height()) / 2.0), cover);
        behind = picture.scaled(1, 1, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).pixelColor(0, 0);
        break;
    }
    case Style::Transparent:
        behind = QColor();
        break;
    }

    const QRectF card(pad, pad, cardW, cardH);
    const qreal radius = px(style.radius);
    QPainterPath shape;
    shape.addRoundedRect(card, radius, radius);

    if (style.shadow && pad > 0) {
        QImage shadow(img.size(), QImage::Format_ARGB32_Premultiplied);
        shadow.fill(Qt::transparent);
        {
            QPainter sp(&shadow);
            sp.setRenderHint(QPainter::Antialiasing);
            sp.translate(0, qMin(px(16), pad / 2));
            sp.fillPath(shape, QColor(0, 0, 0, 125));
        }
        blur(shadow, int(qMin(px(30), pad * 0.8)));
        p.drawImage(0, 0, shadow);
    }

    p.fillPath(shape, t.editorBg);
    p.setPen(QPen(t.border, qMax(1.0, px(1))));
    p.setBrush(Qt::NoBrush);
    const qreal inset = qMax(1.0, px(1)) / 2;
    p.drawRoundedRect(card.adjusted(inset, inset, -inset, -inset), radius, radius);

    if (bar) {
        const qreal cy = card.top() + barH / 2;
        if (style.windowButtons) {
            const QColor dots[] = {t.danger, t.warning, t.success};
            p.setPen(Qt::NoPen);
            for (int i = 0; i < 3; ++i) {
                p.setBrush(dots[i]);
                p.drawEllipse(QPointF(card.left() + px(22) + i * px(20), cy), px(6), px(6));
            }
        }
        if (!title.isEmpty()) {
            p.setFont(ui);
            p.setPen(t.textMuted);
            const QRectF r(card.left() + dotsW, card.top(), card.width() - 2 * dotsW, barH);
            p.drawText(r, Qt::AlignCenter, uiFm.elidedText(title, Qt::ElideMiddle, r.width()));
        }
    }

    p.save();
    p.setClipPath(shape);
    p.setFont(font);
    const qreal x = card.left() + padX;
    qreal y = card.top() + barH + padTop;
    const qreal baselineShift = (lineH - fm.height()) / 2;
    for (int i = 0; i < lines.size(); ++i, y += lineH) {
        if (style.lineNumbers) {
            p.setPen(t.gutterFg);
            p.drawText(QRectF(x, y, gutterW - charW * 1.5, lineH), Qt::AlignRight | Qt::AlignVCenter, QString::number(source.firstLine + i));
        }
        p.setPen(t.editorFg);
        layouts[size_t(i)]->draw(&p, QPointF(x + gutterW, y + baselineShift));
    }
    p.restore();

    if (style.watermark) {
        QFont markFont = ui;
        markFont.setBold(true);
        markFont.setLetterSpacing(QFont::PercentageSpacing, 108);
        const QFontMetricsF mfm(markFont);
        const QString name = QStringLiteral("QODE");
        const qreal iconSize = std::round(mfm.height() * 1.15);
        const qreal gap = px(6);
        const qreal w = iconSize + gap + mfm.horizontalAdvance(name);
        QColor ink;
        qreal right, cy;
        if (markInside) {
            ink = t.textMuted;
            right = card.right() - padX;
            cy = card.bottom() - footerH / 2 - px(4);
        } else {
            ink = !behind.isValid() ? t.textMuted : luminance(behind) > 0.6 ? QColor(0, 0, 0, 150) : QColor(255, 255, 255, 200);
            right = card.right();
            cy = card.bottom() + pad / 2;
        }
        const QRectF iconRect(right - w, cy - iconSize / 2, iconSize, iconSize);
        p.setOpacity(0.9);
        QIcon(QStringLiteral(":/icons/qode.svg")).paint(&p, iconRect.toAlignedRect());
        p.setFont(markFont);
        p.setPen(ink);
        p.drawText(QRectF(iconRect.right() + gap, cy - mfm.height() / 2, w, mfm.height()), Qt::AlignLeft | Qt::AlignVCenter, name);
        p.setOpacity(1);
    }
    return img;
}

} // namespace

QImage render(const Source &source, const Style &style, qreal scaleOverride)
{
    return draw(source, style, scaleOverride > 0 ? scaleOverride : qreal(style.scale), nullptr);
}

QSize measure(const Source &source, const Style &style)
{
    QSize size;
    draw(source, style, style.scale, &size);
    return size;
}

} // namespace CodeImage
