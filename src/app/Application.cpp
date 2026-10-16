#include "Application.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QGridLayout>
#include <QLayout>
#include <QTabWidget>
#include <QRegularExpression>
#include <functional>
#include <QComboBox>
#include <QDir>
#include <QMenu>
#include <QMenuBar>
#include <QTabBar>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>

namespace {
constexpr double kBaseFontSize = 9.5;
}

Application::Application(int &argc, char **argv)
    : QApplication(argc, argv)
{
    setApplicationName(QStringLiteral("QODE"));
    setOrganizationName(QStringLiteral("QODE"));
    setApplicationVersion(QStringLiteral(QODE_VERSION));
    setDesktopFileName(QStringLiteral("qode"));
    QIcon appIcon;
    for (int size : {16, 32, 48, 64, 128, 256, 512})
        appIcon.addFile(QStringLiteral(":/icons/qode-%1.png").arg(size), QSize(size, size));
    setWindowIcon(appIcon);
    setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    // Bundled fonts: Plus Jakarta Sans for the UI, JetBrains Mono for code (see SettingsManager::editorFont).
    const QStringList fonts = QDir(QStringLiteral(":/fonts")).entryList({QStringLiteral("*.ttf")});
    for (const QString &f : fonts)
        QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/") + f);
    QFont ui(QStringLiteral("Plus Jakarta Sans"));
    ui.setPointSizeF(kBaseFontSize);
    setFont(ui);
    m_zoom = SettingsManager::instance().uiZoom() / 100.0;
    ui.setPointSizeF(kBaseFontSize * m_zoom);
    setFont(ui);

    installEventFilter(this); // app-wide: gives clickable widgets a hand cursor (see eventFilter)
    applyTheme(SettingsManager::instance().theme());
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &Application::applyTheme);
    connect(&SettingsManager::instance(), &SettingsManager::editorSettingsChanged, this, &Application::applyZoom);
    connect(&SettingsManager::instance(), &SettingsManager::projectSettingsChanged, this, &Application::applyZoom);
}

namespace {
// Every size a widget has "from the program" is remembered together with the value we last wrote, so a
// later change made by the program itself (a new fixed height, a new style sheet) becomes the new base.
template<typename T, typename Scale>
void track(QWidget *w, const char *name, const T &current, Scale scale, const std::function<void(const T &)> &apply)
{
    const QByteArray baseKey = QByteArray("_zb_") + name, doneKey = QByteArray("_zd_") + name;
    QVariant base = w->property(baseKey.constData());
    const QVariant done = w->property(doneKey.constData());
    if (!base.isValid() || !done.isValid() || done.value<T>() != current)
        base = QVariant::fromValue(current);
    const T target = scale(base.value<T>());
    w->setProperty(baseKey.constData(), base);
    w->setProperty(doneKey.constData(), QVariant::fromValue(target));
    if (target != current)
        apply(target);
}
}

QString Application::scaled(const QString &sheet) const
{
    if (qFuzzyCompare(m_zoom, 1.0) || sheet.isEmpty())
        return sheet;
    static const QRegularExpression re(QStringLiteral("(\\d+(?:\\.\\d+)?)(px|pt)\\b"));
    QString out;
    qsizetype last = 0;
    for (auto it = re.globalMatch(sheet); it.hasNext();) {
        const auto m = it.next();
        out += sheet.mid(last, m.capturedStart() - last);
        const double v = m.captured(1).toDouble() * m_zoom;
        out += (m.captured(2) == QLatin1String("px") ? QString::number(qRound(v)) : QString::number(v, 'f', 1)) + m.captured(2);
        last = m.capturedEnd();
    }
    return out + sheet.mid(last);
}

