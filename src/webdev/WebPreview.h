#pragma once

#include <QHash>
#include <QPointer>
#include <QUrl>
#include <QVector>
#include <QWidget>

#include <functional>

class DevServer;
class DeviceStage;
class QLabel;
class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QStringListModel;
class QTimer;
class QToolButton;
#ifdef QODE_HAS_WEBENGINE
class QWebEngineProfile;
class QWebEngineView;
class PreviewPage;
#endif

// Right-hand panel that shows the running dev server's page in an embedded Chromium (Qt WebEngine), so edits made
// from the chat show up live through the server's hot reload. It follows the server: a placeholder while it is
// stopped / starting / failed, the page once it runs (a restart on another port follows along). Device modes lay the
// page out at phone / tablet / desktop width and scale it down to fit the panel; phone mode shows it inside an iPhone
// or Android mockup, picked with a pill above the device. Console messages of the page are
// collected in a strip below it. Without Qt WebEngine the panel only offers to open the page in the browser.
// A project can have several things to preview (a web app and Expo apps): the header picks the target, and the
// placeholder offers to start any of them. An Expo app is shown only in the phone mockup, and it is the app's web
// version (react-native-web), which the panel says wherever that matters: native-only modules do not exist there.
// A target that knows its routes (Expo Router's app/ folder) lists them next to the address bar.
class WebPreview : public QWidget
{
    Q_OBJECT
public:
    struct ConsoleEntry
    {
        int level = 0; // 0 info, 1 warning, 2 error
        QString message, source;
        int line = 0;
        int serial = 0; // counts up across page loads (the chat's preview_console reads what is new)
    };
    enum class Device { Fit, Phone, Tablet, Desktop };
    struct Route
    {
        QString path; // "/friend/[id]": bracketed segments are asked for when the route is opened
        QString file; // the screen's file, relative to the app
    };
    // Something the panel can show: the web dev server or an Expo app run for web.
    struct Target
    {
        QString id;                 // "web", "expo:<relative app folder>"
        QString name;               // "Next.js", the Expo app's name
        QString where;              // folder relative to the project, "" = its root
        DevServer *server = nullptr; // the server running this target, nullptr while it is not running
        bool mobile = false;        // an Expo app: phone mockup only
        QString problem;            // why it cannot run yet (offered a fix), empty when it can
        std::function<QVector<Route>()> routes; // read again each time the list opens; unset = no route list
        QString routesDir;          // where they come from ("app"), for the list's heading
        int minChromium = 0;        // the page needs at least this Chromium; an older preview engine says so
        QString minChromiumWhy;     // what needs it: "expo-sqlite"
    };

    explicit WebPreview(QWidget *parent = nullptr);
    ~WebPreview() override;

    // `current` picks the shown target (kept when empty and still listed, else the first).
    void setTargets(const QVector<Target> &targets, const QString &current = QString());
    QString currentTarget() const { return m_target; }
    QUrl currentUrl() const;
    const QVector<ConsoleEntry> &console() const { return m_console; }

