// The "preview" tool group: the web preview's page as the model's eyes - a screenshot (sent to the model as an image),
// the page's console, the device the page is laid out for, and navigation. Everything goes through Hooks::ide
// (MainWindowIdePreview.cpp), which shows the preview and waits for the page first.
#include "AgentToolHelpers.h"

#include <QDateTime>
#include <QFile>

namespace agent {

namespace {
const QStringList kDevices{"fit", "phone", "tablet", "desktop"};
constexpr const char *kServerHelp = "Which app to show (server id from server_status, e.g. expo:apps/mobile). Default: what the preview shows.";
} // namespace

ToolResult AgentTools::previewTool(const QString &action, const QJsonObject &a)
{
    if (!m_hooks.ide)
        return fail(QStringLiteral("the IDE cannot be controlled right now"));
    if (action == QLatin1String("preview_screenshot") && a.value("reload").toBool())
        flushEdits(); // a static server serves what is on disk
    QJsonObject r = m_hooks.ide(action, a);
    if (r.contains("error"))
        return fail(r.value("error").toString());
    ToolResult out;
    const QJsonObject image = r.take("image").toObject();
    if (!image.isEmpty()) {
        out.images.append(image);
        // Debugging: what the model was shown.
        const QString dump = qEnvironmentVariable("QODE_AGENT_DUMP_IMAGES");
        if (!dump.isEmpty()) {
            QFile f(QStringLiteral("%1/%2-%3.png").arg(dump, QDateTime::currentDateTime().toString(QStringLiteral("HHmmss-zzz")), action));
            if (f.open(QIODevice::WriteOnly))
                f.write(QByteArray::fromBase64(image.value("data").toString().toLatin1()));
        }
    }
    QString summary = r.take("summary").toString();
    if (summary.isEmpty()) {
        summary = r.value("picture").toString(r.value("viewport").toString());
        const int errors = r.value("console").toObject().value("errors").toInt();
        if (action == QLatin1String("preview_console"))
            summary = QStringLiteral("%1 entries").arg(r.value("entries").toArray().size());
        else if (errors > 0)
            summary += QStringLiteral(" · %1").arg(plural(errors, "error", "errors"));
    }
    out.response = r;
    out.summary = summary;
    return out;
}

void AgentTools::registerPreviewTools()
{
    m_groupText.insert(QStringLiteral("preview"),
                       QStringLiteral("Web preview\n"
                                      "- QODE shows the dev server's page in its preview (Chromium). You can see it: preview_screenshot returns a picture "
                                      "of the page at the current device, unscaled. After a visual or layout change, take a screenshot at the device the "
                                      "change is about (preview_set_viewport: phone 393 px wide, tablet 820, desktop 1440; fit = the panel) and look at "
                                      "it before you answer; fix what looks wrong and look again. Check both phone and desktop when the request is "
                                      "about responsiveness.\n"
                                      "- preview_console lists the page's errors and warnings; read it when the page is blank, broken or something does "
                                      "not respond. Every preview answer also counts the console's errors.\n"
                                      "- Servers with hot reload update the page by themselves after an edit is saved. A plain static server does not: "
                                      "pass reload=true to preview_screenshot (it saves your edits first).\n"
                                      "- preview_click, preview_type and preview_eval act in the page (the user approves them): use them to try what "
                                      "you built (fill a form, open a menu, check a value), then look again.\n"
                                      "- The preview needs the server running (server_start)."));

    Spec shot{"preview_screenshot", "A picture of the web preview's page as a browser at the current device shows it (unscaled CSS pixels), with its "
                                    "url, viewport size and console error count. Shows the preview panel when it is hidden.",
              makeSchema({{"full_page", "boolean", "The whole scroll height (at most 3000 px) instead of the visible part. Default false.", false},
                          {"reload", "boolean", "Save edits and reload the page first (servers without hot reload). Default false.", false},
                          {"server", "string", kServerHelp, false}}),
              Risk::ReadOnly, [this](const QJsonObject &a) { return previewTool(QStringLiteral("preview_screenshot"), a); },
              [](const QJsonObject &a) { return a.value("full_page").toBool() ? QStringLiteral("Look at the whole page") : QStringLiteral("Look at the page"); }, {}};
    shot.group = QStringLiteral("preview");
    add(shot);

    Spec console{"preview_console", "The page's console since it loaded: errors and warnings (or everything) with their source file and line. By "
                                    "default only what earlier calls did not return.",
                 withEnum(makeSchema({{"level", "string", "error, warning (errors and warnings, default) or all.", false},
                                      {"include_read", "boolean", "Also what earlier calls returned. Default false.", false},
                                      {"server", "string", kServerHelp, false}}),
                          "level", {"error", "warning", "all"}),
                 Risk::ReadOnly, [this](const QJsonObject &a) { return previewTool(QStringLiteral("preview_console"), a); },
                 [](const QJsonObject &) { return QStringLiteral("Read the page's console"); }, {}};
    console.group = QStringLiteral("preview");
    add(console);

    Spec viewport{"preview_set_viewport", "Lay the page out for a device, like the preview's buttons: phone (iPhone 15 393x852, or Android Pixel 9 Pro "
                                          "412x915), tablet (820x1180), desktop (1440 wide; screenshots 1440x900), fit (the panel's size). The user sees the change.",
                  withEnum(withEnum(makeSchema({{"device", "string", "Which device."},
                                                {"landscape", "boolean", "Phone or tablet turned sideways. Default false.", false},
                                                {"phone_os", "string", "ios (default) or android.", false},
                                                {"server", "string", kServerHelp, false}}),
                                    "device", kDevices),
                           "phone_os", {"ios", "android"}),
                  Risk::Edit, [this](const QJsonObject &a) { return previewTool(QStringLiteral("preview_viewport"), a); },
                  [](const QJsonObject &a) {
                      return QStringLiteral("Show the page on %1%2").arg(str(a, "device"), a.value("landscape").toBool() ? QStringLiteral(" (landscape)") : QString());
                  }, {}};
    viewport.group = QStringLiteral("preview");
    add(viewport);

    Spec nav{"preview_navigate", "Open a page of the dev server in the preview (a path like /about), or reload / go back. Waits for the page to load.",
             withEnum(makeSchema({{"path", "string", "Path on the dev server, e.g. /pricing?plan=pro.", false},
                                  {"action", "string", "go (default, needs path), reload or back.", false},
                                  {"server", "string", kServerHelp, false}}),
                      "action", {"go", "reload", "back"}),
             Risk::ReadOnly, [this](const QJsonObject &a) { return previewTool(QStringLiteral("preview_navigate"), a); },
             [](const QJsonObject &a) {
                 const QString action = str(a, "action");
                 if (action == QLatin1String("reload"))
                     return QStringLiteral("Reload the page");
                 if (action == QLatin1String("back"))
                     return QStringLiteral("Go back in the preview");
                 return QStringLiteral("Open %1 in the preview").arg(str(a, "path"));
             }, {}};
    nav.group = QStringLiteral("preview");
    add(nav);

    Spec click{"preview_click", "Click the first element matching a CSS selector in the preview's page (scrolled into view), then wait for what it "
                                "starts. The user approves it.",
               makeSchema({{"selector", "string", "CSS selector, e.g. #join, nav a[href='/pricing'], button.primary."},
                           {"server", "string", kServerHelp, false}}),
               Risk::Dangerous, [this](const QJsonObject &a) { return previewTool(QStringLiteral("preview_click"), a); },
               [](const QJsonObject &a) { return QStringLiteral("Click %1 in the page").arg(shorten(str(a, "selector"), 50)); },
               [](const QJsonObject &a) { return QStringLiteral("Click in the preview's page:\n\n%1").arg(str(a, "selector")); }};
    click.group = QStringLiteral("preview");
    add(click);

    Spec type{"preview_type", "Type into the first input, textarea or editable element matching a CSS selector (replacing its value unless "
                              "append), optionally pressing Enter to submit. The user approves it.",
              makeSchema({{"selector", "string", "CSS selector of the field."}, {"text", "string", "What to type."},
                          {"append", "boolean", "Keep the current value. Default false.", false},
                          {"submit", "boolean", "Press Enter afterwards (submits its form). Default false.", false},
                          {"server", "string", kServerHelp, false}}),
              Risk::Dangerous, [this](const QJsonObject &a) { return previewTool(QStringLiteral("preview_type"), a); },
              [](const QJsonObject &a) { return QStringLiteral("Type into %1").arg(shorten(str(a, "selector"), 50)); },
              [](const QJsonObject &a) {
                  return QStringLiteral("Type into %1 in the preview's page:\n\n%2%3")
                      .arg(str(a, "selector"), str(a, "text"), a.value("submit").toBool() ? QStringLiteral("\n\n(then press Enter)") : QString());
              }};
    type.group = QStringLiteral("preview");
    add(type);

    Spec eval{"preview_eval", "Run JavaScript in the preview's page (its own global scope) and return the value of the last statement as JSON; "
                              "a promise is awaited. For reading state the screenshot does not show (computed styles, sizes, app state). "
                              "The user approves it.",
              makeSchema({{"code", "string", "JavaScript, e.g. getComputedStyle(document.querySelector('nav')).display"},
                          {"timeout_seconds", "integer", "How long to wait for a promise. Default 10, max 60.", false},
                          {"server", "string", kServerHelp, false}}),
              Risk::Dangerous, [this](const QJsonObject &a) { return previewTool(QStringLiteral("preview_eval"), a); },
              [](const QJsonObject &a) { return QStringLiteral("Run `%1` in the page").arg(shorten(str(a, "code"), 60)); },
              [](const QJsonObject &a) { return QStringLiteral("Run JavaScript in the preview's page:\n\n%1").arg(str(a, "code")); }};
    eval.group = QStringLiteral("preview");
    add(eval);
}

} // namespace agent
