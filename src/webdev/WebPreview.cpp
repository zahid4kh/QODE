#include "WebPreview.h"
#include "platform/AppPaths.h"

#include "DevServer.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QBuffer>
#include <QCompleter>
#include <QDesktopServices>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringListModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>

#ifdef QODE_HAS_WEBENGINE
#include <QWebEngineHistory>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QtWebEngineCore/qtwebenginecoreglobal.h>
#include <QWebEngineView>
#endif

#include <functional>
#include <memory>
#include <utility>

namespace {

struct DeviceSize
{
    int width, height; // 0 height = as tall as the panel
    int bezel, radius; // CSS px of the frame drawn around the screen and of the screen's rounded corners
    const char *name;
};

DeviceSize deviceSize(WebPreview::Device d)
{
    switch (d) {
    case WebPreview::Device::Tablet:
        return {820, 1180, 18, 20, QT_TRANSLATE_NOOP("WebPreview", "Tablet")};
    case WebPreview::Device::Desktop:
        return {1440, 0, 0, 0, QT_TRANSLATE_NOOP("WebPreview", "Desktop")};
    case WebPreview::Device::Phone: // a mockup, see Mockup
    case WebPreview::Device::Fit:
        break;
    }
    return {0, 0, 0, 0, nullptr};
}

// A phone picture the page is shown in. Rectangles are in the file's own units (PNG pixels / SVG viewBox); the
// screen of both files is opaque black, so the page is laid over it and the corners and camera are drawn on top.
struct Mockup
{
    const char *resource;
    QSizeF image;
    QRectF screen;
    qreal radius;     // of the screen's corners
    QRectF body;      // the phone's outline, for the shadow
    qreal bodyRadius;
    QRectF camera;    // Dynamic Island / punch hole, drawn over the page
    QSize css;        // the page's size in CSS px
    int top, bottom;  // safe-area insets in CSS px, upright: status bar over the camera, home indicator / gesture bar
    const char *name;
};

const Mockup kIphone{":/phone-mockup/iphone-mockup.png", {1022, 2082}, {52, 46, 918, 1990}, 130, {5, 0, 1012, 2082}, 185,
                     {364, 72, 294, 86}, {393, 852}, 59, 34, QT_TRANSLATE_NOOP("WebPreview", "iPhone 15")};
const Mockup kPixel{":/phone-mockup/pixel-9-pro-mockup.svg", {353, 745}, {11.7227, 12.7058, 326, 722}, 55, {2.7, 3.7, 344, 740}, 64,
                    {164, 31, 22, 22}, {412, 915}, 52, 24, QT_TRANSLATE_NOOP("WebPreview", "Pixel 9 Pro")};

// The mockup at `px` device pixels. The Pixel SVG punches its screen out with a <mask>, which Qt's SVG renderer
// (Tiny 1.2) draws as plain shapes, a white sheet behind the phone: it is cut out, the screen stays black like the
// iPhone's.
QImage renderMockup(const Mockup &m, const QSize &px)
{
    const QString resource = QString::fromLatin1(m.resource);
    if (!resource.endsWith(QLatin1String(".svg"))) {
        static QHash<QString, QImage> sources;
        QImage &src = sources[resource];
        if (src.isNull())
            src = QImage(resource);
        return src.scaled(px, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    static QHash<QString, QByteArray> svgs;
    QByteArray &svg = svgs[resource];
    if (svg.isEmpty()) {
        QFile f(resource);
        if (f.open(QIODevice::ReadOnly)) {
            QString text = QString::fromUtf8(f.readAll());
            text.remove(QRegularExpression(QStringLiteral("<mask\\b.*?</mask>"), QRegularExpression::DotMatchesEverythingOption));
            text.remove(QRegularExpression(QStringLiteral("\\smask=\"[^\"]*\"")));
            svg = text.toUtf8();
        }
    }
    QBuffer buffer(&svg);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "svg");
    reader.setScaledSize(px);
    return reader.read();
}

// The tablet: a rounded body in the theme's colours with a front camera in the top bezel.
QImage drawnDevice(const QSize &size, int bezel, qreal radius, const QColor &body, const QColor &outline, qreal dpr)
{
    QImage img(size * dpr, QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(outline, 1));
    p.setBrush(body);
    p.drawRoundedRect(QRectF(0.5, 0.5, size.width() - 1, size.height() - 1), radius + bezel, radius + bezel);
    p.setPen(Qt::NoPen);
    p.setBrush(outline);
    const qreal r = qMax(1.5, bezel * 0.16);
    p.drawEllipse(QPointF(size.width() / 2.0, bezel / 2.0), r, r);
    return img;
}

// Hides the page's scrollbars in phone / tablet mode, as on a touch screen (scrolling still works).
// React Navigation's bottom tabs give a label below the icon only 10px (49px bar, 28px icon, padding); React Native
// Web clips its single-line text to that box, cutting off the bottom of the letters, where iOS draws the overflow.
// The second rule lets the glyphs spill past the box like on a device; a long label still ends in an ellipsis.
[[maybe_unused]] const char *kDeviceStyleJs = "(function(on){var id='__qode_device',s=document.getElementById(id);"
                             "if(!on){if(s)s.remove();return;}"
                             "if(!s){s=document.createElement('style');s.id=id;"
                             "s.textContent='::-webkit-scrollbar{display:none!important}"
                             "[role=tablist] [dir=auto][class*=css-text-]{overflow:clip!important;overflow-clip-margin:4px}';"
                             "(document.head||document.documentElement).appendChild(s);}})(%1)";

// Gives an Expo app the mockup's safe-area insets. A browser reports env(safe-area-inset-*) as 0 and a page cannot
// change that, so react-native-safe-area-context (its web provider measures a hidden element padded with
// env(safe-area-inset-*) through getComputedStyle) would lay the app out under the camera and the home indicator.
// getComputedStyle answers for such inline styles as if env() held the mockup's insets; a later call (iOS <-> Android)
// updates them and fires the transition event the provider listens to, so it measures again without a reload.
// Runs in the page's own world, before the app's scripts.
[[maybe_unused]] const char *kSafeAreaJs = R"((function(i){var w=window;if(w.__qodeSafeArea){w.__qodeSafeArea.set(i);return;}
var cs=w.getComputedStyle,insets=i,seen=new Set(),re=/(?:env|constant)\(\s*safe-area-inset-(top|right|bottom|left)\s*(?:,[^)]*)?\)/g;
w.getComputedStyle=function(el,pseudo){var real=cs.call(w,el,pseudo),st=el&&el.getAttribute&&el.getAttribute('style');
if(!st||st.indexOf('safe-area-inset')<0)return real;seen.add(el);
return new Proxy(real,{get:function(t,k){var v=Reflect.get(t,k);if(typeof v==='function')return v.bind(t);
var own=typeof k==='string'?el.style[k]:null;if(typeof own!=='string'||own.indexOf('safe-area-inset')<0)return v;
var p=document.createElement('div');p.style.position='fixed';p.style.visibility='hidden';
p.style[k]=own.replace(re,function(m,side){return(insets[side]||0)+'px';});
(document.body||document.documentElement).appendChild(p);var r=cs.call(w,p)[k];p.remove();return r;}});};
w.__qodeSafeArea={set:function(n){insets=n;seen.forEach(function(el){if(!el.isConnected){seen.delete(el);return;}
['webkitTransitionEnd','transitionend'].forEach(function(e){el.dispatchEvent(new Event(e));});});}};})(%1))";

} // namespace

// What the page sits on. In a device mode it draws the device around the page: the phone mockup or the tablet with a
// soft shadow, a thin outline for desktop.
class DeviceStage : public QWidget
{
public:
    using QWidget::QWidget;
    QPixmap device; // drawn at devicePos, under the page
    QPoint devicePos;
    QString deviceKey; // what `device` was rendered for, so a relayout at the same size reuses it
    QRectF hull;       // outline of the device, casts the shadow
    qreal hullRadius = 0;
    QRect outlined; // desktop: a thin line around the page
    QColor backdrop, body, outline, shadow;
    QPixmap scene; // all of the above as painted, which the patches over the page copy from

    void rebuild()
    {
        const qreal dpr = devicePixelRatioF();
        scene = QPixmap(size() * dpr);
        scene.setDevicePixelRatio(dpr);
        QPainter p(&scene);
        p.fillRect(rect(), backdrop);
        p.setRenderHint(QPainter::Antialiasing);
        if (!hull.isEmpty()) {
            QColor c = shadow;
            c.setAlphaF(0.05f);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            for (int i = 1; i <= 8; ++i)
                p.drawRoundedRect(hull.adjusted(-i, -i + 4, i, i + 4), hullRadius + i, hullRadius + i);
        }
        if (!device.isNull())
            p.drawPixmap(devicePos, device);
        if (!outlined.isEmpty()) {
            p.setPen(QPen(outline, 1));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRectF(outlined).adjusted(-0.5, -0.5, 0.5, 0.5), 3, 3);
        }
        p.end();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override { QPainter(this).drawPixmap(0, 0, scene); }
};