    // For the chat's preview tools (MainWindowIdePreview.cpp). The waits run the event loop.
    DevServer *currentServer() const;
    bool pageShown() const; // the page (not a placeholder) is up
    bool isLoading() const { return m_loading; }
    int loadsFinished() const { return m_loadsFinished; }
    QString pageTitle() const;
    int consoleSerial() const { return m_consoleSerial; } // the serial of the newest entry
    // Device mode as the header's buttons set it; an Expo app is shown on a phone only.
    void setDeviceMode(Device d, bool landscape, bool android);
    Device device() const { return m_device; }
    bool landscape() const { return m_landscape; }
    bool android() const { return m_android; }
    bool mobileTarget() const { return m_mobile; }
    QString deviceName() const; // "iPhone 15", "Tablet", "Desktop", "" for Fit
    QSize cssViewport() const;  // the page's CSS size as laid out now
    // The page as a browser at the current device would show it: unscaled CSS pixels (the panel shows it scaled down;
    // the page is laid out at 100 % for the picture while a still image covers the panel). `fullPage` takes the
    // whole scroll height (at most 3000 px). Null with `error` set when it fails.
    QImage capturePage(bool fullPage, QString *error);
    // Runs JavaScript in the page and returns its value (invalid on a timeout or an exception). `mainWorld`: in the
    // page's own scripts' world, else in an isolated one (same DOM, no page globals).
    QVariant evaluate(const QString &script, int timeoutMs, bool mainWorld = false, bool *timedOut = nullptr);
    void navigateTo(const QString &text) { navigate(text); }
    void reloadPage();
    void goBack();

signals:
    void closeRequested();
    void targetChosen(const QString &id); // picked in the header or the placeholder
    void startRequested(const QString &id);
    void logRequested(const QString &id);
    void fixRequested(const QString &id); // the target's problem: "Add Web Support"

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    const Target *target() const;
    void selectTarget(const QString &id);
    void attach(DevServer *server);
    void applyTargetKind();
    void rebuildTargetMenu();
    void onServerState();
    void showPlaceholder(const QString &text, bool start, bool log);
    void showChoices(); // more than one target and none of them running: offer each
    void ensureView();
    void loadFromServer();
    void navigate(const QString &text);
    void refreshRoutes();
    void rebuildRoutesMenu();
    void openRoute(const QString &pattern);
    void checkCrash();
    void showCrash(const QString &message);
    void setDevice(Device d);
    void layoutView();
    void applyDeviceStyle();
    void updateNav();
    void addConsole(const ConsoleEntry &e);
    void updateConsoleButton();
    void openDevTools();
    void restyle();

    QVector<Target> m_targets;
    QString m_target;
    QPointer<DevServer> m_server;
    QUrl m_base;       // http://localhost:<port>/ of the running server
    QString m_lastUrl; // what was shown, so a restart on the same port comes back to the same route
    QHash<QString, QString> m_lastUrls; // of the targets not shown now
    Device m_device = Device::Fit;
    bool m_mobile = false;                // showing an Expo app: phone only
    Device m_webDevice = Device::Fit;     // the web app's device while an Expo app is shown
    bool m_webLandscape = false;
    bool m_landscape = false; // phone / tablet turned sideways
    bool m_android = false;   // phone mockup: Pixel instead of iPhone
    QVector<ConsoleEntry> m_console;
    int m_consoleSerial = 0;
    bool m_loading = false;
    int m_loadsFinished = 0;
    int m_errors = 0, m_warnings = 0;
    bool m_pendingLoad = false; // the server came up while the panel was hidden: load once it is shown
    QVector<Route> m_routes;
    QHash<QString, QString> m_paramValues; // last value typed for a dynamic segment, by its name
    QString m_firstError;                  // the first uncaught error since the page loaded
    QTimer *m_crashCheck;                  // after it: is the page still blank?
    bool m_routePicked = false;            // a completion was picked; it navigates, not returnPressed

    QLabel *m_icon, *m_title, *m_sizeLabel;
    QToolButton *m_targetBtn;
    QMenu *m_targetMenu;
    QToolButton *m_fit, *m_phone, *m_tablet, *m_desktop, *m_rotate, *m_devTools, *m_external, *m_closeBtn;
    QToolButton *m_back, *m_forward, *m_reload, *m_consoleBtn, *m_routesBtn;
    QMenu *m_routesMenu;
    QStringListModel *m_routeModel; // the address bar's completions
    QLineEdit *m_address;
    QWidget *m_notice; // over the page: it crashed while starting
    QLabel *m_noticeIcon, *m_noticeText;
    QProgressBar *m_progress;
    QStackedWidget *m_stack;
    QLabel *m_placeholder;
    QPushButton *m_startBtn, *m_logBtn, *m_fixBtn;
    QWidget *m_choices; // a start button per target
    DeviceStage *m_viewport;
    QWidget *m_osToggle;   // iOS / Android pill above the phone
    QWidget *m_patches[5]; // pieces of the device drawn over the page: its four rounded corners and the camera
    QPlainTextEdit *m_consoleView;
#ifdef QODE_HAS_WEBENGINE
    QWebEngineProfile *m_profile = nullptr;
    PreviewPage *m_page = nullptr;
    QWebEngineView *m_view = nullptr;
    QWebEngineView *m_devToolsView = nullptr;
#endif
};