void Application::scaleWidget(QWidget *w)
{
    if (w->inherits("QTipLabel"))
        return;
    auto sc = [this](int v) { return qRound(v * m_zoom); };
    // style sheet
    {
        const QString cur = w->styleSheet();
        QString base = w->property("_zb_sheet").toString();
        if (cur != w->property("_zd_sheet").toString() || !w->property("_zd_sheet").isValid())
            base = cur;
        const QString target = scaled(base);
        w->setProperty("_zb_sheet", base);
        w->setProperty("_zd_sheet", target);
        if (target != cur)
            w->setStyleSheet(target);
    }
    // fonts that were set explicitly (the application font reaches the others by itself)
    if (w->testAttribute(Qt::WA_SetFont) && w->font().pointSizeF() > 0) {
        track<double>(w, "font", w->font().pointSizeF(), [this](double v) { return v * m_zoom; }, [w](const double &v) {
            QFont f = w->font();
            f.setPointSizeF(v);
            w->setFont(f);
        });
    }
    if (!w->isWindow()) {
        const QSize mn = w->minimumSize(), mx = w->maximumSize();
        auto big = [](int v) { return v >= QWIDGETSIZE_MAX; };
        track<QSize>(w, "min", mn, [&](const QSize &s) { return QSize(sc(s.width()), sc(s.height())); }, [w](const QSize &s) { w->setMinimumSize(s); });
        track<QSize>(w, "max", mx, [&](const QSize &s) { return QSize(big(s.width()) ? s.width() : sc(s.width()), big(s.height()) ? s.height() : sc(s.height())); },
                     [w](const QSize &s) { w->setMaximumSize(s); });
    }
    QVariant icon = w->property("iconSize");
    if (qobject_cast<QAbstractItemView *>(w) && icon.toSize().isEmpty())
        icon = QSize(16, 16); // the style's default for item views
    if (icon.metaType() == QMetaType::fromType<QSize>() && icon.toSize().isValid() && !w->inherits("QMenu") && !qobject_cast<QTabWidget *>(w)) // QTabWidget::iconSize is its tab bar's: scaling both compounded
        track<QSize>(w, "icon", icon.toSize(), [&](const QSize &s) { return QSize(sc(s.width()), sc(s.height())); }, [w](const QSize &s) { w->setProperty("iconSize", s); });
    if (QLayout *l = w->layout()) {
        track<QMargins>(w, "margins", l->contentsMargins(), [&](const QMargins &m) { return QMargins(sc(m.left()), sc(m.top()), sc(m.right()), sc(m.bottom())); },
                        [l](const QMargins &m) { l->setContentsMargins(m); });
        if (l->spacing() >= 0 && !qobject_cast<QGridLayout *>(l))
            track<int>(w, "spacing", l->spacing(), sc, [l](const int &v) { l->setSpacing(v); });
    }
}

void Application::applyZoom()
{
    const double z = SettingsManager::instance().uiZoom() / 100.0;
    if (qFuzzyCompare(z, m_zoom))
        return;
    m_zoom = z;
    QFont f = font();
    f.setPointSizeF(kBaseFontSize * m_zoom);
    setFont(f);
    setStyleSheet(scaled(Theme::byName(SettingsManager::instance().theme()).styleSheet()));
    const auto widgets = allWidgets();
    for (QWidget *w : widgets)
        scaleWidget(w);
    for (QWidget *w : widgets) { // wrapped labels and size hints must be recomputed for the new metrics
        if (w->layout())
            w->layout()->invalidate();
        w->updateGeometry();
        if (auto *v = qobject_cast<QAbstractItemView *>(w))
            v->doItemsLayout();
    }
}

// Buttons, tabs and combo boxes get the pointing-hand cursor when they are first polished, unless a
// widget already chose its own cursor.
bool Application::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::StyleChange && watched->isWidgetType() && !qFuzzyCompare(m_zoom, 1.0)) {
        auto *w = static_cast<QWidget *>(watched);
        if (w->styleSheet() != w->property("_zd_sheet").toString())
            scaleWidget(w);
    }
    if (event->type() == QEvent::Polish && watched->isWidgetType()) {
        auto *w = static_cast<QWidget *>(watched);
        if (!qFuzzyCompare(m_zoom, 1.0))
            scaleWidget(w);
        if (qstrcmp(w->metaObject()->className(), "QTipLabel") == 0) { // tooltip window: transparent corners so the stylesheet's radius shows
            w->setWindowFlags(w->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
            w->setAttribute(Qt::WA_TranslucentBackground);
            return QApplication::eventFilter(watched, event);
        }
        if (qobject_cast<QMenu *>(w) && !w->testAttribute(Qt::WA_TranslucentBackground)) { // every menu: transparent corners so the stylesheet's border radius shows
            w->setWindowFlags(w->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
            w->setAttribute(Qt::WA_TranslucentBackground);
        }
        const bool clickable = qobject_cast<QAbstractButton *>(w) || qobject_cast<QTabBar *>(w) || qobject_cast<QComboBox *>(w) ||
                               qobject_cast<QMenuBar *>(w);
        if (clickable && !w->testAttribute(Qt::WA_SetCursor))
            w->setCursor(Qt::PointingHandCursor);
    }
    return QApplication::eventFilter(watched, event);
}

void Application::applyTheme(const QString &name)
{
    const Theme t = Theme::byName(name);
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.editorFg);
    p.setColor(QPalette::Base, t.editorBg);
    p.setColor(QPalette::AlternateBase, t.panel);
    p.setColor(QPalette::Text, t.editorFg);
    p.setColor(QPalette::Button, t.panel);
    p.setColor(QPalette::ButtonText, t.editorFg);
    p.setColor(QPalette::ToolTipBase, t.panel);
    p.setColor(QPalette::ToolTipText, t.editorFg);
    p.setColor(QPalette::Highlight, t.selection);
    p.setColor(QPalette::HighlightedText, t.editorFg);
    p.setColor(QPalette::PlaceholderText, t.textMuted);
    p.setColor(QPalette::Disabled, QPalette::Text, t.textMuted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.textMuted);
    setPalette(p);
    setStyleSheet(scaled(t.styleSheet()));
}