namespace {

// A piece of the stage drawn over the page (a rounded corner of the screen, the camera), so the page looks like it
// sits inside the device. The whole stage, not only the device: a screen's square corner can reach past the phone's
// rounder outline. Sits above the page and lets the mouse through.
class DevicePatch : public QWidget
{
public:
    explicit DevicePatch(DeviceStage *stage) : QWidget(stage), m_stage(stage)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

    // `shape` in stage coordinates; empty hides the patch.
    void setShape(const QPainterPath &shape)
    {
        if (shape.isEmpty()) {
            hide();
            return;
        }
        const QRect r = shape.boundingRect().toAlignedRect();
        m_shape = shape.translated(-r.topLeft());
        setGeometry(r);
        show();
        raise();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        // Antialiased edges: the shape as an alpha mask, the stage composed into it.
        const qreal dpr = devicePixelRatioF();
        QImage img(size() * dpr, QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(dpr);
        img.fill(Qt::transparent);
        QPainter q(&img);
        q.setRenderHint(QPainter::Antialiasing);
        q.fillPath(m_shape, Qt::black);
        q.setCompositionMode(QPainter::CompositionMode_SourceIn);
        q.drawPixmap(-pos(), m_stage->scene);
        q.end();
        QPainter(this).drawImage(0, 0, img);
    }

private:
    DeviceStage *m_stage;
    QPainterPath m_shape;
};

// A two-way switch shaped as a pill: the selected label sits on an accent thumb that slides across.
class SegmentPill : public QWidget
{
public:
    SegmentPill(const QStringList &labels, QWidget *parent) : QWidget(parent), m_labels(labels)
    {
        setFocusPolicy(Qt::TabFocus);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
        QFont f = font();
        f.setWeight(QFont::DemiBold);
        setFont(f);
        m_anim = new QVariantAnimation(this);
        m_anim->setDuration(180);
        m_anim->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            m_pos = v.toReal();
            update();
        });
    }

    std::function<void(int)> onChanged;
    QColor track, border, thumb, text, textHover, textActive, focus;

    int current() const { return m_current; }
    void setCurrent(int i)
    {
        if (i == m_current || i < 0 || i >= m_labels.size())
            return;
        m_current = i;
        m_anim->stop();
        m_anim->setStartValue(m_pos);
        m_anim->setEndValue(qreal(i));
        m_anim->start();
        if (onChanged)
            onChanged(i);
    }

    QSize sizeHint() const override
    {
        int w = 0;
        for (const QString &l : m_labels)
            w = qMax(w, fontMetrics().horizontalAdvance(l));
        return {int(m_labels.size()) * (w + 30) + 2 * kPad, fontMetrics().height() + 14};
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(hasFocus() ? focus : border, 1));
        p.setBrush(track);
        p.drawRoundedRect(r, r.height() / 2, r.height() / 2);

        const QRectF t = segment(m_pos);
        QColor glow = thumb;
        glow.setAlphaF(0.25);
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawRoundedRect(t.adjusted(-1, 0, 1, 2), t.height() / 2 + 1, t.height() / 2 + 1);
        p.setBrush(thumb);
        p.drawRoundedRect(t, t.height() / 2, t.height() / 2);

        for (int i = 0; i < m_labels.size(); ++i) {
            // The label colour follows the thumb, so it fades over while sliding.
            const qreal on = qBound(0.0, 1.0 - qAbs(m_pos - i), 1.0);
            const QColor off = i == m_hover ? textHover : text;
            p.setPen(QColor::fromRgbF(off.redF() + (textActive.redF() - off.redF()) * on, off.greenF() + (textActive.greenF() - off.greenF()) * on,
                                      off.blueF() + (textActive.blueF() - off.blueF()) * on));
            p.drawText(segment(i), Qt::AlignCenter, m_labels.at(i));
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton)
            setCurrent(indexAt(e->position()));
    }
    void mouseMoveEvent(QMouseEvent *e) override
    {
        const int h = indexAt(e->position());
        if (h != m_hover) {
            m_hover = h;
            update();
        }
    }
    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        switch (e->key()) {
        case Qt::Key_Left:
            setCurrent(m_current - 1);
            break;
        case Qt::Key_Right:
            setCurrent(m_current + 1);
            break;
        case Qt::Key_Space:
            setCurrent((m_current + 1) % int(m_labels.size()));
            break;
        default:
            QWidget::keyPressEvent(e);
        }
    }
    void focusInEvent(QFocusEvent *e) override
    {
        QWidget::focusInEvent(e);
        update();
    }
    void focusOutEvent(QFocusEvent *e) override
    {
        QWidget::focusOutEvent(e);
        update();
    }

private:
    static constexpr int kPad = 3;
    QRectF segment(qreal i) const
    {
        const qreal w = (width() - 2.0 * kPad) / m_labels.size();
        return {kPad + i * w, qreal(kPad), w, height() - 2.0 * kPad};
    }
    int indexAt(const QPointF &pt) const { return qBound(0, int((pt.x() - kPad) / ((width() - 2.0 * kPad) / m_labels.size())), int(m_labels.size()) - 1); }

    QStringList m_labels;
    int m_current = 0, m_hover = -1;
    qreal m_pos = 0; // the thumb's position in segments, animated
    QVariantAnimation *m_anim;
};

// "/friend/[id]" as a regular expression for the URL path it stands for.
QRegularExpression routePattern(const QString &route)
{
    QString re;
    for (const QString &seg : route.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (seg.startsWith(QLatin1String("[[...")))
            re += QStringLiteral("(?:/.*)?");
        else if (seg.startsWith(QLatin1String("[...")))
            re += QStringLiteral("/.+");
        else if (seg.startsWith(QLatin1Char('[')))
            re += QStringLiteral("/[^/]+");
        else
            re += QLatin1Char('/') + QRegularExpression::escape(seg);
    }
    return QRegularExpression(QLatin1Char('^') + (re.isEmpty() ? QStringLiteral("/?") : re + QStringLiteral("/?")) + QLatin1Char('$'));
}

bool isLocalHost(const QString &host)
{
    return host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") || host == QLatin1String("::1")
           || host == QLatin1String("0.0.0.0") || host.endsWith(QLatin1String(".localhost"));
}

} // namespace

#ifdef QODE_HAS_WEBENGINE
// Collects the page's console and keeps the preview on the dev server: links to other sites and new tabs
// (target="_blank", a plain window.open) go to the system browser. A sized popup (window.open with features, which is
// how Firebase / Google / OAuth sign-in opens) stays in QODE in its own window on the same profile: the page that
// opened it waits for it through window.opener / postMessage and its cookies, which a system browser would never give.
class PreviewPage : public QWebEnginePage
{
public:
    using QWebEnginePage::QWebEnginePage;
    ~PreviewPage() override
    {
        // Popups' pages must go before the profile, which the preview deletes right after this page.
        for (const QPointer<QWebEngineView> &v : std::as_const(m_popups))
            delete v.data();
    }
    std::function<void(const WebPreview::ConsoleEntry &)> onConsole;

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level, const QString &message, int line, const QString &source) override
    {
        if (onConsole)
            onConsole({level == ErrorMessageLevel ? 2 : level == WarningMessageLevel ? 1 : 0, message, source, line});
    }

    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override
    {
        if (isMainFrame && type == NavigationTypeLinkClicked && url.scheme().startsWith(QLatin1String("http")) && !isLocalHost(url.host())) {
            QDesktopServices::openUrl(url);
            return false;
        }
        return true;
    }

    QWebEnginePage *createWindow(WebWindowType type) override
    {
        if (type == WebDialog)
            return openPopup();
        auto *page = new QWebEnginePage(profile(), this);
        connect(page, &QWebEnginePage::urlChanged, page, [page](const QUrl &url) {
            if (url.isEmpty())
                return;
            QDesktopServices::openUrl(url);
            page->deleteLater();
        });
        return page;
    }

private:
    QWebEnginePage *openPopup()
    {
        auto *view = new QWebEngineView;
        view->setAttribute(Qt::WA_DeleteOnClose);
        view->resize(520, 640);
        auto *page = new QWebEnginePage(profile(), view);
        view->setPage(page);
        connect(page, &QWebEnginePage::titleChanged, view, &QWidget::setWindowTitle);
        connect(page, &QWebEnginePage::windowCloseRequested, view, &QWidget::close);
        connect(page, &QWebEnginePage::geometryChangeRequested, view, [view](const QRect &r) {
            if (r.width() > 0 && r.height() > 0)
                view->resize(r.size());
        });
        m_popups.append(view);
        view->show();
        return page;
    }

    QList<QPointer<QWebEngineView>> m_popups;
};
#endif

