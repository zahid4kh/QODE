// MainWindow's side of the chat's preview tools (group "preview"): screenshots of the web preview's page, its console,
// the device it is laid out for, and navigation. Every call first makes sure the preview is shown and its page has
// loaded; the server itself is started with the server tools (MainWindowIdeServers.cpp).
#include "MainWindow.h"

#include "expo/ExpoBar.h"
#include "webdev/DevServer.h"
#include "webdev/DevServerBar.h"
#include "webdev/WebPreview.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QUrl>

namespace {
QJsonObject error(const QString &message)
{
    return QJsonObject{{"error", message}};
}

void pause(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// Runs the event loop until `done` or the time is up.
bool waitFor(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() >= timeoutMs)
            return false;
        pause(100);
    }
    return true;
}

QString stateName(DevServer *s)
{
    if (!s)
        return QStringLiteral("stopped");
    switch (s->state()) {
    case DevServer::State::Starting: return QStringLiteral("still starting");
    case DevServer::State::Running: return QStringLiteral("running");
    case DevServer::State::Failed: return QStringLiteral("failed");
    default: return QStringLiteral("stopped");
    }
}

QString levelName(int level)
{
    return level == 2 ? QStringLiteral("error") : level == 1 ? QStringLiteral("warning") : QStringLiteral("log");
}

// A JavaScript string literal.
QString jsString(const QString &s)
{
    const QByteArray a = QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact); // ["..."]
    return QString::fromUtf8(a.mid(1, a.size() - 2));
}

// "app.js:7" from a console entry's source URL.
QString place(const WebPreview::ConsoleEntry &e)
{
    if (e.source.isEmpty())
        return {};
    const QUrl url(e.source);
    QString p = url.path().isEmpty() ? e.source : url.path();
    if (p.startsWith(QLatin1Char('/')))
        p.remove(0, 1);
    return e.line > 0 ? QStringLiteral("%1:%2").arg(p).arg(e.line) : p;
}
} // namespace

WebPreview *MainWindow::idePreview(const QString &server, QString *err)
{
#ifndef QODE_HAS_WEBENGINE
    Q_UNUSED(server)
    *err = QStringLiteral("this QODE was built without the web preview (Qt WebEngine)");
    return nullptr;
#else
    if (!(m_serverBar && m_serverBar->isAvailable()) && !(m_expoBar && m_expoBar->isAvailable())) {
        *err = QStringLiteral("this project has nothing to preview (no web dev server or Expo app)");
        return nullptr;
    }
    WebPreview *p = webPreview();
    if (!server.isEmpty() && server != p->currentTarget()) {
        showPreviewTarget(server, false);
        if (p->currentTarget() != server) {
            *err = QStringLiteral("the preview cannot show '%1'; see server_status for the ids").arg(server);
            return nullptr;
        }
    }
    if (!rightPanelShows(p))
        toggleWebPreview();
    DevServer *srv = p->currentServer();
    if (!srv || srv->state() != DevServer::State::Running) {
        *err = QStringLiteral("the preview shows %1, whose server is %2; start it with server_start (server: %1)").arg(p->currentTarget(), stateName(srv));
        return nullptr;
    }
    // Just shown: the page loads now.
    if (!waitFor([p] { return p->pageShown() && !p->isLoading(); }, 15000)) {
        *err = p->pageShown() ? QStringLiteral("the page is still loading after 15 s") : QStringLiteral("the page did not open in the preview");
        return nullptr;
    }
    return p;
#endif
}

// What every preview answer carries: where the page is, how it is laid out, and its console's errors since it loaded.
QJsonObject MainWindow::idePreviewState(WebPreview *p) const
{
    const QSize css = p->cssViewport();
    QString device = p->deviceName().isEmpty() ? QStringLiteral("fit (the panel's width)") : p->deviceName();
    if (p->landscape())
        device += QStringLiteral(", landscape");
    QJsonObject o{{"url", p->currentUrl().toString()}, {"title", p->pageTitle()}, {"device", device},
                  {"viewport", QStringLiteral("%1x%2").arg(css.width()).arg(css.height())}};
    int errors = 0, warnings = 0;
    QJsonArray firstErrors;
    for (const WebPreview::ConsoleEntry &e : p->console()) {
        errors += e.level == 2;
        warnings += e.level == 1;
        if (e.level == 2 && firstErrors.size() < 3)
            firstErrors.append(e.message.left(300) + (place(e).isEmpty() ? QString() : QStringLiteral(" (%1)").arg(place(e))));
    }
    o.insert("console", QJsonObject{{"errors", errors}, {"warnings", warnings}});
    if (!firstErrors.isEmpty())
        o.insert("errors", firstErrors);
    return o;
}

