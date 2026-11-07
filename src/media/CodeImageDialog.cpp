#include "CodeImageDialog.h"

#include "dialogs/FileDialogs.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"
#include "settings/ThemeManager.h"

#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
// Background combo entries after the presets (the presets are 0..n-1).
enum : int { kSolid = -1, kGradient = -2, kPicture = -3, kTransparent = -4 };

QString lastSaveDir;
}

CodeImageDialog::CodeImageDialog(CodeImage::Source source, QWidget *parent)
    : QDialog(parent)
    , m_source(std::move(source))
    , m_style(CodeImage::Style::load())
    , m_timer(new QTimer(this))
{
    setWindowTitle(tr("Copy as Image"));
    const Theme t = Theme::byName(SettingsManager::instance().theme());

    m_preview = new QLabel(this);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setMinimumSize(560, 380);
    m_preview->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_preview->setStyleSheet(QStringLiteral("QLabel { background: %1; border: 1px solid %2; border-radius: 8px; }")
                                 .arg(t.panel.name(), t.border.name()));

    // --- options
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_background = new QComboBox(this);
    const QVector<CodeImage::Preset> &presets = CodeImage::presets();
    for (int i = 0; i < presets.size(); ++i) {
        QPixmap swatch(28, 16);
        swatch.fill(Qt::transparent);
        QPainter p(&swatch);
        p.setRenderHint(QPainter::Antialiasing);
        QLinearGradient g(0, 0, 28, 16);
        g.setColorAt(0, presets[i].from);
        g.setColorAt(1, presets[i].to);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(0.5, 0.5, 27, 15), 4, 4);
        p.end();
        m_background->addItem(QIcon(swatch), tr(presets[i].name), i);
    }
    m_background->insertSeparator(m_background->count());
    m_background->addItem(tr("Colour"), int(kSolid));
    m_background->addItem(tr("Gradient"), int(kGradient));
    m_background->addItem(tr("Picture…"), int(kPicture));
    m_background->addItem(tr("None (transparent)"), int(kTransparent));
    form->addRow(tr("Background"), m_background);

    m_coloursRow = new QWidget(this);
    auto *colours = new QHBoxLayout(m_coloursRow);
    colours->setContentsMargins(0, 0, 0, 0);
    m_colour1 = new QToolButton(m_coloursRow);
    m_colour2 = new QToolButton(m_coloursRow);
    for (QToolButton *b : {m_colour1, m_colour2}) {
        b->setIconSize(QSize(28, 16));
        b->setToolTip(tr("Choose the colour"));
    }
    m_angle = new QSpinBox(m_coloursRow);
    m_angle->setRange(0, 359);
    m_angle->setWrapping(true);
    m_angle->setSingleStep(15);
    m_angle->setSuffix(QStringLiteral("°"));
    m_angle->setToolTip(tr("Direction of the gradient"));
    colours->addWidget(m_colour1);
    colours->addWidget(m_colour2);
    colours->addWidget(m_angle);
    colours->addStretch();
    form->addRow(QString(), m_coloursRow);

    m_pictureRow = new QWidget(this);
    auto *pictureLay = new QHBoxLayout(m_pictureRow);
    pictureLay->setContentsMargins(0, 0, 0, 0);
    m_pictureBtn = new QPushButton(m_pictureRow);
    m_pictureBtn->setToolTip(tr("Choose another picture"));
    pictureLay->addWidget(m_pictureBtn, 1);
    form->addRow(QString(), m_pictureRow);

    auto slider = [this, form](const QString &label, int max, int step, QSlider *&s, QLabel *&value) {
        auto *row = new QHBoxLayout;
        s = new QSlider(Qt::Horizontal, this);
        s->setRange(0, max);
        s->setSingleStep(step);
        s->setPageStep(step * 2);
        value = new QLabel(this);
        value->setMinimumWidth(QFontMetrics(value->font()).horizontalAdvance(QStringLiteral("000 px")));
        row->addWidget(s, 1);
        row->addWidget(value);
        form->addRow(label, row);
    };
    slider(tr("Padding"), 160, 8, m_padding, m_paddingValue);
    slider(tr("Corners"), 40, 2, m_radius, m_radiusValue);

    m_fontSize = new QSpinBox(this);
    m_fontSize->setRange(8, 40);
    m_fontSize->setSuffix(QStringLiteral(" px"));
    form->addRow(tr("Font size"), m_fontSize);

    m_theme = new QComboBox(this);
    m_theme->addItem(tr("Current theme"), QString());
    for (const ThemeManager::Info &info : ThemeManager::instance().themes())
        m_theme->addItem(info.name, info.id);
    form->addRow(tr("Theme"), m_theme);

    auto *titleRow = new QHBoxLayout;
    m_titleOn = new QCheckBox(this);
    m_titleOn->setToolTip(tr("Show a title (the file name) above the code"));
    m_title = new QLineEdit(m_source.title, this);
    m_title->setPlaceholderText(tr("File name"));
    titleRow->addWidget(m_titleOn);
    titleRow->addWidget(m_title, 1);
    form->addRow(tr("Title"), titleRow);

    m_buttons = new QCheckBox(tr("Window buttons"), this);
    m_lineNumbers = new QCheckBox(tr("Line numbers"), this);
    m_shadow = new QCheckBox(tr("Shadow"), this);
    m_watermark = new QCheckBox(tr("QODE mark"), this);
    for (QCheckBox *c : {m_buttons, m_lineNumbers, m_shadow, m_watermark})
        form->addRow(QString(), c);

    m_scale = new QComboBox(this);
    for (int s = 1; s <= 4; ++s)
        m_scale->addItem(QStringLiteral("%1×").arg(s), s);
    m_scale->setToolTip(tr("Pixels per point: 2× and up stay sharp on high-resolution screens"));
    form->addRow(tr("Resolution"), m_scale);

    m_size = new QLabel(this);
    m_size->setWordWrap(true);
    m_size->setStyleSheet(QStringLiteral("color: %1").arg(t.textMuted.name()));
    form->addRow(QString(), m_size);

    auto *side = new QVBoxLayout;
    side->addLayout(form);
    side->addStretch();

    auto *body = new QHBoxLayout;
    body->addWidget(m_preview, 1);
    body->addLayout(side);

    auto *save = new QPushButton(tr("Save PNG…"), this);
    m_copyBtn = new QPushButton(tr("Copy"), this);
    m_copyBtn->setDefault(true);
    auto *close = new QPushButton(tr("Close"), this);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(save);
    buttons->addWidget(m_copyBtn);
    buttons->addWidget(close);

    auto *lay = new QVBoxLayout(this);
    lay->addLayout(body, 1);
    lay->addLayout(buttons);

    // --- current values
    int bgIndex = 0;
    switch (m_style.background) {
    case CodeImage::Style::Preset: bgIndex = m_background->findData(m_style.preset); break;
    case CodeImage::Style::Solid: bgIndex = m_background->findData(int(kSolid)); break;
    case CodeImage::Style::Gradient: bgIndex = m_background->findData(int(kGradient)); break;
    case CodeImage::Style::Picture: bgIndex = m_background->findData(int(kPicture)); break;
    case CodeImage::Style::Transparent: bgIndex = m_background->findData(int(kTransparent)); break;
    }
    m_background->setCurrentIndex(qMax(0, bgIndex));
    m_lastBackground = m_background->currentIndex();
    m_angle->setValue(m_style.angle);
    m_padding->setValue(m_style.padding);
    m_radius->setValue(m_style.radius);
    m_fontSize->setValue(m_style.fontSize);
    m_theme->setCurrentIndex(qMax(0, m_theme->findData(m_style.theme)));
    m_titleOn->setChecked(m_style.title);
    m_buttons->setChecked(m_style.windowButtons);
    m_lineNumbers->setChecked(m_style.lineNumbers);
    m_shadow->setChecked(m_style.shadow);
    m_watermark->setChecked(m_style.watermark);
    m_scale->setCurrentIndex(m_style.scale - 1);
    updateSwatches();
    syncVisibility();

    // --- changes
    m_timer->setSingleShot(true);
    m_timer->setInterval(40);
    connect(m_timer, &QTimer::timeout, this, &CodeImageDialog::refresh);
    connect(m_background, &QComboBox::activated, this, [this] {
        const int kind = m_background->currentData().toInt();
        if (kind == kPicture && (m_style.picture.isEmpty() || !QFileInfo::exists(m_style.picture))) {
            choosePicture();
            if (m_style.picture.isEmpty()) {
                m_background->setCurrentIndex(m_lastBackground);
                return;
            }
        }
        if (kind >= 0) {
            m_style.background = CodeImage::Style::Preset;
            m_style.preset = kind;
        } else {
            m_style.background = kind == kSolid ? CodeImage::Style::Solid
                                 : kind == kGradient ? CodeImage::Style::Gradient
                                 : kind == kPicture ? CodeImage::Style::Picture
                                                    : CodeImage::Style::Transparent;
        }
        m_lastBackground = m_background->currentIndex();
        syncVisibility();
        schedule();
    });
    connect(m_colour1, &QToolButton::clicked, this, [this] { pickColour(1); });
    connect(m_colour2, &QToolButton::clicked, this, [this] { pickColour(2); });
    connect(m_pictureBtn, &QPushButton::clicked, this, [this] {
        choosePicture();
        schedule();
    });
    connect(m_angle, &QSpinBox::valueChanged, this, [this](int v) { m_style.angle = v; schedule(); });
    connect(m_padding, &QSlider::valueChanged, this, [this](int v) { m_style.padding = v; schedule(); });
    connect(m_radius, &QSlider::valueChanged, this, [this](int v) { m_style.radius = v; schedule(); });
    connect(m_fontSize, &QSpinBox::valueChanged, this, [this](int v) { m_style.fontSize = v; schedule(); });
    connect(m_theme, &QComboBox::currentIndexChanged, this, [this] { m_style.theme = m_theme->currentData().toString(); schedule(); });
    connect(m_scale, &QComboBox::currentIndexChanged, this, [this] { m_style.scale = m_scale->currentData().toInt(); schedule(); });
    connect(m_title, &QLineEdit::textChanged, this, [this](const QString &text) { m_source.title = text; schedule(); });
    auto toggle = [this](QCheckBox *box, bool CodeImage::Style::*field) {
        connect(box, &QCheckBox::toggled, this, [this, field](bool on) { m_style.*field = on; syncVisibility(); schedule(); });
    };
    toggle(m_titleOn, &CodeImage::Style::title);
    toggle(m_buttons, &CodeImage::Style::windowButtons);
    toggle(m_lineNumbers, &CodeImage::Style::lineNumbers);
    toggle(m_shadow, &CodeImage::Style::shadow);
    toggle(m_watermark, &CodeImage::Style::watermark);
    connect(m_copyBtn, &QPushButton::clicked, this, &CodeImageDialog::copy);
    connect(save, &QPushButton::clicked, this, &CodeImageDialog::savePng);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);

    resize(1000, 600);
    refresh();
}