WebPreview::WebPreview(QWidget *parent) : QWidget(parent)
{
    auto button = [this](const QString &icon, const QString &tip, bool checkable = false) {
        auto *b = new QToolButton(this);
        b->setAutoRaise(true);
        b->setCheckable(checkable);
        b->setToolTip(tip);
        Icons::bind(b, icon);
        return b;
    };

    m_icon = new QLabel(this);
    m_icon->setFixedSize(16, 16);
    m_title = new QLabel(tr("PREVIEW"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_title->setTextInteractionFlags(Qt::NoTextInteraction);
    m_targetMenu = new QMenu(this);
    m_targetBtn = new QToolButton(this);
    m_targetBtn->setAutoRaise(true);
    m_targetBtn->setPopupMode(QToolButton::InstantPopup);
    m_targetBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_targetBtn->setMenu(m_targetMenu);
    m_targetBtn->setToolTip(tr("What the preview shows: the web app or an Expo app"));
    m_targetBtn->hide();
    m_fit = button(QStringLiteral(":/new-icons/panel-right.svg"), tr("Fit the panel (the page's own responsive layout)"), true);
    m_phone = button(QStringLiteral(":/new-icons/smartphone.svg"), tr("Phone (iPhone or Android, scaled to fit)"), true);
    m_tablet = button(QStringLiteral(":/new-icons/tablet.svg"), tr("Tablet (820 × 1180, scaled to fit)"), true);
    m_desktop = button(QStringLiteral(":/new-icons/monitor.svg"), tr("Desktop (1440 wide, scaled to fit)"), true);
    for (QToolButton *b : {m_fit, m_phone, m_tablet, m_desktop})
        b->setAutoExclusive(true);
    m_fit->setChecked(true);
    m_rotate = button(QStringLiteral(":/new-icons/rotate-ccw.svg"), tr("Turn the device sideways"), true);
    m_rotate->setEnabled(false);
    m_devTools = button(QStringLiteral(":/new-icons/code-xml.svg"), tr("Developer tools (inspect elements, network, console)"));
    m_external = button(QStringLiteral(":/new-icons/external-link.svg"), tr("Open this page in the browser"));
    m_closeBtn = button(QStringLiteral(":/new-icons/x.svg"), tr("Close preview"));

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 0, 4, 0);
    hl->setSpacing(2);
    hl->addWidget(m_icon);
    hl->addSpacing(4);
    hl->addWidget(m_title);
    hl->addSpacing(4);
    hl->addWidget(m_targetBtn);
    hl->addStretch(1);
    hl->addWidget(m_fit);
    hl->addWidget(m_phone);
    hl->addWidget(m_tablet);
    hl->addWidget(m_desktop);
    hl->addWidget(m_rotate);
    hl->addSpacing(6);
    hl->addWidget(m_devTools);
    hl->addWidget(m_external);
    hl->addWidget(m_closeBtn);
    header->setStyleSheet(QStringLiteral("QWidget#mediaHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#mediaHeader QLabel { background: transparent; border: none; }"));

    m_back = button(QStringLiteral(":/new-icons/arrow-left.svg"), tr("Back"));
    m_forward = button(QStringLiteral(":/new-icons/arrow-right.svg"), tr("Forward"));
    m_reload = button(QStringLiteral(":/new-icons/refresh-cw.svg"), tr("Reload the page"));
    m_routesMenu = new QMenu(this);
    m_routesBtn = button(QStringLiteral(":/new-icons/route.svg"), tr("Go to a screen of the app"));
    m_routesBtn->setPopupMode(QToolButton::InstantPopup);
    m_routesBtn->setMenu(m_routesMenu);
    m_routesBtn->hide();
    m_address = new QLineEdit(this);
    m_address->setPlaceholderText(tr("/route or full address"));
    m_address->setToolTip(tr("Type a route (/about) or an address and press Enter"));
    m_routeModel = new QStringListModel(this);
    auto *completer = new QCompleter(m_routeModel, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    m_address->setCompleter(completer);
    // A picked route with [params] asks for their values instead of loading the brackets.
    // Enter in its popup also reaches the address bar's returnPressed: only one of them goes.
    connect(completer, QOverload<const QString &>::of(&QCompleter::activated), this, [this](const QString &text) {
        m_routePicked = true;
        QTimer::singleShot(0, this, [this, text] {
            m_routePicked = false;
            navigate(text);
        });
    });
    auto *nav = new QWidget(this);
    auto *nl = new QHBoxLayout(nav);
    nl->setContentsMargins(6, 4, 6, 4);
    nl->setSpacing(2);
    nl->addWidget(m_back);
    nl->addWidget(m_forward);
    nl->addWidget(m_reload);
    nl->addSpacing(4);
    nl->addWidget(m_routesBtn);
    nl->addWidget(m_address, 1);

    m_progress = new QProgressBar(this);
    m_progress->setFixedHeight(2);
    m_progress->setTextVisible(false);
    m_progress->setRange(0, 100);
    m_progress->hide();

    m_notice = new QWidget(this);
    m_notice->setObjectName(QStringLiteral("previewNotice"));
    m_noticeIcon = new QLabel(m_notice);
    m_noticeIcon->setFixedSize(16, 16);
    m_noticeText = new QLabel(m_notice);
    m_noticeText->setWordWrap(true);
    m_noticeText->setTextFormat(Qt::RichText);
    m_noticeText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *noticeBrowser = new QPushButton(tr("Open in Browser"), m_notice);
    noticeBrowser->setToolTip(tr("Open the page in the system browser, a newer Chromium or Firefox than the preview's engine"));
    auto *noticeConsole = new QPushButton(tr("Console"), m_notice);
    noticeConsole->setToolTip(tr("Show the page's console with the whole error"));
    auto *noticeClose = button(QStringLiteral(":/new-icons/x.svg"), tr("Dismiss"));
    noticeClose->setParent(m_notice);
    // The text gets the panel's whole width; the buttons go under it.
    auto *nol = new QGridLayout(m_notice);
    nol->setContentsMargins(10, 8, 6, 8);
    nol->setHorizontalSpacing(8);
    nol->setVerticalSpacing(6);
    nol->addWidget(m_noticeIcon, 0, 0, Qt::AlignTop);
    nol->addWidget(m_noticeText, 0, 1);
    nol->addWidget(noticeClose, 0, 2, Qt::AlignTop);
    auto *noticeButtons = new QHBoxLayout;
    noticeButtons->addWidget(noticeBrowser);
    noticeButtons->addWidget(noticeConsole);
    noticeButtons->addStretch(1);
    nol->addLayout(noticeButtons, 1, 1);
    nol->setColumnStretch(1, 1);
    m_notice->hide();
    connect(noticeConsole, &QPushButton::clicked, this, [this] { m_consoleBtn->setChecked(true); });
    connect(noticeBrowser, &QPushButton::clicked, m_external, &QToolButton::click);
    connect(noticeClose, &QToolButton::clicked, m_notice, &QWidget::hide);
    m_crashCheck = new QTimer(this);
    m_crashCheck->setSingleShot(true);
    m_crashCheck->setInterval(1500);
    connect(m_crashCheck, &QTimer::timeout, this, &WebPreview::checkCrash);

    m_placeholder = new QLabel(this);
    m_placeholder->setObjectName(QStringLiteral("emptyText"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setWordWrap(true);
    m_placeholder->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_startBtn = new QPushButton(tr("Start Dev Server"), this);
    m_logBtn = new QPushButton(tr("Show Log"), this);
    m_fixBtn = new QPushButton(tr("Add Web Support"), this);
    m_fixBtn->setToolTip(tr("Runs npx expo install react-native-web react-dom @expo/metro-runtime in the terminal, which adds them to "
                            "package.json. Code that uses native-only modules may still need changes before the app runs in a browser."));
    m_fixBtn->hide();
    m_choices = new QWidget(this);
    auto *cl = new QVBoxLayout(m_choices);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(6);
    m_choices->hide();
    auto *empty = new QWidget(this);
    auto *el = new QVBoxLayout(empty);
    el->setContentsMargins(24, 24, 24, 24);
    el->addStretch(1);
    el->addWidget(m_placeholder);
    el->addSpacing(10);
    auto *btns = new QHBoxLayout;
    btns->addStretch(1);
    btns->addWidget(m_startBtn);
    btns->addWidget(m_fixBtn);
    btns->addWidget(m_logBtn);
    btns->addStretch(1);
    el->addLayout(btns);
    auto *choiceRow = new QHBoxLayout;
    choiceRow->addStretch(1);
    choiceRow->addWidget(m_choices);
    choiceRow->addStretch(1);
    el->addLayout(choiceRow);
    el->addStretch(2);

    m_viewport = new DeviceStage(this);
    m_viewport->installEventFilter(this);
    for (QWidget *&patch : m_patches)
        patch = new DevicePatch(m_viewport);
    auto *os = new SegmentPill({tr("iOS"), tr("Android")}, m_viewport);
    os->setToolTip(tr("Show it on an iPhone or an Android phone"));
    os->hide();
    os->onChanged = [this](int i) {
        m_android = i == 1;
        layoutView();
        applyDeviceStyle();
    };
    m_osToggle = os;
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(empty);
    m_stack->addWidget(m_viewport);

    m_consoleView = new QPlainTextEdit(this);
    m_consoleView->setReadOnly(true);
    m_consoleView->setMaximumBlockCount(1000);
    m_consoleView->setFixedHeight(140);
    m_consoleView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_consoleView->setPlaceholderText(tr("The page's console output appears here."));
    m_consoleView->hide();

    m_sizeLabel = new QLabel(this);
    m_sizeLabel->setObjectName(QStringLiteral("emptyText"));
    m_consoleBtn = new QToolButton(this);
    m_consoleBtn->setAutoRaise(true);
    m_consoleBtn->setCheckable(true);
    m_consoleBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_consoleBtn->setToolTip(tr("Show the page's console (errors, warnings, logs)"));
    Icons::bind(m_consoleBtn, QStringLiteral(":/new-icons/scroll-text.svg"));
    auto *status = new QWidget(this);
    status->setObjectName(QStringLiteral("previewStatus"));
    auto *sl = new QHBoxLayout(status);
    sl->setContentsMargins(10, 1, 4, 1);
    sl->addWidget(m_sizeLabel, 1);
    sl->addWidget(m_consoleBtn);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(nav);
    layout->addWidget(m_progress);
    layout->addWidget(m_notice);
    layout->addWidget(m_stack, 1);
    layout->addWidget(m_consoleView);
    layout->addWidget(status);

    connect(m_closeBtn, &QToolButton::clicked, this, &WebPreview::closeRequested);
    connect(m_startBtn, &QPushButton::clicked, this, [this] { emit startRequested(m_target); });
    connect(m_logBtn, &QPushButton::clicked, this, [this] { emit logRequested(m_target); });
    connect(m_fixBtn, &QPushButton::clicked, this, [this] { emit fixRequested(m_target); });
    connect(m_fit, &QToolButton::clicked, this, [this] { setDevice(Device::Fit); });
    connect(m_phone, &QToolButton::clicked, this, [this] { setDevice(Device::Phone); });
    connect(m_tablet, &QToolButton::clicked, this, [this] { setDevice(Device::Tablet); });
    connect(m_desktop, &QToolButton::clicked, this, [this] { setDevice(Device::Desktop); });
    connect(m_rotate, &QToolButton::toggled, this, [this](bool on) {
        m_landscape = on;
        layoutView();
    });
    connect(m_external, &QToolButton::clicked, this, [this] {
        const QUrl url = currentUrl();
        if (url.isValid())
            QDesktopServices::openUrl(url);
    });
    connect(m_devTools, &QToolButton::clicked, this, &WebPreview::openDevTools);
    connect(m_address, &QLineEdit::returnPressed, this, [this] {
        if (!m_routePicked)
            navigate(m_address->text());
    });
    connect(m_routesMenu, &QMenu::aboutToShow, this, [this] {
        refreshRoutes();
        rebuildRoutesMenu();
    });
    connect(m_consoleBtn, &QToolButton::toggled, m_consoleView, &QWidget::setVisible);
#ifdef QODE_HAS_WEBENGINE
    connect(m_back, &QToolButton::clicked, this, [this] {
        if (m_view)
            m_view->back();
    });
    connect(m_forward, &QToolButton::clicked, this, [this] {
        if (m_view)
            m_view->forward();
    });
    connect(m_reload, &QToolButton::clicked, this, [this] {
        if (m_view && !m_view->url().isEmpty())
            m_view->reload();
        else
            loadFromServer();
    });
#endif
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &WebPreview::restyle);
    restyle();
    updateConsoleButton();
    onServerState();
}

WebPreview::~WebPreview()
{
#ifdef QODE_HAS_WEBENGINE
    // Pages must go before their profile, or WebEngine complains and may leak the profile.
    delete m_devToolsView;
    delete m_view;
    delete m_page;
    delete m_profile;
#endif
}

void WebPreview::setTargets(const QVector<Target> &targets, const QString &current)
{
    m_targets = targets;
    QString id = current.isEmpty() ? m_target : current;
    bool listed = false;
    for (const Target &t : targets)
        listed = listed || t.id == id;
    if (!listed)
        id = targets.isEmpty() ? QString() : targets.first().id;
    selectTarget(id);
}

const WebPreview::Target *WebPreview::target() const
{
    for (const Target &t : m_targets)
        if (t.id == m_target)
            return &t;
    return nullptr;
}

void WebPreview::selectTarget(const QString &id)
{
    if (id != m_target) {
        if (!m_target.isEmpty())
            m_lastUrls.insert(m_target, m_lastUrl);
        m_lastUrl = m_lastUrls.value(id);
        m_target = id;
        m_base.clear();
        m_address->clear();
        m_notice->hide();
        m_crashCheck->stop();
#ifdef QODE_HAS_WEBENGINE
        if (m_view) {
            m_view->setUrl(QUrl(QStringLiteral("about:blank"))); // the other app stops (hot reload, sockets, timers)
            m_view->history()->clear();
        }
#endif
    }
    const Target *t = target();
    m_targetBtn->setText(t ? t->name + QStringLiteral("  ▾") : QString());
    m_targetBtn->setVisible(m_targets.size() > 1);
    rebuildTargetMenu();
    applyTargetKind();
    refreshRoutes();
    attach(t ? t->server : nullptr);
}

void WebPreview::attach(DevServer *server)
{
    if (m_server != server) {
        if (m_server)
            disconnect(m_server, nullptr, this, nullptr);
        m_server = server;
        if (server) {
            connect(server, &DevServer::stateChanged, this, &WebPreview::onServerState);
            connect(server, &DevServer::buildError, this, [this](const QString &message) {
                addConsole({2, tr("Bundling failed: %1").arg(message), QString(), 0});
                m_consoleBtn->setChecked(true); // the page stays blank: say why
            });
        }
    }
    onServerState();
}

void WebPreview::rebuildTargetMenu()
{
    m_targetMenu->clear();
    for (const Target &t : std::as_const(m_targets)) {
        QString text = (t.mobile ? tr("Expo app · %1") : tr("Web app · %1")).arg(t.name);
        if (!t.where.isEmpty())
            text += QStringLiteral("  (%1)").arg(t.where);
        QAction *a = m_targetMenu->addAction(text);
        a->setCheckable(true);
        a->setChecked(t.id == m_target);
        connect(a, &QAction::triggered, this, [this, id = t.id] {
            selectTarget(id);
            emit targetChosen(id);
        });
    }
}

// An Expo app is a phone app: only the mockup (iOS / Android pill), upright. The web app's device comes back after.
void WebPreview::applyTargetKind()
{
    const Target *t = target();
    const bool mobile = t && t->mobile;
    for (QToolButton *b : {m_fit, m_phone, m_tablet, m_desktop, m_rotate})
        b->setVisible(!mobile);
    if (mobile == m_mobile)
        return;
    m_mobile = mobile;
    if (mobile) {
        m_webDevice = m_device;
        m_webLandscape = m_landscape;
        m_rotate->setChecked(false);
        m_phone->setChecked(true);
        setDevice(Device::Phone);
    } else {
        m_rotate->setChecked(m_webLandscape);
        (m_webDevice == Device::Phone ? m_phone : m_webDevice == Device::Tablet ? m_tablet : m_webDevice == Device::Desktop ? m_desktop : m_fit)
            ->setChecked(true);
        setDevice(m_webDevice);
    }
}

QUrl WebPreview::currentUrl() const
{
#ifdef QODE_HAS_WEBENGINE
    if (m_view && m_view->url().scheme().startsWith(QLatin1String("http")))
        return m_view->url();
#endif
    return m_base;
}

void WebPreview::showPlaceholder(const QString &text, bool start, bool log)
{
    m_placeholder->setText(text);
    m_startBtn->setVisible(start);
    m_logBtn->setVisible(log);
    m_fixBtn->hide();
    m_choices->hide();
    m_stack->setCurrentIndex(0);
    m_progress->hide();
}

void WebPreview::showChoices()
{
    showPlaceholder(tr("This project has more than one app. Which one should the preview show?"), false, false);
    QLayout *l = m_choices->layout();
    while (QLayoutItem *item = l->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    for (const Target &t : std::as_const(m_targets)) {
        const bool live = t.server && t.server->isActive();
        QString text = t.mobile ? (live ? tr("Show Expo app · %1") : tr("Run Expo app · %1")).arg(t.name)
                                : (live ? tr("Show web app · %1") : tr("Run web app · %1")).arg(t.name);
        if (!t.where.isEmpty())
            text += QStringLiteral("  (%1)").arg(t.where);
        auto *b = new QPushButton(Icons::tinted(t.mobile ? QStringLiteral(":/new-icons/smartphone.svg") : QStringLiteral(":/new-icons/globe.svg"),
                                                theme.editorFg),
                                  text, m_choices);
        b->setToolTip(t.mobile ? tr("Runs the app for web with Expo and shows it in an iPhone or Android mockup")
                               : tr("Runs the dev server and shows the page"));
        connect(b, &QPushButton::clicked, this, [this, id = t.id, start = !live && t.problem.isEmpty()] {
            selectTarget(id);
            emit targetChosen(id);
            if (start)
                emit startRequested(id);
        });
        l->addWidget(b);
    }
    m_choices->show();
}

void WebPreview::onServerState()
{
    using S = DevServer::State;
    const Target *t = target();
    const S state = m_server ? m_server->state() : S::Stopped;
    const bool running = state == S::Running && m_server->port() > 0;
    for (QWidget *w : {static_cast<QWidget *>(m_back), static_cast<QWidget *>(m_forward), static_cast<QWidget *>(m_reload),
                       static_cast<QWidget *>(m_address), static_cast<QWidget *>(m_external), static_cast<QWidget *>(m_devTools)})
        w->setEnabled(running);
    m_routesBtn->setVisible(running && t && t->routes);
    const bool mobile = t && t->mobile;
    m_startBtn->setText(mobile ? tr("Start Expo for Web") : tr("Start Dev Server"));
    if (!t) {
        showPlaceholder(tr("Open a web project with a dev script (dev, start, serve, …) or an Expo app to preview it here."), false, false);
        return;
    }
    if (!t->problem.isEmpty()) {
        showPlaceholder(t->problem, false, false);
        m_fixBtn->show();
        return;
    }
    switch (state) {
    case S::Stopped:
        if (m_targets.size() > 1) {
            showChoices();
            return;
        }
        showPlaceholder(mobile ? tr("%1 is not running for web.\nStart it to see the app in an iPhone or Android mockup; it updates live as "
                                    "files change.")
                                     .arg(t->name)
                               : tr("The dev server is not running.\nStart it to see the page here; it updates live as files change."),
                        true, false);
        return;
    case S::Starting:
        showPlaceholder(mobile ? tr("Starting Metro for %1…\nThe first bundle can take a minute. The preview shows the app's web version, "
                                    "run with react-native-web.")
                                     .arg(t->name)
                               : tr("Starting the dev server…"),
                        false, true);
        return;
    case S::Failed:
        showPlaceholder((mobile ? tr("Metro stopped.\n%1") : tr("The dev server failed.\n%1")).arg(m_server->errorString()), true, true);
        m_startBtn->setText(tr("Start Again"));
        return;
    case S::Running:
        break;
    }
    if (!running) {
        showPlaceholder(tr("The dev server is running, but it did not print the address it listens on."), false, true);
        return;
    }
    const QUrl base(m_server->url() + QLatin1Char('/'));
    if (base == m_base && m_stack->currentIndex() == 1)
        return; // already showing it (the targets were only refreshed)
    m_base = base;
#ifdef QODE_HAS_WEBENGINE
    if (!isVisible()) {
        m_pendingLoad = true; // no Chromium is started for a panel nobody looks at
        return;
    }
    loadFromServer();
#else
    showPlaceholder(tr("Running on %1.\nThe live preview needs Qt WebEngine: install qt6-webengine-dev and rebuild QODE. "
                       "Until then the page opens in the browser.")
                        .arg(m_server->url()),
                    false, false);
    m_address->setText(m_base.toString());
#endif
}

void WebPreview::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (m_pendingLoad && m_server && m_server->state() == DevServer::State::Running)
        loadFromServer();
}

void WebPreview::ensureView()
{
#ifdef QODE_HAS_WEBENGINE
    if (m_view)
        return;
    // Own profile so the previewed app's cookies and localStorage (logins, settings) survive restarts, kept apart
    // from anything else; the HTTP cache stays in memory so a dev server's files are never served stale from disk.
    m_profile = new QWebEngineProfile(QStringLiteral("preview"));
    m_profile->setPersistentStoragePath(AppPaths::data(QStringLiteral("web-preview")));
    m_profile->setCachePath(AppPaths::cache(QStringLiteral("web-preview")));
    m_profile->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
    m_page = new PreviewPage(m_profile);
    m_page->onConsole = [this](const ConsoleEntry &e) {
        addConsole(e);
        // An uncaught error (React reports the component it tore down) may have left the page blank: look shortly.
        if (e.level == 2 && (e.message.startsWith(QLatin1String("Uncaught")) || e.message.contains(QLatin1String("An error occurred in the <")))) {
            if (m_firstError.isEmpty() || (e.message.startsWith(QLatin1String("Uncaught")) && !m_firstError.startsWith(QLatin1String("Uncaught"))))
                m_firstError = e.message;
            if (!m_notice->isVisible())
                m_crashCheck->start();
        }
    };
    m_view = new QWebEngineView(m_viewport);
    m_view->setPage(m_page);
    connect(m_view, &QWebEngineView::urlChanged, this, [this](const QUrl &url) {
        if (!url.scheme().startsWith(QLatin1String("http")))
            return; // about:blank after switching targets
        if (!m_address->hasFocus())
            m_address->setText(url.toString());
        m_lastUrl = url.toString();
        updateNav();
    });
    connect(m_view, &QWebEngineView::loadStarted, this, [this] {
        m_loading = true;
        m_console.clear();
        m_consoleView->clear();
        m_errors = m_warnings = 0;
        m_firstError.clear();
        m_crashCheck->stop();
        m_notice->hide();
        updateConsoleButton();
        m_progress->setValue(5);
        m_progress->show();
    });
    connect(m_view, &QWebEngineView::loadProgress, m_progress, &QProgressBar::setValue);
    connect(m_view, &QWebEngineView::loadFinished, this, [this] {
        m_loading = false;
        ++m_loadsFinished;
        m_progress->hide();
        updateNav();
        // Known before anything fails: the app needs a newer engine than this Qt's (Expo's error overlay follows).
        const Target *t = target();
        const int engine = QString::fromLatin1(qWebEngineChromiumVersion()).section(QLatin1Char('.'), 0, 0).toInt();
        if (t && engine > 0 && t->minChromium > engine && m_view->url().scheme().startsWith(QLatin1String("http"))) {
            m_noticeText->setText(tr("<b>%1 uses %2, whose web build needs Chromium %3+.</b> The preview's engine (Qt %4) is "
                                     "Chromium %5, so the app stops here. It runs in the browser, or in QODE built with a newer Qt.")
                                      .arg(t->name.toHtmlEscaped(), t->minChromiumWhy.toHtmlEscaped())
                                      .arg(t->minChromium)
                                      .arg(QLatin1String(qVersion()))
                                      .arg(engine));
            m_notice->show();
        }
    });
    connect(m_page, &QWebEnginePage::renderProcessTerminated, this, [this](QWebEnginePage::RenderProcessTerminationStatus status) {
        m_loading = false;
        if (status == QWebEnginePage::NormalTerminationStatus)
            return;
        addConsole({2, tr("The page's renderer process crashed; reload to try again."), QString(), 0});
    });
    m_view->show();
    layoutView();
    applyDeviceStyle();
#endif
}

void WebPreview::loadFromServer()
{
    m_pendingLoad = false;
    if (m_base.isEmpty())
        return;
#ifdef QODE_HAS_WEBENGINE
    ensureView();
    m_stack->setCurrentIndex(1);
    // A restart on the same port comes back to the route that was open; another port starts at its root.
    const QUrl last(m_lastUrl);
    const QUrl target = !m_lastUrl.isEmpty() && last.host() == m_base.host() && last.port() == m_base.port() ? last : m_base;
    m_view->load(target);
    m_address->setText(target.toString());
#endif
}

void WebPreview::navigate(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return;
    if (t.startsWith(QLatin1Char('/')) && t.contains(QLatin1Char('['))) {
        openRoute(t); // a route as listed: ask for its [params]
        return;
    }
    QUrl url;
    if (t.contains(QLatin1String("://")))
        url = QUrl(t);
    else if (t.startsWith(QLatin1Char('/')) || t.startsWith(QLatin1Char('?')) || t.startsWith(QLatin1Char('#')))
        url = m_base.resolved(QUrl(t));
    else if (isLocalHost(t.section(QLatin1Char('/'), 0, 0).section(QLatin1Char(':'), 0, 0)))
        url = QUrl(QStringLiteral("http://") + t);
    else
        url = m_base.resolved(QUrl(QLatin1Char('/') + t));
#ifdef QODE_HAS_WEBENGINE
    if (!url.isValid())
        return;
    ensureView();
    m_stack->setCurrentIndex(1);
    m_view->load(url);
    m_view->setFocus();
#else
    QDesktopServices::openUrl(url);
#endif
}

void WebPreview::refreshRoutes()
{
    const Target *t = target();
    m_routes = t && t->routes ? t->routes() : QVector<Route>();
    QStringList paths;
    for (const Route &r : std::as_const(m_routes))
        paths << r.path;
    m_routeModel->setStringList(paths);
}

// Routes in a folder with several screens ("/settings/…") go into a submenu; the open one is checked.
void WebPreview::rebuildRoutesMenu()
{
    m_routesMenu->clear();
    const Target *t = target();
    if (m_routes.isEmpty()) {
        m_routesMenu->addAction(tr("No screens found in %1/").arg(t ? t->routesDir : QString()))->setEnabled(false);
        return;
    }
    m_routesMenu->addAction(tr("Screens in %1/").arg(t ? t->routesDir : QString()))->setEnabled(false); // addSection has no text in Fusion
    m_routesMenu->addSeparator();
    QString current;
#ifdef QODE_HAS_WEBENGINE
    if (m_view && m_view->url().scheme().startsWith(QLatin1String("http")))
        current = m_view->url().path();
#endif
    QHash<QString, int> perFolder;
    for (const Route &r : std::as_const(m_routes))
        ++perFolder[r.path.section(QLatin1Char('/'), 1, 1)];
    QHash<QString, QMenu *> submenus;
    for (const Route &r : std::as_const(m_routes)) {
        const QString folder = r.path.section(QLatin1Char('/'), 1, 1);
        QMenu *menu = m_routesMenu;
        if (perFolder.value(folder) > 1 && !folder.isEmpty()) {
            QMenu *&sub = submenus[folder];
            if (!sub)
                sub = m_routesMenu->addMenu(QLatin1Char('/') + folder);
            menu = sub;
        }
        const bool params = r.path.contains(QLatin1Char('['));
        QAction *a = menu->addAction(params ? r.path + QStringLiteral("…") : r.path);
        a->setToolTip(params ? tr("%1 — asks for the values of its [parameters]").arg(r.file) : r.file);
        a->setCheckable(true);
        a->setChecked(!current.isEmpty() && routePattern(r.path).match(current).hasMatch());
        connect(a, &QAction::triggered, this, [this, path = r.path] { openRoute(path); });
    }
    m_routesMenu->setToolTipsVisible(true);
    m_routesMenu->addSeparator();
    QAction *note = m_routesMenu->addAction(tr("A screen behind a login sends you to it until you sign in here"));
    note->setEnabled(false);
}

// Fills the route's [params] (the last value typed for each is offered again) and loads it.
void WebPreview::openRoute(const QString &pattern)
{
    QStringList out;
    for (const QString &seg : pattern.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (!(seg.startsWith(QLatin1Char('[')) && seg.endsWith(QLatin1Char(']')))) {
            out << seg;
            continue;
        }
        QString name = seg.mid(1, seg.size() - 2);
        const bool optional = name.startsWith(QLatin1Char('['));
        if (optional)
            name = name.mid(1, name.size() - 2);
        const bool rest = name.startsWith(QLatin1String("..."));
        if (rest)
            name = name.mid(3);
        QString label = tr("Value for <b>%1</b> in %2").arg(name.toHtmlEscaped(), pattern.toHtmlEscaped());
        if (rest)
            label += QStringLiteral("<br>") + (optional ? tr("Any number of segments separated by /, or none.") : tr("One or more segments separated by /."));
        bool ok = false;
        QString value = QInputDialog::getText(this, tr("Open %1").arg(pattern), label, QLineEdit::Normal, m_paramValues.value(name), &ok).trimmed();
        if (!ok)
            return;
        while (value.startsWith(QLatin1Char('/')))
            value.remove(0, 1);
        while (value.endsWith(QLatin1Char('/')))
            value.chop(1);
        if (value.isEmpty() && !optional)
            return;
        m_paramValues.insert(name, value);
        if (value.isEmpty())
            continue;
        const QStringList parts = rest ? value.split(QLatin1Char('/'), Qt::SkipEmptyParts) : QStringList{value};
        for (const QString &part : parts)
            out << QString::fromUtf8(QUrl::toPercentEncoding(part));
    }
    navigate(QLatin1Char('/') + out.join(QLatin1Char('/')));
}

void WebPreview::checkCrash()
{
#ifdef QODE_HAS_WEBENGINE
    if (!m_page || m_firstError.isEmpty())
        return;
    // Blank: React unmounted everything (an overlay in <body> outside the root does not count).
    m_page->runJavaScript(QStringLiteral("(function(){var r=document.getElementById('root')||document.body;"
                                         "return (r.innerText||'').trim().length+r.querySelectorAll('img,svg,canvas,input,video').length;})()"),
                          QWebEngineScript::ApplicationWorld, [this, error = m_firstError](const QVariant &v) {
                              if (v.isValid() && v.toInt() == 0 && error == m_firstError)
                                  showCrash(error);
                          });
#endif
}

void WebPreview::showCrash(const QString &message)
{
    QString error = message.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (error.startsWith(QLatin1String("Uncaught ")))
        error = error.mid(9);
    if (error.size() > 300)
        error = error.left(300) + QStringLiteral("…");
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    const Target *t = target();
    QString text;
    if (t && t->mobile)
        text = tr("<b>%1 stopped while starting in the browser:</b> <code>%2</code><br><span style=\"color:%3\">This is the app's "
                  "web version (react-native-web), not the iOS / Android build: code that only works on a device fails here, "
                  "which doesn't mean the app is broken on a phone.</span>")
                   .arg(t->name.toHtmlEscaped(), error.toHtmlEscaped(), theme.textMuted.name());
    else
        text = tr("<b>The page stopped while starting.</b><br><code>%1</code>").arg(error.toHtmlEscaped());
    m_noticeText->setText(text);
    m_notice->show();
}

void WebPreview::setDevice(Device d)
{
    m_device = d;
    m_rotate->setEnabled(d == Device::Phone || d == Device::Tablet);
    layoutView();
    applyDeviceStyle();
}

void WebPreview::applyDeviceStyle()
{
#ifdef QODE_HAS_WEBENGINE
    if (!m_page)
        return;
    const bool touch = m_device == Device::Phone || m_device == Device::Tablet;
    const QString name = QStringLiteral("qode-device");
    QWebEngineScriptCollection &scripts = m_page->scripts();
    for (const QWebEngineScript &s : scripts.find(name))
        scripts.remove(s);
    if (touch) { // every page loaded from now on
        QWebEngineScript script;
        script.setName(name);
        script.setSourceCode(QString::fromLatin1(kDeviceStyleJs).arg(QLatin1String("true")));
        script.setInjectionPoint(QWebEngineScript::DocumentReady);
        script.setWorldId(QWebEngineScript::ApplicationWorld);
        scripts.insert(script);
    }
    // and the page that is open now
    m_page->runJavaScript(QString::fromLatin1(kDeviceStyleJs).arg(QLatin1String(touch ? "true" : "false")), QWebEngineScript::ApplicationWorld);

    // An Expo app in the upright mockup gets its insets (see kSafeAreaJs); anything else keeps the browser's zeros.
    const QString safeArea = QStringLiteral("qode-safe-area");
    for (const QWebEngineScript &s : scripts.find(safeArea))
        scripts.remove(s);
    const bool insets = m_mobile && m_device == Device::Phone && !m_landscape;
    const Mockup &m = m_android ? kPixel : kIphone;
    if (insets) {
        const QString js = QString::fromLatin1(kSafeAreaJs).arg(QStringLiteral("{top:%1,bottom:%2}").arg(m.top).arg(m.bottom));
        QWebEngineScript script;
        script.setName(safeArea);
        script.setSourceCode(js);
        script.setInjectionPoint(QWebEngineScript::DocumentCreation);
        script.setWorldId(QWebEngineScript::MainWorld);
        scripts.insert(script);
        m_page->runJavaScript(js, QWebEngineScript::MainWorld);
    } else { // back to zeros where it was installed
        m_page->runJavaScript(QStringLiteral("window.__qodeSafeArea&&window.__qodeSafeArea.set({})"), QWebEngineScript::MainWorld);
    }
#endif
}

bool WebPreview::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_viewport && event->type() == QEvent::Resize)
        layoutView();
    return QWidget::eventFilter(watched, event);
}

// Fit fills the panel at 100 %. A device mode lays the page out at that device's CSS size inside the device, scaled
// down so the whole device fits the panel and centred on the backdrop; desktop is only scaled to the panel's width.
// Phone shows a mockup (iOS / Android pill above it), tablet a frame drawn in the theme's colours.
void WebPreview::layoutView()
{
    const QRect area = m_viewport->rect();
    const qreal dpr = m_viewport->devicePixelRatioF();
    const int margin = 16;
    auto *os = static_cast<SegmentPill *>(m_osToggle);
    os->setVisible(m_device == Device::Phone);
    DeviceStage *stage = m_viewport;
    stage->hull = QRectF();
    stage->outlined = QRect();
    QRect geo = area;
    qreal zoom = 1.0, radius = 0;
    QPainterPath camera;
    const char *name = nullptr;
    QSize css; // the page's CSS size when the device fixes it
    QString key;

    if (m_device == Device::Phone) {
        const Mockup &m = m_android ? kPixel : kIphone;
        name = m.name;
        const QSize pill = os->sizeHint();
        os->setGeometry(area.x() + (area.width() - pill.width()) / 2, area.y() + 12, pill.width(), pill.height());
        const int top = 12 + pill.height() + 12;
        const qreal unit = m.css.width() / m.screen.width(); // CSS px per unit of the file
        QSizeF full = m.image * unit;
        css = m.css;
        if (m_landscape) {
            full.transpose();
            css.transpose();
        }
        const qreal availW = qMax(50, area.width() - 2 * margin), availH = qMax(50, area.height() - top - margin);
        zoom = qBound(0.25, qMin(availW / full.width(), availH / full.height()), 1.0);
        const QSizeF size = full * zoom;
        const QPoint origin(area.x() + qRound((area.width() - size.width()) / 2), area.y() + top + qMax(0, qRound((availH - size.height()) / 2)));
        const qreal s = unit * zoom;
        // File units to the stage; sideways turns the phone a quarter left, camera on the left.
        QTransform xf = QTransform::fromTranslate(origin.x(), origin.y());
        if (m_landscape) {
            xf.translate(0, size.height());
            xf.rotate(-90);
        }
        xf.scale(s, s);
        geo = xf.mapRect(m.screen).toRect();
        radius = m.radius * s;
        QPainterPath cam;
        cam.addRoundedRect(m.camera, qMin(m.camera.width(), m.camera.height()) / 2, qMin(m.camera.width(), m.camera.height()) / 2);
        camera = xf.map(cam);
        stage->hull = xf.mapRect(m.body);
        stage->hullRadius = m.bodyRadius * s;
        stage->devicePos = origin;
        const QSize px = (m.image * s * dpr).toSize();
        key = QStringLiteral("%1 %2 %3x%4").arg(QLatin1String(m.resource)).arg(m_landscape).arg(px.width()).arg(px.height());
        if (stage->deviceKey != key) {
            QImage img = renderMockup(m, px);
            if (m_landscape)
                img = img.transformed(QTransform().rotate(-90));
            stage->device = QPixmap::fromImage(img);
            stage->device.setDevicePixelRatio(dpr);
        }
    } else if (m_device != Device::Fit) {
        DeviceSize dev = deviceSize(m_device);
        name = dev.name;
        if (m_landscape && dev.height > 0)
            std::swap(dev.width, dev.height);
        const int availW = qMax(50, area.width() - 2 * margin), availH = qMax(50, area.height() - 2 * margin);
        zoom = qreal(availW) / (dev.width + 2 * dev.bezel);
        if (dev.height > 0)
            zoom = qMin(zoom, qreal(availH) / (dev.height + 2 * dev.bezel));
        zoom = qBound(0.25, zoom, 1.0);
        const int bezel = qRound(dev.bezel * zoom);
        radius = qRound(dev.radius * zoom);
        const int w = qRound(dev.width * zoom);
        const int h = dev.height > 0 ? qRound(dev.height * zoom) : availH;
        const int y = dev.height > 0 ? qMax(margin + bezel, (area.height() - h) / 2) : margin;
        geo = QRect(area.x() + (area.width() - w) / 2, area.y() + y, w, h);
        if (bezel > 0) {
            const QSize size = geo.size() + QSize(2 * bezel, 2 * bezel);
            stage->hull = QRectF(geo).adjusted(-bezel, -bezel, bezel, bezel);
            stage->hullRadius = radius + bezel;
            stage->devicePos = geo.topLeft() - QPoint(bezel, bezel);
            key = QStringLiteral("tablet %1x%2 %3").arg(size.width()).arg(size.height()).arg(dpr);
            if (stage->deviceKey != key) {
                stage->device = QPixmap::fromImage(drawnDevice(size, bezel, radius, stage->body, stage->outline, dpr));
                stage->device.setDevicePixelRatio(dpr);
            }
        } else {
            stage->hull = QRectF(geo);
            stage->hullRadius = 3;
            stage->outlined = geo;
        }
    }
    if (key.isEmpty())
        stage->device = QPixmap();
    stage->deviceKey = key;
    stage->rebuild();

    for (int i = 0; i < 4; ++i) {
        // the part of the corner's square outside the rounded screen
        QPainterPath corner, screen;
        corner.addRect(QRectF((i & 1) ? geo.right() + 1 - radius : geo.x(), (i & 2) ? geo.bottom() + 1 - radius : geo.y(), radius, radius));
        screen.addRoundedRect(QRectF(geo), radius, radius);
        static_cast<DevicePatch *>(m_patches[i])->setShape(radius > 0 ? corner.subtracted(screen) : QPainterPath());
    }
    static_cast<DevicePatch *>(m_patches[4])->setShape(camera);

    if (css.isEmpty())
        css = QSize(qRound(geo.width() / zoom), qRound(geo.height() / zoom));
    QString size = tr("%1 × %2").arg(css.width()).arg(css.height());
    if (name)
        size.prepend(tr(name) + QStringLiteral(" · "));
    if (zoom < 0.995)
        size += QStringLiteral(" · %1%").arg(qRound(zoom * 100));
    if (m_mobile)
        size += tr(" · web version");
    m_sizeLabel->setText(size);
    m_sizeLabel->setToolTip(m_mobile ? tr("The preview runs the app's web version (react-native-web) in a phone frame, not the native iOS or "
                                          "Android app. Native-only modules (in-app purchases, Google Sign-In, Firebase's React Native "
                                          "persistence, …) don't exist there, so code that works on a device can fail here. Run on a "
                                          "device or emulator to see the native app.")
                                       : QString());
#ifdef QODE_HAS_WEBENGINE
    if (!m_view)
        return;
    m_view->setGeometry(geo);
    if (!qFuzzyCompare(m_view->zoomFactor(), zoom))
        m_view->setZoomFactor(zoom);
#endif
}

void WebPreview::updateNav()
{
#ifdef QODE_HAS_WEBENGINE
    m_back->setEnabled(m_view && m_view->history()->canGoBack());
    m_forward->setEnabled(m_view && m_view->history()->canGoForward());
#endif
}

void WebPreview::addConsole(const ConsoleEntry &entry)
{
    ConsoleEntry e = entry;
    e.serial = ++m_consoleSerial;
    m_console.append(e);
    if (m_console.size() > 1000)
        m_console.removeFirst();
    if (e.level == 2)
        ++m_errors;
    else if (e.level == 1)
        ++m_warnings;
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const QColor colour = e.level == 2 ? t.danger : e.level == 1 ? t.warning : t.editorFg;
    QString where;
    if (!e.source.isEmpty()) {
        const QUrl src(e.source);
        where = (src.path().isEmpty() ? e.source : src.path()) + (e.line > 0 ? QStringLiteral(":%1").arg(e.line) : QString());
    }
    m_consoleView->appendHtml(QStringLiteral("<span style=\"color:%1\">%2</span>%3")
                                  .arg(colour.name(), e.message.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")),
                                       where.isEmpty() ? QString()
                                                       : QStringLiteral(" <span style=\"color:%1\">— %2</span>").arg(t.textMuted.name(), where.toHtmlEscaped())));
    updateConsoleButton();
}

void WebPreview::updateConsoleButton()
{
    QStringList bits;
    if (m_errors)
        bits << tr("%n error(s)", nullptr, m_errors);
    if (m_warnings)
        bits << tr("%n warning(s)", nullptr, m_warnings);
    m_consoleBtn->setText(bits.isEmpty() ? tr("Console") : bits.join(QStringLiteral(" · ")));
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_consoleBtn->setStyleSheet(m_errors ? QStringLiteral("QToolButton { color: %1; }").arg(t.danger.name())
                                : m_warnings ? QStringLiteral("QToolButton { color: %1; }").arg(t.warning.name())
                                             : QString());
}

void WebPreview::openDevTools()
{
#ifdef QODE_HAS_WEBENGINE
    if (!m_page)
        return;
    if (!m_devToolsView) {
        m_devToolsView = new QWebEngineView;
        m_devToolsView->setWindowTitle(tr("Preview — Developer Tools"));
        m_devToolsView->resize(1100, 720);
        auto *page = new QWebEnginePage(m_profile, m_devToolsView);
        m_devToolsView->setPage(page);
        m_page->setDevToolsPage(page);
    }
    m_devToolsView->show();
    m_devToolsView->raise();
    m_devToolsView->activateWindow();
#endif
}

void WebPreview::restyle()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_icon->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/globe.svg"), t.editorFg, 16, devicePixelRatioF()));
    // The frame reads as a device in both kinds of theme: near black on dark, the text colour on light.
    m_viewport->backdrop = t.frame;
    m_viewport->body = t.dark ? t.editorBg.darker(170) : t.editorFg;
    m_viewport->outline = t.dark ? t.border.lighter(130) : t.border;
    m_viewport->shadow = t.dark ? t.frame.darker(250) : t.textMuted;
    m_viewport->deviceKey.clear(); // the tablet is drawn in these colours
    auto *os = static_cast<SegmentPill *>(m_osToggle);
    os->track = t.panel;
    os->border = t.border;
    os->thumb = t.accent;
    os->text = t.textMuted;
    os->textHover = t.editorFg;
    os->textActive = t.onAccent();
    os->focus = t.accent;
    os->update();
    layoutView();
    m_progress->setStyleSheet(QStringLiteral("QProgressBar { border: none; background: transparent; } QProgressBar::chunk { background: %1; }")
                                  .arg(t.accent.name()));
    m_consoleView->setStyleSheet(QStringLiteral("QPlainTextEdit { border: none; border-top: 1px solid %1; }").arg(t.border.name()));
    m_noticeIcon->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/triangle-alert.svg"), t.warning, 16, devicePixelRatioF()));
    m_notice->setStyleSheet(QStringLiteral("QWidget#previewNotice { background: %1; border-bottom: 1px solid %2; }"
                                           "QWidget#previewNotice QLabel { background: transparent; }")
                                .arg(t.panel.name(), t.warning.name()));
    updateConsoleButton();
}

