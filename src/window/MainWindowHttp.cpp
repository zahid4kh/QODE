// .http / .rest request files: the editor's run markers and Ctrl+Enter, the HTTP client panel in the right-hand island.
#include "window/MainWindow.h"

#include "app/StartupProfile.h"
#include "dialogs/FileDialogs.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "http/HttpFile.h"
#include "http/HttpPanel.h"
#include "media/MarkdownPreview.h"
#include "media/MediaPanel.h"
#include "project/ProjectManager.h"
#include "settings/SettingsManager.h"
#include "spring/SpringBar.h"
#include "spring/SpringServer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTextDocument>
#include <QTimer>

void MainWindow::setupHttp()
{
    connect(m_editors, &EditorManager::currentChanged, this, &MainWindow::followHttp);
    connect(m_editors, &EditorManager::documentPathChanged, this, [this](Document *doc) {
        updateHttpMarkers(doc); // renamed to or away from .http
        followHttp();
    });
    connect(m_projects, &ProjectManager::projectOpened, this, [this] {
        if (m_http)
            m_http->setProjectRoot(m_projects->root());
    });
    connect(m_projects, &ProjectManager::projectClosed, this, [this] {
        if (m_http)
            m_http->setProjectRoot(QString());
    });
}

// Created when first used: a request sent, the panel opened, or a tooltip in a .http file.
HttpPanel *MainWindow::httpPanel()
{
    if (m_http)
        return m_http;
    StartupProfile::Later timing(QStringLiteral("HTTP panel"));
    m_http = new HttpPanel(this);
    m_rightStack->addWidget(m_http);
    connect(m_http, &HttpPanel::closeRequested, this, &MainWindow::hideMedia);
    connect(m_http, &HttpPanel::locationRequested, this, [this](const QString &path, int line) {
        m_editorHiddenByUser = false;
        if (m_editors->openFileAt(path, line + 1))
            setEditorColumnVisible(true);
    });
    connect(m_http, &HttpPanel::openTextRequested, this, &MainWindow::openTextInEditor);
    connect(m_http, &HttpPanel::newFileRequested, this, &MainWindow::newHttpFile);
    connect(m_http, &HttpPanel::requestFinished, this, &MainWindow::noteHttpForChat);
    if (m_projects->hasProject())
        m_http->setProjectRoot(m_projects->root());
    return m_http;
}

QString MainWindow::springBaseUrl() const
{
    if (!m_springBar || !m_springBar->isAvailable())
        return {};
    const SpringServer *server = m_springBar->server();
    const int port = server && server->port() > 0 ? server->port() : m_springBar->options().port > 0 ? m_springBar->options().port : 8080;
    return QStringLiteral("http://localhost:%1").arg(port);
}

void MainWindow::wireHttp(Document *doc, CodeEditor *ed)
{
    connect(doc->textDocument(), &QTextDocument::contentsChanged, this, [this, doc] { updateHttpMarkers(doc); });
    connect(ed, &CodeEditor::runRequested, this, [this, doc](int line) { sendHttp(doc, line); });
    ed->setTooltipProvider([this, doc](int line, int column) {
        if (!HttpFile::isHttpFile(doc->filePath()))
            return QString();
        httpPanel()->setSpringBase(springBaseUrl());
        return m_http->describeAt(doc->filePath(), doc->text(), line, column);
    });
    updateHttpMarkers(doc);
}

void MainWindow::updateHttpMarkers(Document *doc)
{
    CodeEditor *ed = m_editors->editorFor(doc);
    if (!ed)
        return;
    if (!HttpFile::isHttpFile(doc->filePath())) {
        updateMainMarkers(doc); // Kotlin / Java main functions, or none
        return;
    }
    ed->setRunMarkers(HttpFile::parse(doc->text()).requestLines(), tr("Send this request (Ctrl+Enter)"));
}

void MainWindow::sendHttp(Document *doc, int line)
{
    if (!HttpFile::isHttpFile(doc->filePath()))
        return;
    httpPanel()->setSpringBase(springBaseUrl());
    m_http->setDocument(doc->filePath(), doc->textDocument());
    showHttpPanel();
    m_http->sendAt(line);
}

// The panel lists the requests of the .http file in the active tab; other files leave it on the last one.
void MainWindow::followHttp()
{
    Document *doc = m_editors->currentDocument();
    if (!m_http || !doc || !HttpFile::isHttpFile(doc->filePath()))
        return;
    m_http->setSpringBase(springBaseUrl());
    m_http->setDocument(doc->filePath(), doc->textDocument());
}

