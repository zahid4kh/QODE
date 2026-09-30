#include "MediaPanel.h"

#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QLocale>
#include <QMimeDatabase>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QShowEvent>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#ifdef QODE_HAS_MULTIMEDIA
#include <QAudioOutput>
#include <QMediaFormat>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QVideoSink>
#include <QVideoWidget>
#endif

namespace {

const QStringList kImageExts = {QStringLiteral("png"),  QStringLiteral("jpg"),  QStringLiteral("jpeg"),
                                QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("webp"),
                                QStringLiteral("ico"),  QStringLiteral("tif"),  QStringLiteral("tiff"),
                                QStringLiteral("icns"), QStringLiteral("avif"), QStringLiteral("jfif")};
const QStringList kVideoExts = {QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("webm"),
                                QStringLiteral("mov"), QStringLiteral("avi"), QStringLiteral("m4v"),
                                QStringLiteral("ogv"), QStringLiteral("flv"), QStringLiteral("wmv"),
                                QStringLiteral("mpg"), QStringLiteral("mpeg")};

QString formatTime(qint64 ms)
{
    const qint64 s = qMax<qint64>(0, ms) / 1000;
    const qint64 h = s / 3600, m = (s / 60) % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString aspectRatio(int w, int h)
{
    if (w <= 0 || h <= 0)
        return {};
    int a = w, b = h;
    while (b) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return QStringLiteral("%1:%2").arg(w / a).arg(h / a);
}

QString sizeText(qint64 bytes)
{
    return QStringLiteral("%1 (%2 bytes)").arg(QLocale().formattedDataSize(bytes, 2, QLocale::DataSizeTraditionalFormat))
        .arg(QLocale().toString(bytes));
}

} // namespace

// Fit-to-panel image viewer with wheel zoom, drag to pan and double-click to reset.
class ImageView : public QWidget
{
public:
    explicit ImageView(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setMinimumHeight(140);
        connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, qOverload<>(&QWidget::update));
    }

    void setPixmap(const QPixmap &pix, bool resetView = true)
    {
        m_pix = pix;
        if (resetView)
            resetZoom();
        update();
    }
    void resetZoom()
    {
        m_zoom = 0;
        m_pan = {};
        update();
    }

protected:
    qreal fitScale() const
    {
        if (m_pix.isNull())
            return 1;
        const qreal w = m_pix.width() / m_pix.devicePixelRatio(), h = m_pix.height() / m_pix.devicePixelRatio();
        return qMin(1.0, qMin((width() - 16) / w, (height() - 16) / h));
    }
    qreal scale() const { return m_zoom > 0 ? m_zoom : fitScale(); }
    QRectF imageRect() const
    {
        const qreal s = scale();
        const QSizeF sz(m_pix.width() / m_pix.devicePixelRatio() * s, m_pix.height() / m_pix.devicePixelRatio() * s);
        const QPointF c = QPointF(width() / 2.0, height() / 2.0) + m_pan;
        return QRectF(c.x() - sz.width() / 2, c.y() - sz.height() / 2, sz.width(), sz.height());
    }

    void paintEvent(QPaintEvent *) override
    {
        const Theme t = Theme::byName(SettingsManager::instance().theme());
        QPainter p(this);
        p.fillRect(rect(), t.editorBg);
        if (m_pix.isNull())
            return;
        const QRectF r = imageRect();
        if (m_pix.hasAlphaChannel()) { // checkerboard behind transparency
            p.save();
            p.setClipRect(r);
            const QColor a = t.dark ? QColor(0x2b, 0x2b, 0x2b) : QColor(0xf2, 0xf2, 0xf2);
            const QColor b = t.dark ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xdc, 0xdc, 0xdc);
            constexpr int cell = 10;
            for (int y = 0; y * cell < r.height(); ++y)
                for (int x = 0; x * cell < r.width(); ++x)
                    p.fillRect(QRectF(r.left() + x * cell, r.top() + y * cell, cell, cell), (x + y) % 2 ? a : b);
            p.restore();
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, scale() < 4.0);
        p.drawPixmap(r, m_pix, QRectF(m_pix.rect()));
        p.setPen(t.border);
        p.setBrush(Qt::NoBrush);
        p.drawRect(r.adjusted(-0.5, -0.5, 0.5, 0.5));