// --- the chat's preview tools ----------------------------------------------------------------------------------------

DevServer *WebPreview::currentServer() const
{
    return m_server;
}

bool WebPreview::pageShown() const
{
#ifdef QODE_HAS_WEBENGINE
    return m_view && m_stack->currentIndex() == 1 && currentUrl().isValid();
#else
    return false;
#endif
}

QString WebPreview::pageTitle() const
{
#ifdef QODE_HAS_WEBENGINE
    return m_view ? m_view->title() : QString();
#else
    return {};
#endif
}

void WebPreview::setDeviceMode(Device d, bool landscape, bool android)
{
    if (m_mobile)
        d = Device::Phone;
    if (d == Device::Phone && android != m_android)
        static_cast<SegmentPill *>(m_osToggle)->setCurrent(android ? 1 : 0); // moves the pill (setDevice relayouts)
    m_android = android;
    (d == Device::Phone ? m_phone : d == Device::Tablet ? m_tablet : d == Device::Desktop ? m_desktop : m_fit)->setChecked(true);
    const bool turn = landscape && (d == Device::Phone || d == Device::Tablet);
    m_landscape = turn;
    m_rotate->blockSignals(true);
    m_rotate->setChecked(turn);
    m_rotate->blockSignals(false);
    setDevice(d);
}