void MainWindow::showHttpPanel()
{
    if (!rightPanelShows(m_http))
        showInRightPanel(httpPanel());
}

void MainWindow::toggleHttpPanel()
{
    if (rightPanelShows(m_http)) {
        hideMedia();
        return;
    }
    httpPanel();
    followHttp();
    showHttpPanel();
}

void MainWindow::newHttpFile()
{
    const QString root = m_projects->hasProject() ? m_projects->root() : SettingsManager::instance().lastDirectory();
    QString path = FileDialogs::getSaveFileName(this, tr("New HTTP Request File"), QDir(root).filePath(QStringLiteral("requests.http")),
                                                tr("HTTP requests (*.http *.rest)"));
    if (path.isEmpty())
        return;
    if (!HttpFile::isHttpFile(path))
        path += QStringLiteral(".http");
    if (!QFileInfo::exists(path)) {
        const QString base = springBaseUrl().isEmpty() ? QStringLiteral("http://localhost:8080") : springBaseUrl();
        const QString text = tr("# Ctrl+Enter (or the ▶ beside a request) sends the request under the cursor.\n"
                                "# Requests are separated by ###. {{name}} is a variable: @name = value here, or an environment in\n"
                                "# http-client.env.json. # @name login lets later requests read {{login.response.body.$.token}}.\n"
                                "\n"
                                "@baseUrl = %1\n"
                                "\n"
                                "### A first request\n"
                                "GET {{baseUrl}}/\n"
                                "Accept: application/json\n")
                                 .arg(base);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0) {
            QMessageBox::warning(this, tr("New HTTP Request File"), tr("Could not create %1").arg(QDir::toNativeSeparators(path)));
            return;
        }
    }
    SettingsManager::instance().setLastDirectory(QFileInfo(path).path());
    m_editorHiddenByUser = false;
    if (!m_editors->openFile(path))
        return;
    setEditorColumnVisible(true);
    followHttp();
    showHttpPanel();
}

// "Open in editor" of a response: a new untitled tab with the text.
void MainWindow::openTextInEditor(const QString &text)
{
    m_editorHiddenByUser = false;
    m_editors->newUntitled();
    Document *doc = m_editors->currentDocument();
    if (CodeEditor *ed = doc ? m_editors->editorFor(doc) : nullptr) {
        ed->setPlainText(text);
        ed->document()->setModified(false);
    }
    setEditorColumnVisible(true);
}


// --- the chat's http_request (group "http") --------------------------------------------------------------------------

namespace {
// A response as the model gets it: status, time, headers (credentials masked), the body as text (JSON indented)
// clipped to 12 KB.
QJsonObject resultJson(const HttpResult &r)
{
    QJsonObject o{{"request", QStringLiteral("%1 %2").arg(r.request.method, r.request.url.toString())}, {"label", r.label}};
    if (!r.ok()) {
        o.insert("error", r.errorText.isEmpty() ? QStringLiteral("no response") : r.errorText); // "error" inside a nested object is fine
        return o;
    }
    o.insert("status", r.status);
    o.insert("reason", r.reason);
    o.insert("ms", r.ms);
    QJsonObject headers;
    static const QList<QByteArray> hidden{"set-cookie", "cookie", "authorization", "proxy-authorization", "x-api-key"};
    for (const auto &h : r.headers)
        headers.insert(QString::fromLatin1(h.first), hidden.contains(h.first.toLower()) ? QStringLiteral("[hidden]") : QString::fromLatin1(h.second));
    o.insert("headers", headers);
    const QString type = r.contentType();
    QString body;
    if (type.contains(QLatin1String("json"))) {
        const QJsonDocument doc = QJsonDocument::fromJson(r.body);
        body = doc.isNull() ? QString::fromUtf8(r.body) : QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    } else if (type.isEmpty() || type.startsWith(QLatin1String("text/")) || type.contains(QLatin1String("xml")) || type.contains(QLatin1String("javascript"))
               || type.contains(QLatin1String("x-www-form-urlencoded"))) {
        body = QString::fromUtf8(r.body);
    } else {
        body = QStringLiteral("(%1 bytes of %2, not shown)").arg(r.body.size()).arg(type);
    }
    if (body.size() > 12000) {
        o.insert("body_clipped", QStringLiteral("the first 12000 of %1 characters").arg(body.size()));
        body = body.left(12000);
    }
    o.insert("body", body);
    if (!r.note.isEmpty())
        o.insert("note", r.note);
    return o;
}
} // namespace