        const QString zoom = QStringLiteral("%1%").arg(qRound(scale() * 100));
        const QRect badge(width() - 58, height() - 26, 52, 20);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(badge, 4, 4);
        p.setPen(Qt::white);
        p.drawText(badge, Qt::AlignCenter, zoom);
    }
    void wheelEvent(QWheelEvent *e) override
    {
        if (m_pix.isNull())
            return;
        const qreal s1 = scale();
        const qreal s2 = qBound(0.02, s1 * std::pow(1.0015, e->angleDelta().y()), 32.0);
        const QPointF center(width() / 2.0, height() / 2.0);
        const QPointF c = center + m_pan, pos = e->position();
        m_zoom = s2;
        m_pan = (pos - (pos - c) * (s2 / s1)) - center;
        update();
    }
    void mousePressEvent(QMouseEvent *e) override
    {
        m_last = e->pos();
        m_drag = e->button() == Qt::LeftButton;
        if (m_drag)
            setCursor(Qt::ClosedHandCursor);
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (!m_drag)
            return;
        m_pan += e->pos() - m_last;
        m_last = e->pos();
        update();
    }
    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_drag = false;
        unsetCursor();
    }
    void mouseDoubleClickEvent(QMouseEvent *) override { resetZoom(); }

private:
    QPixmap m_pix;
    qreal m_zoom = 0; // 0 = fit
    QPointF m_pan;
    QPoint m_last;
    bool m_drag = false;
};

MediaPanel::MediaPanel(QWidget *parent)
    : QWidget(parent)
{
    m_title = new QLabel(tr("MEDIA"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    auto *closeBtn = new QToolButton(this);
    Icons::bind(closeBtn, QStringLiteral(":/new-icons/x.svg"));
    closeBtn->setToolTip(tr("Close media panel"));
    closeBtn->setAutoRaise(true);
    connect(closeBtn, &QToolButton::clicked, this, &MediaPanel::closeRequested);

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(0, 0, 4, 0);
    hl->addWidget(m_title, 1);
    hl->addWidget(closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#mediaHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#mediaHeader QLabel { background: transparent; border: none; }"));

    // --- preview area ---
    m_image = new ImageView(this);
    m_message = new QLabel(this);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_message->setContentsMargins(16, 16, 16, 16);
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_image);   // 0
    m_stack->addWidget(m_message); // 1
#ifdef QODE_HAS_MULTIMEDIA
    m_video = new QVideoWidget(this);
    m_video->setStyleSheet(QStringLiteral("background: black;"));
    m_stack->addWidget(m_video); // 2
    m_audio = new QAudioOutput(this);
    m_audio->setVolume(0.7f);
    m_player = new QMediaPlayer(this);
    m_player->setAudioOutput(m_audio);
    m_player->setVideoOutput(m_video);
#endif

    // --- video controls ---
    m_playBtn = new QToolButton(this);
    m_playBtn->setAutoRaise(true);
    Icons::bind(m_playBtn, QStringLiteral(":/new-icons/play.svg"));
    m_seek = new QSlider(Qt::Horizontal, this);
    m_seek->setRange(0, 0);
    m_time = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    m_muteBtn = new QToolButton(this);
    m_muteBtn->setAutoRaise(true);
    m_muteBtn->setCheckable(true);
    Icons::bind(m_muteBtn, QStringLiteral(":/new-icons/volume-2.svg"));
    m_volume = new QSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setValue(70);
    m_volume->setFixedWidth(70);
    m_controls = new QWidget(this);
    auto *cl = new QHBoxLayout(m_controls);
    cl->setContentsMargins(6, 4, 6, 4);
    cl->addWidget(m_playBtn);
    cl->addWidget(m_seek, 1);
    cl->addWidget(m_time);
    cl->addWidget(m_muteBtn);
    cl->addWidget(m_volume);
    m_controls->hide();

    auto *preview = new QWidget(this);
    auto *pl = new QVBoxLayout(preview);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(0);
    pl->addWidget(m_stack, 1);
    pl->addWidget(m_controls);

    // --- info area ---
    auto *infoBody = new QWidget;
    auto *il = new QVBoxLayout(infoBody);
    il->setContentsMargins(12, 10, 12, 10);
    m_info = new QGridLayout;
    m_info->setColumnStretch(1, 1);
    m_info->setHorizontalSpacing(12);
    m_info->setVerticalSpacing(4);
    il->addLayout(m_info);

    auto *copyBtn = new QPushButton(tr("Copy Path"), infoBody);
    auto *revealBtn = new QPushButton(tr("Show in Folder"), infoBody);
    auto *externalBtn = new QPushButton(tr("Open Externally"), infoBody);
    auto *bl = new QHBoxLayout;
    bl->addWidget(copyBtn);
    bl->addWidget(revealBtn);
    bl->addWidget(externalBtn);
    bl->addStretch(1);
    il->addSpacing(6);
    il->addLayout(bl);
    il->addStretch(1);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &MediaPanel::refreshInfo);
    connect(copyBtn, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_path); });
    connect(revealBtn, &QPushButton::clicked, this, [this] {
        if (!m_path.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_path).absolutePath()));
    });
    connect(externalBtn, &QPushButton::clicked, this, [this] {
        if (!m_path.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_path));
    });

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(infoBody);

    m_split = new QSplitter(Qt::Vertical, this);
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(1);
    m_split->addWidget(preview);
    m_split->addWidget(scroll);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(m_split, 1);