QString WebPreview::deviceName() const
{
    switch (m_device) {
    case Device::Phone: return tr((m_android ? kPixel : kIphone).name);
    case Device::Tablet: return tr("Tablet");
    case Device::Desktop: return tr("Desktop");
    case Device::Fit: break;
    }
    return {};
}

QSize WebPreview::cssViewport() const
{
#ifdef QODE_HAS_WEBENGINE
    if (m_view) {
        const qreal zoom = m_view->zoomFactor() > 0 ? m_view->zoomFactor() : 1.0;
        return QSize(qRound(m_view->width() / zoom), qRound(m_view->height() / zoom));
    }
#endif
    return {};
}

QVariant WebPreview::evaluate(const QString &script, int timeoutMs, bool mainWorld, bool *timedOut)
{
    if (timedOut)
        *timedOut = false;
#ifdef QODE_HAS_WEBENGINE
    if (!m_page)
        return {};
    // The callback may come after the wait gave up: it only touches this shared state.
    struct State {
        bool done = false;
        QVariant value;
        QPointer<QEventLoop> loop;
    };
    auto state = std::make_shared<State>();
    QEventLoop loop;
    state->loop = &loop;
    m_page->runJavaScript(script, mainWorld ? QWebEngineScript::MainWorld : QWebEngineScript::ApplicationWorld, [state](const QVariant &v) {
        state->done = true;
        state->value = v;
        if (state->loop)
            state->loop->quit();
    });
    if (!state->done) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!state->done && timedOut)
        *timedOut = true;
    return state->value;