QJsonObject MainWindow::idePreviewAction(const QString &action, const QJsonObject &args)
{
    QString err;
    WebPreview *p = idePreview(args.value("server").toString(), &err);
    if (!p)
        return error(err);

    if (action == QLatin1String("preview_screenshot")) {
        if (args.value("reload").toBool()) {
            const int loads = p->loadsFinished();
            p->reloadPage();
            waitFor([p, loads] { return p->loadsFinished() > loads && !p->isLoading(); }, 15000);
            pause(300); // scripts that run after load
        }
        const QImage image = p->capturePage(args.value("full_page").toBool(), &err);
        if (image.isNull())
            return error(err);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        QJsonObject o = idePreviewState(p);
        o.insert("picture", QStringLiteral("%1x%2").arg(image.width()).arg(image.height()));
        o.insert("image", QJsonObject{{"mimeType", "image/png"}, {"data", QString::fromLatin1(png.toBase64())}});
        return o;
    }

    if (action == QLatin1String("preview_console")) {
        const QString level = args.value("level").toString(QStringLiteral("warning"));
        const int minLevel = level == QLatin1String("all") ? 0 : level == QLatin1String("error") ? 2 : 1;
        const bool all = args.value("include_read").toBool();
        QJsonArray entries;
        int skipped = 0;
        for (const WebPreview::ConsoleEntry &e : p->console()) {
            if (e.level < minLevel || (!all && e.serial <= m_previewConsoleRead))
                continue;
            if (entries.size() >= 60) {
                ++skipped;
                continue;
            }
            QJsonObject row{{"level", levelName(e.level)}, {"message", e.message.left(1500)}};
            if (!place(e).isEmpty())
                row.insert("at", place(e));
            entries.append(row);
        }
        m_previewConsoleRead = p->consoleSerial();
        QJsonObject o = idePreviewState(p);
        o.remove("errors");
        o.insert("entries", entries);
        if (skipped)
            o.insert("more", skipped);
        if (entries.isEmpty())
            o.insert("note", all ? QStringLiteral("nothing at this level since the page loaded")
                                 : QStringLiteral("nothing new at this level since the last preview_console (include_read shows everything since the page loaded)"));
        return o;
    }

    if (action == QLatin1String("preview_viewport")) {
        const QString d = args.value("device").toString();
        const WebPreview::Device device = d == QLatin1String("phone") ? WebPreview::Device::Phone
                                        : d == QLatin1String("tablet") ? WebPreview::Device::Tablet
                                        : d == QLatin1String("desktop") ? WebPreview::Device::Desktop
                                                                        : WebPreview::Device::Fit;
        p->setDeviceMode(device, args.value("landscape").toBool(), args.value("phone_os").toString() == QLatin1String("android"));
        pause(400); // media queries and the page's own resize handlers
        QJsonObject o = idePreviewState(p);
        if (p->mobileTarget() && device != WebPreview::Device::Phone)
            o.insert("note", QStringLiteral("an Expo app is only shown on a phone"));
        o.insert("summary", QStringLiteral("%1 %2").arg(o.value("device").toString(), o.value("viewport").toString()));
        return o;
    }

    if (action == QLatin1String("preview_navigate")) {
        const QString what = args.value("action").toString();
        const QString path = args.value("path").toString().trimmed();
        const int loads = p->loadsFinished();
        if (what == QLatin1String("reload")) {
            p->reloadPage();
        } else if (what == QLatin1String("back")) {
            p->goBack();
        } else {
            if (path.isEmpty())
                return error(QStringLiteral("pass path (a route like /about) or action reload / back"));
            const QUrl url(path);
            if (path.contains(QLatin1String("://")) && url.host() != QLatin1String("localhost") && url.host() != QLatin1String("127.0.0.1"))
                return error(QStringLiteral("the preview only opens the dev server's pages; pass a path like /about"));
            p->navigateTo(path);
        }
        const bool loaded = waitFor([p, loads] { return p->loadsFinished() > loads && !p->isLoading(); }, 15000);
        pause(300);
        QJsonObject o = idePreviewState(p);
        if (!loaded)
            o.insert("note", QStringLiteral("no page load finished within 15 s (a client-side route change does not reload the page)"));
        o.insert("summary", p->currentUrl().path().isEmpty() ? QStringLiteral("/") : p->currentUrl().path());
        return o;
    }
    if (action == QLatin1String("preview_click") || action == QLatin1String("preview_type")) {
        const QString selector = args.value("selector").toString();
        if (selector.trimmed().isEmpty())
            return error(QStringLiteral("pass a CSS selector"));
        const bool click = action == QLatin1String("preview_click");
        // In an isolated world (same DOM, the page's listeners fire). Typing goes through the native value setter and
        // input / change events, so frameworks that track the value (React) see it.
        const QString script = QStringLiteral(
            "(function(){const el=document.querySelector(%1);"
            "if(!el)return JSON.stringify({found:false});"
            "el.scrollIntoView({block:'center'});"
            "const what=el.tagName.toLowerCase()+(el.id?'#'+el.id:'')+' '+JSON.stringify((el.innerText||el.placeholder||el.getAttribute('aria-label')||el.name||'').trim().slice(0,60));"
            "if(%2){el.click();return JSON.stringify({found:true,what});}"
            "el.focus();const proto=el.tagName==='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;"
            "const set=Object.getOwnPropertyDescriptor(proto,'value');"
            "const text=%3;const value=(%4?'':(el.value||''))+text;"
            "if(set&&set.set&&('value' in el))set.set.call(el,value);else if(el.isContentEditable)el.textContent=value;else return JSON.stringify({found:true,what,error:'not a text field'});"
            "el.dispatchEvent(new Event('input',{bubbles:true}));el.dispatchEvent(new Event('change',{bubbles:true}));"
            "if(%5){const o={key:'Enter',code:'Enter',keyCode:13,which:13,bubbles:true,cancelable:true};"
            "const go=el.dispatchEvent(new KeyboardEvent('keydown',o));el.dispatchEvent(new KeyboardEvent('keyup',o));"
            "if(go&&el.form){if(el.form.requestSubmit)el.form.requestSubmit();else el.form.submit();}}"
            "return JSON.stringify({found:true,what,value:String(el.value===undefined?el.textContent:el.value).slice(0,200)});})()")
                                   .arg(jsString(selector), QLatin1String(click ? "true" : "false"), jsString(args.value("text").toString()),
                                        QLatin1String(args.value("append").toBool() ? "false" : "true"), QLatin1String(args.value("submit").toBool() ? "true" : "false"));
        const int loads = p->loadsFinished();
        bool timedOut = false;
        const QJsonObject r = QJsonDocument::fromJson(p->evaluate(script, 5000, false, &timedOut).toString().toUtf8()).object();
        if (timedOut)
            return error(QStringLiteral("the page did not answer within 5 s"));
        if (!r.value("found").toBool())
            return error(QStringLiteral("no element matches '%1'").arg(selector));
        if (r.contains("error"))
            return error(QStringLiteral("%1 is %2").arg(r.value("what").toString(), r.value("error").toString()));
        pause(600); // handlers, transitions, a navigation starting
        if (p->isLoading() || p->loadsFinished() > loads)
            waitFor([p] { return !p->isLoading(); }, 15000);
        QJsonObject o = idePreviewState(p);
        o.insert(click ? "clicked" : "typed_into", r.value("what").toString());
        if (!click)
            o.insert("value_now", r.value("value").toString());
        o.insert("summary", r.value("what").toString());
        return o;
    }

    if (action == QLatin1String("preview_eval")) {
        const QString code = args.value("code").toString();
        if (code.trimmed().isEmpty())
            return error(QStringLiteral("pass code"));
        // Indirect eval runs statements as well as expressions in the page's global scope; a promise is awaited. The
        // result is stored under a key of its own and polled, so a slow promise never blocks the window.
        static int counter = 0;
        const QString key = QStringLiteral("__qodeEval%1").arg(++counter);
        const QString start = QStringLiteral(
            "(function(){const k=%1;window[k]=null;"
            "Promise.resolve().then(()=>(0,eval)(%2)).then(v=>{let s;try{s=JSON.stringify(v===undefined?null:v);}catch(e){s=undefined;}"
            "if(s===undefined)s=JSON.stringify(String(v));window[k]={ok:true,value:s};},"
            "e=>{window[k]={ok:false,error:String(e&&e.stack||e)};});return true;})()")
                                  .arg(jsString(key), jsString(code));
        bool timedOut = false;
        p->evaluate(start, 5000, true, &timedOut);
        if (timedOut)
            return error(QStringLiteral("the page did not answer within 5 s"));
        QJsonObject result;
        const int seconds = qBound(1, args.value("timeout_seconds").toInt(10), 60);
        waitFor([&] {
            const QString got = p->evaluate(QStringLiteral("JSON.stringify(window[%1]||null)").arg(jsString(key)), 2000, true).toString();
            result = QJsonDocument::fromJson(got.toUtf8()).object();
            return !result.isEmpty();
        }, seconds * 1000);
        p->evaluate(QStringLiteral("delete window[%1]").arg(jsString(key)), 1000, true);
        if (result.isEmpty())
            return error(QStringLiteral("no result within %1 s (a promise that never settles?)").arg(seconds));
        if (!result.value("ok").toBool())
            return error(QStringLiteral("it threw: %1").arg(result.value("error").toString().left(2000)));
        QString value = result.value("value").toString();
        const int size = value.size();
        if (size > 8000)
            value = value.left(8000);
        QJsonObject o = idePreviewState(p);
        o.insert("value", value); // JSON text
        if (size > 8000)
            o.insert("clipped", QStringLiteral("the first 8000 of %1 characters").arg(size));
        o.insert("summary", value.simplified().left(60));
        return o;
    }
    return error(QStringLiteral("unknown preview action '%1'").arg(action));
}