#ifdef QODE_HAS_MULTIMEDIA
    connect(m_playBtn, &QToolButton::clicked, this, [this] {
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            m_player->pause();
        else
            m_player->play();
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState st) {
        Icons::bind(m_playBtn, st == QMediaPlayer::PlayingState ? QStringLiteral(":/new-icons/pause.svg")
                                                                : QStringLiteral(":/new-icons/play.svg"));
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 d) {
        m_seek->setRange(0, int(d));
        updateTime();
        refreshInfo();
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 pos) {
        if (!m_seek->isSliderDown())
            m_seek->setValue(int(pos));
        updateTime();
    });
    connect(m_player, &QMediaPlayer::metaDataChanged, this, &MediaPanel::refreshInfo);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus st) {
        if (st == QMediaPlayer::EndOfMedia) {
            m_player->pause();
            m_player->setPosition(0);
        }
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &msg) {
        showMessage(tr("This video can't be played:\n%1").arg(msg));
    });
    connect(m_seek, &QSlider::sliderMoved, m_player, &QMediaPlayer::setPosition);
    connect(m_seek, &QSlider::sliderReleased, this, [this] { m_player->setPosition(m_seek->value()); });
    connect(m_volume, &QSlider::valueChanged, this, [this](int v) { m_audio->setVolume(v / 100.0f); });
    connect(m_muteBtn, &QToolButton::toggled, this, [this](bool muted) {
        m_audio->setMuted(muted);
        Icons::bind(m_muteBtn, muted ? QStringLiteral(":/new-icons/volume-x.svg") : QStringLiteral(":/new-icons/volume-2.svg"));
    });
#endif
}

bool MediaPanel::isImage(const QString &path)
{
    return kImageExts.contains(QFileInfo(path).suffix().toLower());
}

bool MediaPanel::isVideo(const QString &path)
{
    return kVideoExts.contains(QFileInfo(path).suffix().toLower());
}

void MediaPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_sized) { // preview and details get equal room on first show
        m_sized = true;
        const int h = m_split->height();
        m_split->setSizes({h / 2, h - h / 2});
    }
}

void MediaPanel::setProjectRoot(const QString &root)
{
    m_root = root;
    if (!m_path.isEmpty())
        refreshInfo();
}

void MediaPanel::clear()
{
#ifdef QODE_HAS_MULTIMEDIA
    m_player->stop();
    m_player->setSource(QUrl());
#endif
    if (m_movie) {
        m_movie->stop();
        m_movie->deleteLater();
        m_movie = nullptr;
    }
    m_image->setPixmap(QPixmap());
    m_path.clear();
    m_svgPreview = false;
    m_extra.clear();
    m_controls->hide();
    m_title->setText(tr("MEDIA"));
    refreshInfo();
}

void MediaPanel::openMedia(const QString &pathIn)
{
    clear();
    m_path = QFileInfo(pathIn).absoluteFilePath();
    m_title->setText(tr("MEDIA — %1").arg(QFileInfo(m_path).fileName()));
    if (isVideo(m_path))
        showVideo();
    else
        showImage();
    refreshInfo();
}