void MainWindow::noteHttpForChat(const HttpResult &r)
{
    if (m_httpForChat)
        m_httpChatResults.append(QJsonObject{{"line", r.requestLine}, {"label", r.label}, {"json", resultJson(r)}});
}

QJsonObject MainWindow::ideHttpSend(const QString &path, const QString &request, int line, const QString &environment)
{
    Document *doc = m_editors->documentForPath(path);
    if (!doc) {
        if (!m_editors->openFile(path))
            return QJsonObject{{"error", QStringLiteral("the file could not be opened")}};
        doc = m_editors->documentForPath(path);
    }
    if (!doc || !HttpFile::isHttpFile(path))
        return QJsonObject{{"error", QStringLiteral("not a .http / .rest file")}};
    const HttpFileParse parse = HttpFile::parse(doc->text());
    int index = line > 0 ? parse.requestAt(line - 1) : -1;
    if (index < 0 && !request.trimmed().isEmpty()) {
        const QString want = request.trimmed();
        for (int i = 0; i < parse.requests.size() && index < 0; ++i) {
            const HttpRequestSpec &r = parse.requests.at(i);
            if (r.name.compare(want, Qt::CaseInsensitive) == 0 || r.title.trimmed().compare(want, Qt::CaseInsensitive) == 0
                || r.label().compare(want, Qt::CaseInsensitive) == 0)
                index = i;
        }
    }
    if (index < 0) {
        QStringList list;
        for (const HttpRequestSpec &r : parse.requests)
            list << QStringLiteral("%1 (line %2)").arg(r.label()).arg(r.line + 1);
        return QJsonObject{{"error", parse.requests.isEmpty() ? QStringLiteral("the file has no requests")
                                                              : QStringLiteral("no such request; the file has: %1").arg(list.join(QStringLiteral("; ")))}};
    }
    const HttpRequestSpec &spec = parse.requests.at(index);
    httpPanel()->setSpringBase(springBaseUrl());
    m_http->setDocument(path, doc->textDocument());
    if (!environment.isEmpty() && !m_http->selectEnvironment(environment)) {
        const QStringList envs = m_http->environments();
        return QJsonObject{{"error", envs.isEmpty() ? QStringLiteral("no environments are defined for this file")
                                                    : QStringLiteral("no environment '%1'; available: %2").arg(environment, envs.join(QStringLiteral(", ")))}};
    }
    showHttpPanel();
    m_httpChatResults = QJsonArray();
    m_httpForChat = true;
    m_httpChatLine = spec.line;
    m_http->sendAt(spec.line);
    return QJsonObject{{"label", spec.label()}, {"summary", QStringLiteral("sending")}};
}

QJsonObject MainWindow::ideHttpPoll()
{
    if (m_http && m_http->isBusy())
        return QJsonObject{{"busy", true}};
    m_httpForChat = false;
    QJsonObject out;
    QJsonArray before;
    for (const QJsonValue &v : std::as_const(m_httpChatResults)) {
        const QJsonObject r = v.toObject();
        if (r.value("line").toInt() == m_httpChatLine)
            out = r.value("json").toObject();
        else
            before.append(r.value("label")); // requests it depends on, sent first
    }
    if (out.isEmpty()) {
        QString why = m_http && !m_http->lastError().isEmpty() ? QStringLiteral("the request did not go out: %1").arg(m_http->lastError())
                                                              : QStringLiteral("the request did not go out");
        const QStringList envs = m_http ? m_http->environments() : QStringList();
        if (!envs.isEmpty())
            why += QStringLiteral(" (environment: %1; available: %2)")
                       .arg(m_http->currentEnvironment().isEmpty() ? QStringLiteral("none") : m_http->currentEnvironment(), envs.join(QStringLiteral(", ")));
        return QJsonObject{{"error", why}};
    }
    if (!before.isEmpty())
        out.insert("sent_first", before);
    if (m_http && !m_http->currentEnvironment().isEmpty())
        out.insert("environment", m_http->currentEnvironment());
    return QJsonObject{{"response", out}};
}

void MainWindow::ideHttpStop()
{
    if (m_http && m_http->isBusy())
        m_http->stopRun();
    m_httpForChat = false;
}