#else
    Q_UNUSED(script)
    Q_UNUSED(timeoutMs)
    Q_UNUSED(mainWorld)
    return {};
#endif
}

QImage WebPreview::capturePage(bool fullPage, QString *error)
{
#ifdef QODE_HAS_WEBENGINE
    if (!pageShown()) {
        *error = tr("no page is shown in the preview");
        return {};
    }
    const QRect geo = m_view->geometry();
    const qreal zoom = m_view->zoomFactor();
    const QSize css = cssViewport();
    int height = css.height();
    if (fullPage) {
        const int scroll = evaluate(QStringLiteral("Math.max(document.documentElement.scrollHeight, document.body ? document.body.scrollHeight : 0)"), 3000).toInt();
        height = qBound(css.height(), scroll, 3000);
    } else if (m_device == Device::Desktop) {
        height = qMin(height, 900); // desktop is as tall as the panel there: a picture of a 1440 x 900 window instead
    }
    const bool relayout = !qFuzzyCompare(zoom, 1.0) || height != css.height();
    QLabel *still = nullptr;
    auto wait = [](int ms) {
        QEventLoop pause;
        QTimer::singleShot(ms, &pause, &QEventLoop::quit);
        pause.exec();
    };
    if (relayout) {
        // A still picture of the panel hides the page while it is laid out at full size behind it.
        still = new QLabel(m_viewport);
        still->setPixmap(m_viewport->grab());
        still->setGeometry(m_viewport->rect());
        still->show();
        still->raise();
        m_view->setZoomFactor(1.0);
        m_view->resize(css.width(), height);
        const QString want = QStringLiteral("%1x%2").arg(css.width()).arg(height);
        for (int i = 0; i < 30; ++i) { // until the page has its new size, then two frames for the paint
            if (evaluate(QStringLiteral("window.innerWidth + 'x' + window.innerHeight"), 500).toString() == want)
                break;
            wait(100);
        }
        wait(250);
    }
    QImage image = m_view->grab().toImage();
    if (relayout) {
        m_view->setGeometry(geo);
        m_view->setZoomFactor(zoom);
        QTimer::singleShot(300, still, &QObject::deleteLater); // the page repaints at the panel's size first
    }
    if (image.isNull()) {
        *error = tr("the page could not be captured");
        return {};
    }
    if (image.size() != QSize(css.width(), height)) // a HiDPI screen: back to CSS pixels
        image = image.scaled(css.width(), height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    image.setDevicePixelRatio(1);
    return image;
#else
    *error = tr("this QODE was built without Qt WebEngine");
    Q_UNUSED(fullPage)
    return {};
#endif
}

void WebPreview::reloadPage()
{
#ifdef QODE_HAS_WEBENGINE
    if (m_view)
        m_view->reload();
#endif
}

void WebPreview::goBack()
{
#ifdef QODE_HAS_WEBENGINE
    if (m_view)
        m_view->back();
#endif
}