void MediaPanel::previewSvg(const QString &pathIn, const QString &text)
{
    const QString path = QFileInfo(pathIn).absoluteFilePath();
    const bool same = m_svgPreview && path == m_path;
    if (!same) {
        clear();
        m_path = path;
        m_svgPreview = true;
        m_title->setText(tr("PREVIEW — %1").arg(QFileInfo(path).fileName()));
    }
    QByteArray data = text.toUtf8();
    QBuffer buf(&data);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf, "svg");
    const QSize dim = reader.size();
    m_extra.clear();
    if (!dim.isValid() || dim.isEmpty()) {
        showMessage(tr("This SVG cannot be rendered (yet).\n%1").arg(reader.errorString()));
        refreshInfo();
        return;
    }
    // Rasterise large so that zooming in stays sharp.
    const int longest = qMax(dim.width(), dim.height());
    const qreal k = qBound<qreal>(0.25, 1600.0 / longest, 64.0);
    reader.setScaledSize(QSize(qMax(1, qRound(dim.width() * k)), qMax(1, qRound(dim.height() * k))));
    const QImage img = reader.read();
    if (img.isNull()) {
        showMessage(tr("This SVG cannot be rendered (yet).\n%1").arg(reader.errorString()));
        refreshInfo();
        return;
    }
    m_extra << tr("Resolution") << tr("%1 × %2 px").arg(dim.width()).arg(dim.height());
    const QString ratio = aspectRatio(dim.width(), dim.height());
    if (!ratio.isEmpty())
        m_extra << tr("Aspect ratio") << ratio;
    m_extra << tr("Format") << tr("SVG (vector)");
    m_image->setPixmap(QPixmap::fromImage(img), !same);
    m_stack->setCurrentWidget(m_image);
    refreshInfo();
}

void MediaPanel::showMessage(const QString &text)
{
    m_message->setText(text);
    m_stack->setCurrentWidget(m_message);
}

void MediaPanel::showImage()
{
    QImageReader reader(m_path);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    const QByteArray fmt = reader.format();
    const bool animated = reader.supportsAnimation() && reader.imageCount() != 1;
    const QImage img = reader.read();
    if (img.isNull()) {
        showMessage(tr("Unable to display this image.\n%1").arg(reader.errorString()));
        return;
    }
    const QSize dim = size.isValid() ? size : img.size();
    m_extra << tr("Resolution") << tr("%1 × %2 px").arg(dim.width()).arg(dim.height());
    const QString ratio = aspectRatio(dim.width(), dim.height());
    if (!ratio.isEmpty())
        m_extra << tr("Aspect ratio") << ratio;
    m_extra << tr("Megapixels") << QLocale().toString(dim.width() * double(dim.height()) / 1e6, 'f', 2) + QStringLiteral(" MP");
    if (!fmt.isEmpty())
        m_extra << tr("Format") << QString::fromLatin1(fmt).toUpper();
    m_extra << tr("Colour depth") << tr("%1-bit").arg(img.depth());
    m_extra << tr("Transparency") << (img.hasAlphaChannel() ? tr("Yes") : tr("No"));
    if (img.dotsPerMeterX() > 0)
        m_extra << tr("Density") << tr("%1 DPI").arg(qRound(img.dotsPerMeterX() * 0.0254));

    if (animated) {
        m_movie = new QMovie(m_path, QByteArray(), this);
        m_movie->setCacheMode(QMovie::CacheAll);
        connect(m_movie, &QMovie::frameChanged, this, [this] { m_image->setPixmap(m_movie->currentPixmap(), false); });
        m_movie->start();
        m_image->setPixmap(QPixmap::fromImage(img));
        m_extra << tr("Frames") << QString::number(m_movie->frameCount());
    } else {
        m_image->setPixmap(QPixmap::fromImage(img));
    }
    m_stack->setCurrentWidget(m_image);
}

void MediaPanel::showVideo()
{
#ifdef QODE_HAS_MULTIMEDIA
    m_controls->show();
    m_seek->setRange(0, 0);
    m_seek->setValue(0);
    m_stack->setCurrentWidget(m_video);
    m_player->setSource(QUrl::fromLocalFile(m_path));
    m_player->pause(); // paused on the first frame
    updateTime();
#else
    showMessage(tr("This build of QODE has no video support (Qt Multimedia was not found).\nUse “Open Externally” to play it."));
#endif
}