void CodeImageDialog::show(CodeImage::Source source, QWidget *parent)
{
    auto *dlg = new CodeImageDialog(std::move(source), parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->QDialog::show();
}

void CodeImageDialog::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    schedule();
}

void CodeImageDialog::schedule()
{
    m_timer->start();
}

void CodeImageDialog::syncVisibility()
{
    const int kind = m_background->currentData().toInt();
    const bool gradient = kind >= 0 || kind == kGradient;
    m_coloursRow->setVisible(kind == kSolid || gradient);
    m_colour1->setVisible(kind == kSolid || kind == kGradient);
    m_colour2->setVisible(kind == kGradient);
    m_angle->setVisible(gradient);
    m_pictureRow->setVisible(kind == kPicture);
    m_pictureBtn->setText(m_style.picture.isEmpty() ? tr("Choose…") : QFileInfo(m_style.picture).fileName());
    m_title->setEnabled(m_titleOn->isChecked());
}

void CodeImageDialog::refresh()
{
    m_style.save();
    m_paddingValue->setText(QStringLiteral("%1 px").arg(m_style.padding));
    m_radiusValue->setText(QStringLiteral("%1 px").arg(m_style.radius));

    // The preview at the screen's own resolution, made smaller when it does not fit.
    const qreal dpr = devicePixelRatioF();
    QImage img = CodeImage::render(m_source, m_style, dpr);
    const QSize room = (m_preview->contentsRect().size() - QSize(24, 24)) * dpr;
    if (img.width() > room.width() || img.height() > room.height())
        img = img.scaled(room.expandedTo(QSize(1, 1)), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPixmap pm = QPixmap::fromImage(img);
    pm.setDevicePixelRatio(dpr);
    m_preview->setPixmap(pm);

    const QSize size = CodeImage::measure(m_source, m_style);
    QString info = tr("%1 × %2 px").arg(size.width()).arg(size.height());
    if (m_source.truncated)
        info += QLatin1Char('\n') + tr("Only the first %1 lines are shown. Select the part to share.").arg(CodeImage::kMaxLines);
    m_size->setText(info);
}

void CodeImageDialog::choosePicture()
{
    QStringList filters;
    for (const QByteArray &f : QImageReader::supportedImageFormats())
        filters << QStringLiteral("*.") + QString::fromLatin1(f);
    const QString start = m_style.picture.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                                                    : QFileInfo(m_style.picture).absolutePath();
    const QString file = FileDialogs::getOpenFileName(this, tr("Background Picture"), start,
                                                      tr("Images (%1)").arg(filters.join(QLatin1Char(' '))));
    if (file.isEmpty())
        return;
    m_style.picture = file;
    m_style.background = CodeImage::Style::Picture;
    syncVisibility();
}

void CodeImageDialog::pickColour(int which)
{
    QColor &c = which == 1 ? m_style.color1 : m_style.color2;
    const QColor chosen = QColorDialog::getColor(c, this, tr("Background Colour"));
    if (!chosen.isValid())
        return;
    c = chosen;
    updateSwatches();
    schedule();
}

void CodeImageDialog::updateSwatches()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    auto swatch = [&t](const QColor &c) {
        QPixmap pm(28, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(t.border);
        p.setBrush(c);
        p.drawRoundedRect(QRectF(0.5, 0.5, 27, 15), 4, 4);
        return QIcon(pm);
    };
    m_colour1->setIcon(swatch(m_style.color1));
    m_colour2->setIcon(swatch(m_style.color2));
}

void CodeImageDialog::copy()
{
    const QImage img = CodeImage::render(m_source, m_style);
    // As an image and as PNG bytes: chat apps and image editors ask for one or the other.
    auto *mime = new QMimeData;
    mime->setImageData(img);
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    img.save(&buffer, "PNG");
    mime->setData(QStringLiteral("image/png"), png);
    QGuiApplication::clipboard()->setMimeData(mime);
    m_copyBtn->setText(tr("Copied"));
    QTimer::singleShot(1500, m_copyBtn, [btn = m_copyBtn] { btn->setText(tr("Copy")); });
}

void CodeImageDialog::savePng()
{
    if (lastSaveDir.isEmpty())
        lastSaveDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    QString base = QFileInfo(m_source.title).completeBaseName();
    if (base.isEmpty())
        base = QStringLiteral("code");
    const QString file = FileDialogs::getSaveFileName(this, tr("Save Image"), QDir(lastSaveDir).filePath(base + QStringLiteral(".png")),
                                                      tr("PNG images (*.png)"));
    if (file.isEmpty())
        return;
    lastSaveDir = QFileInfo(file).absolutePath();
    const QString path = file.endsWith(QLatin1String(".png"), Qt::CaseInsensitive) ? file : file + QStringLiteral(".png");
    if (!CodeImage::render(m_source, m_style).save(path, "PNG"))
        QMessageBox::warning(this, tr("Save Image"), tr("Could not write %1.").arg(QDir::toNativeSeparators(path)));
}