void MediaPanel::updateTime()
{
#ifdef QODE_HAS_MULTIMEDIA
    m_time->setText(QStringLiteral("%1 / %2").arg(formatTime(m_player->position()), formatTime(m_player->duration())));
#endif
}

void MediaPanel::addInfo(const QString &key, const QString &value, bool wrapPath)
{
    const int row = m_info->rowCount();
    auto *k = new QLabel(key);
    k->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::byName(SettingsManager::instance().theme()).textMuted.name()));
    k->setAlignment(Qt::AlignRight | Qt::AlignTop);
    auto *v = new QLabel;
    v->setText(wrapPath ? QString(value).replace(QLatin1Char('/'), QStringLiteral("/​")) : value);
    v->setWordWrap(true);
    v->setTextInteractionFlags(Qt::TextSelectableByMouse);
    v->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    v->setToolTip(value);
    m_info->addWidget(k, row, 0);
    m_info->addWidget(v, row, 1);
}

void MediaPanel::refreshInfo()
{
    while (QLayoutItem *item = m_info->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (m_path.isEmpty())
        return;

    const QFileInfo fi(m_path);
    QStringList extra = m_extra;
#ifdef QODE_HAS_MULTIMEDIA
    if (isVideo(m_path)) {
        const QMediaMetaData md = m_player->metaData();
        QSize res = md.value(QMediaMetaData::Resolution).toSize();
        if (!res.isValid())
            res = m_video->videoSink() ? m_video->videoSink()->videoSize() : QSize();
        if (res.isValid() && !res.isEmpty()) {
            extra << tr("Resolution") << tr("%1 × %2 px").arg(res.width()).arg(res.height());
            const QString ratio = aspectRatio(res.width(), res.height());
            if (!ratio.isEmpty())
                extra << tr("Aspect ratio") << ratio;
        }
        if (m_player->duration() > 0)
            extra << tr("Duration") << formatTime(m_player->duration());
        const double fps = md.value(QMediaMetaData::VideoFrameRate).toDouble();
        if (fps > 0)
            extra << tr("Frame rate") << tr("%1 fps").arg(QLocale().toString(fps, 'f', 2));
        const int vcodec = md.value(QMediaMetaData::VideoCodec).toInt();
        if (md.value(QMediaMetaData::VideoCodec).isValid())
            extra << tr("Video codec") << QMediaFormat::videoCodecName(QMediaFormat::VideoCodec(vcodec));
        if (md.value(QMediaMetaData::AudioCodec).isValid())
            extra << tr("Audio codec") << QMediaFormat::audioCodecName(QMediaFormat::AudioCodec(md.value(QMediaMetaData::AudioCodec).toInt()));
        const int vbr = md.value(QMediaMetaData::VideoBitRate).toInt();
        if (vbr > 0)
            extra << tr("Video bitrate") << tr("%1 kb/s").arg(vbr / 1000);
        const int abr = md.value(QMediaMetaData::AudioBitRate).toInt();
        if (abr > 0)
            extra << tr("Audio bitrate") << tr("%1 kb/s").arg(abr / 1000);
        if (md.value(QMediaMetaData::FileFormat).isValid())
            extra << tr("Container") << QMediaFormat::fileFormatName(QMediaFormat::FileFormat(md.value(QMediaMetaData::FileFormat).toInt()));
    }
#endif

    addInfo(tr("Name"), fi.fileName());
    addInfo(tr("Type"), QMimeDatabase().mimeTypeForFile(fi).comment());
    if (!fi.exists()) {
        addInfo(tr("Status"), tr("File no longer exists"));
    } else {
        addInfo(tr("Size"), sizeText(fi.size()));
    }
    for (int i = 0; i + 1 < extra.size(); i += 2)
        addInfo(extra[i], extra[i + 1]);
    if (fi.exists())
        addInfo(tr("Modified"), QLocale().toString(fi.lastModified(), QLocale::LongFormat));
    addInfo(tr("Path"), fi.absoluteFilePath(), true);
    if (!m_root.isEmpty() && fi.absoluteFilePath().startsWith(m_root + QLatin1Char('/')))
        addInfo(tr("Relative path"), QDir(m_root).relativeFilePath(fi.absoluteFilePath()), true);
    else
        addInfo(tr("Relative path"), tr("— (outside the project)"));
    addInfo(tr("Folder"), fi.absolutePath(), true);
}
