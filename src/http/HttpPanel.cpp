#include "http/HttpPanel.h"

#include "http/HttpResponseView.h"
#include "http/HttpRunner.h"
#include "http/HttpUi.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QNetworkReply>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kMaxHistory = 50;
using namespace HttpUi;

// ">> out.json": a free name unless ">>!" asked to overwrite.
QString saveTarget(const QString &dir, const QString &name, bool overwrite)
{
    const QString path = QDir(dir).absoluteFilePath(name);
    if (overwrite || !QFileInfo::exists(path))
        return path;
    const QFileInfo fi(path);
    for (int i = 1;; ++i) {
        const QString candidate = fi.path() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral("-%1").arg(i)
                                  + (fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix());
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
}

bool secretName(const QString &name)
{
    const QString n = name.toLower();
    return n.contains(QLatin1String("password")) || n.contains(QLatin1String("secret")) || n.contains(QLatin1String("token"));
}

} // namespace

HttpPanel::HttpPanel(QWidget *parent) : QWidget(parent), m_runner(new HttpRunner(this)), m_reparse(new QTimer(this))
{
    auto button = [this](const QString &tip, const QString &icon) {
        auto *b = new QToolButton(this);
        b->setToolTip(tip);
        b->setAutoRaise(true);
        Icons::bind(b, icon);
        return b;
    };

    m_logo = new QLabel(this);
    m_logo->setFixedSize(16, 16);
    m_title = new QLabel(tr("HTTP CLIENT"), this);
    m_title->setObjectName(QStringLiteral("panelTitle"));
    m_runAll = button(tr("Send all requests of the file, top to bottom"), QStringLiteral(":/new-icons/fast-forward.svg"));
    m_stop = button(tr("Stop"), QStringLiteral(":/new-icons/square.svg"));
    m_stop->hide();
    m_cookies = button(QString(), QStringLiteral(":/new-icons/cookie.svg"));
    m_vars = button(tr("Show the variables and what they are in the selected environment"), QStringLiteral(":/new-icons/variable.svg"));
    m_vars->setCheckable(true);
    m_close = button(tr("Close the HTTP client"), QStringLiteral(":/new-icons/x.svg"));

    auto *header = new QWidget(this);
    header->setObjectName(QStringLiteral("mediaHeader"));
    auto *hl = new QHBoxLayout(header);
    hl->setContentsMargins(8, 0, 4, 0);
    hl->setSpacing(2);
    hl->addWidget(m_logo);
    hl->addSpacing(4);
    hl->addWidget(m_title, 1);
    hl->addWidget(m_runAll);
    hl->addWidget(m_stop);
    hl->addWidget(m_cookies);
    hl->addWidget(m_vars);
    hl->addWidget(m_close);
    header->setStyleSheet(QStringLiteral("QWidget#mediaHeader { background: palette(alternate-base); border-bottom: 1px solid palette(shadow); }"
                                         "QWidget#mediaHeader QLabel { background: transparent; border: none; }"));

    m_file = new QLabel(this);
    m_file->setObjectName(QStringLiteral("emptyText"));
    m_file->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_envIcon = new QLabel(this);
    m_envIcon->setFixedSize(14, 14);
    m_env = new QComboBox(this);
    m_env->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    auto *fileRow = new QWidget(this);
    auto *fl = new QHBoxLayout(fileRow);
    fl->setContentsMargins(10, 6, 8, 6);
    fl->setSpacing(6);
    fl->addWidget(m_file, 1);
    fl->addWidget(m_envIcon);
    fl->addWidget(m_env);

    m_items = new QListWidget(this);
    m_varList = new QListWidget(this);
    for (QListWidget *l : {m_items, m_varList}) {
        l->setFrameShape(QFrame::NoFrame);
        l->setMouseTracking(true);
        l->setItemDelegate(new HttpRowDelegate(&m_theme, l));
    }
    m_items->setContextMenuPolicy(Qt::CustomContextMenu);
    m_listStack = new QStackedWidget(this);
    m_listStack->addWidget(m_items);
    m_listStack->addWidget(m_varList);

    m_response = new HttpResponseView(this);
    m_response->setPlaceholder(tr("Put the cursor in a request and press Ctrl+Enter, or click ▶ beside it."));
    m_historyBtn = new QToolButton(this);
    m_historyBtn->setAutoRaise(true);
    m_historyBtn->setToolTip(tr("Earlier responses of this session"));
    Icons::bind(m_historyBtn, QStringLiteral(":/new-icons/history.svg"));
    m_response->addTool(m_historyBtn);

    auto *split = new QSplitter(Qt::Vertical, this);
    split->setChildrenCollapsible(false);
    split->addWidget(m_listStack);
    split->addWidget(m_response);
    split->setSizes({180, 420});
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(header);
    layout->addWidget(fileRow);
    layout->addWidget(split, 1);

    m_reparse->setSingleShot(true);
    m_reparse->setInterval(300);
    connect(m_reparse, &QTimer::timeout, this, &HttpPanel::reparse);
    connect(m_close, &QToolButton::clicked, this, &HttpPanel::closeRequested);
    connect(m_runAll, &QToolButton::clicked, this, &HttpPanel::sendAll);
    connect(m_stop, &QToolButton::clicked, this, &HttpPanel::stop);
    connect(m_cookies, &QToolButton::clicked, this, [this] {
        m_runner->clearCookies();
        applyTheme();
    });
    connect(m_vars, &QToolButton::toggled, this, [this](bool on) {
        m_listStack->setCurrentWidget(on ? m_varList : m_items);
        if (on)
            populateVariables();
    });
    connect(m_env, &QComboBox::activated, this, [this] {
        SettingsManager::instance().setHttpEnvironment(m_env->currentData().toString());
        populateVariables();
    });
    connect(m_items, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        const int i = it->data(PayloadRole).toInt();
        if (it->data(HeaderRole).toBool())
            return;
        if (i >= 0 && i < m_parse.requests.size())
            emit locationRequested(m_path, m_parse.requests.at(i).line);
    });
    connect(m_items, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *it) {
        const int i = it->data(PayloadRole).toInt();
        if (!it->data(HeaderRole).toBool() && i >= 0 && i < m_parse.requests.size())
            sendAt(m_parse.requests.at(i).line);
    });
    connect(m_items, &QWidget::customContextMenuRequested, this, &HttpPanel::itemMenu);
    connect(m_historyBtn, &QToolButton::clicked, this, &HttpPanel::historyMenu);
    connect(m_response, &HttpResponseView::openTextRequested, this, &HttpPanel::openTextRequested);
    connect(m_response, &HttpResponseView::linkActivated, this, [this](const QString &link) {
        if (link == QLatin1String("new"))
            emit newFileRequested();
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &HttpPanel::applyTheme);

    applyTheme();
    populate();
}

void HttpPanel::applyTheme()
{
    m_theme = Theme::byName(SettingsManager::instance().theme());
    setStyleSheet(HttpUi::styleSheet(m_theme));
    m_logo->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/arrow-left-right.svg"), m_theme.accent, 16));
    m_envIcon->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/server.svg"), m_theme.textMuted, 14));
    const int cookies = m_runner->cookieCount();
    m_cookies->setToolTip(cookies ? tr("Forget the %n cookie(s) the servers set this session", nullptr, cookies)
                                  : tr("Cookies the servers set are sent back until you clear them (none stored now)"));
    m_items->viewport()->update();
    m_varList->viewport()->update();
}

void HttpPanel::setProjectRoot(const QString &root)
{
    m_root = root;
    m_named.clear();
    m_last.clear();
    reloadEnvironments();
}

void HttpPanel::setSpringBase(const QString &base)
{
    m_springBase = base;
}

void HttpPanel::setDocument(const QString &path, QTextDocument *doc)
{
    if (m_doc == doc && m_path == path)
        return;
    if (m_doc)
        disconnect(m_doc, nullptr, m_reparse, nullptr);
    m_doc = doc;
    m_path = path;
    if (doc) {
        connect(doc, &QTextDocument::contentsChanged, m_reparse, qOverload<>(&QTimer::start));
        connect(doc, &QObject::destroyed, m_reparse, qOverload<>(&QTimer::start)); // the tab was closed
    }
    reloadEnvironments();
    reparse();
}

void HttpPanel::reparse()
{
    m_parse = m_doc ? HttpFile::parse(m_doc->toPlainText()) : HttpFileParse();
    populate();
    if (m_vars->isChecked())
        populateVariables();
}

QStringList HttpPanel::environments()
{
    reloadEnvironments();
    return m_envs.names;
}

bool HttpPanel::selectEnvironment(const QString &name)
{
    reloadEnvironments();
    if (!name.isEmpty() && !m_envs.names.contains(name))
        return false;
    SettingsManager::instance().setHttpEnvironment(name);
    reloadEnvironments();
    if (m_vars->isChecked())
        populateVariables();
    return true;
}

QString HttpPanel::currentEnvironment() const
{
    return m_env->currentData().toString();
}

void HttpPanel::reloadEnvironments()
{
    const QString dir = m_path.isEmpty() ? m_root : QFileInfo(m_path).path();
    m_envs = HttpEnv::load(dir, m_root);
    const QString wanted = SettingsManager::instance().httpEnvironment();
    m_env->clear();
    m_env->addItem(tr("No environment"), QString());
    for (const QString &n : std::as_const(m_envs.names))
        m_env->addItem(n, n);
    const int i = m_env->findData(wanted);
    m_env->setCurrentIndex(i < 0 ? 0 : i);
    QStringList files;
    for (const QString &s : std::as_const(m_envs.sources))
        files << QDir::toNativeSeparators(m_root.isEmpty() ? s : QDir(m_root).relativeFilePath(s));
    m_env->setToolTip(files.isEmpty() ? tr("No environments: add http-client.env.json (IntelliJ) or rest-client.environmentVariables in "
                                           ".vscode/settings.json (VS Code)")
                                      : tr("Environments from %1").arg(files.join(QStringLiteral(", "))));
    m_env->setVisible(!m_envs.names.isEmpty() || !m_envs.shared.isEmpty());
    m_envIcon->setVisible(m_env->isVisible());
}

HttpContext HttpPanel::context(const QString &path, const HttpFileParse &parse) const
{
    HttpContext c;
    c.dir = QFileInfo(path).path();
    c.envName = m_env->currentData().toString();
    c.env = m_envs.vars(c.envName);
    for (const HttpFileVar &v : parse.variables)
        c.fileVars.insert(v.name, v.value);
    c.prompts = m_promptAnswers;
    c.dotenv = HttpEnv::dotenv(c.dir, m_root);
    c.responses = m_named.value(path);
    c.springBase = m_springBase;
    return c;
}

QString HttpPanel::statusKey(const HttpRequestSpec &spec) const
{
    return spec.label();
}

void HttpPanel::populate()
{
    m_items->setUpdatesEnabled(false);
    m_items->clear();
    auto note = [this](const QString &text) {
        auto *it = new QListWidgetItem(text, m_items);
        it->setFlags(Qt::NoItemFlags);
        it->setData(HeaderRole, true);
    };
    if (!m_doc) {
        m_file->setText(tr("No .http file open"));
        note(tr("Open a .http or .rest file"));
        m_response->setPlaceholder(tr("Requests are written in .http files, the format of VS Code's REST Client and IntelliJ:\n\n"
                                      "### Get a user\nGET https://example.com/users/1\nAccept: application/json\n\n"
                                      "File > New HTTP Request File… starts one."));
    } else {
        const auto &reqs = m_parse.requests;
        m_file->setText(tr("%1 · %n request(s)", nullptr, int(reqs.size())).arg(QFileInfo(m_path).fileName()));
        m_file->setToolTip(QDir::toNativeSeparators(m_path));
        m_response->setPlaceholder(tr("Put the cursor in a request and press Ctrl+Enter, or click ▶ beside it."));
        const auto last = m_last.value(m_path);
        for (int i = 0; i < reqs.size(); ++i) {
            const HttpRequestSpec &r = reqs.at(i);
            auto *it = new QListWidgetItem(!r.title.isEmpty() ? r.title : !r.name.isEmpty() ? r.name : r.url, m_items);
            it->setData(MethodRole, r.method);
            it->setData(PayloadRole, i);
            it->setToolTip(QStringLiteral("%1 %2\n%3%4")
                               .arg(r.method, r.url, r.name.isEmpty() ? QString() : QStringLiteral("@name %1 · ").arg(r.name),
                                    tr("line %1 · double-click to send").arg(r.line + 1)));
            const auto s = last.constFind(statusKey(r));
            if (s != last.constEnd()) {
                it->setData(StatusRole, s->first);
                it->setData(DetailRole, s->first > 0 ? QStringLiteral("%1 · %2 ms").arg(s->first).arg(s->second)
                                                     : s->first == 0 ? tr("failed") : tr("not sent"));
            }
        }
        if (reqs.isEmpty())
            note(tr("No requests yet. Write one: GET https://…"));
    }
    m_runAll->setEnabled(m_doc && m_parse.requests.size() > 0 && !m_runner->busy());
    m_items->setUpdatesEnabled(true);
}

void HttpPanel::populateVariables()
{
    m_varList->setUpdatesEnabled(false);
    m_varList->clear();
    const HttpContext ctx = context(m_path, m_parse);
    HttpResolver resolver(ctx);
    auto header = [this](const QString &text) {
        auto *it = new QListWidgetItem(text, m_varList);
        it->setFlags(Qt::NoItemFlags);
        it->setData(HeaderRole, true);
    };
    auto row = [this, &resolver](const QString &name, const QString &raw) {
        QString value;
        const bool ok = resolver.lookup(name, &value);
        auto *it = new QListWidgetItem(name, m_varList);
        it->setData(DetailRole, ok ? value : tr("%1 (not defined)").arg(raw));
        if (!ok)
            it->setData(StatusRole, 500); // shown in the error colour
        it->setToolTip(QStringLiteral("%1 = %2").arg(name, ok ? value : raw));
    };
    if (!m_parse.variables.isEmpty()) {
        header(tr("This file"));
        QSet<QString> seen;
        for (int i = int(m_parse.variables.size()) - 1; i >= 0; --i) // the last definition wins
            if (!seen.contains(m_parse.variables.at(i).name))
                seen.insert(m_parse.variables.at(i).name);
        for (const HttpFileVar &v : m_parse.variables)
            if (seen.remove(v.name))
                row(v.name, ctx.fileVars.value(v.name));
    }
    if (!ctx.env.isEmpty()) {
        header(ctx.envName.isEmpty() ? tr("Shared") : tr("Environment %1").arg(ctx.envName));
        QStringList names = ctx.env.keys();
        names.sort(Qt::CaseInsensitive);
        for (const QString &n : std::as_const(names))
            row(n, ctx.env.value(n));
    }
    if (!ctx.responses.isEmpty()) {
        header(tr("Named responses"));
        for (auto it = ctx.responses.cbegin(); it != ctx.responses.cend(); ++it) {
            auto *item = new QListWidgetItem(it.key(), m_varList);
            item->setData(DetailRole, it.value().ok() ? QString::number(it.value().status) : tr("failed"));
            item->setData(StatusRole, it.value().status);
            item->setToolTip(tr("{{%1.response.body.$.field}}, {{%1.response.headers.Name}}").arg(it.key()));
        }
    }
    if (m_varList->count() == 0) {
        header(tr("No variables"));
        auto *hint = new QListWidgetItem(tr("@name = value in the file, or an environment file"), m_varList);
        hint->setFlags(Qt::NoItemFlags);
    }
    m_varList->setUpdatesEnabled(true);
}

void HttpPanel::queueWithDependencies(int index, QList<int> &queue, QSet<int> &seen) const
{
    if (seen.contains(index))
        return;
    seen.insert(index);
    const auto responses = m_named.value(m_path);
    for (const QString &name : HttpVars::referencedNames(m_parse.requests.at(index))) {
        const int dep = m_parse.indexOfName(name);
        if (dep >= 0 && dep != index && !responses.contains(name))
            queueWithDependencies(dep, queue, seen);
    }
    queue << index;
}

void HttpPanel::sendAt(int line)
{
    if (!m_doc)
        return;
    m_reparse->stop();
    m_parse = HttpFile::parse(m_doc->toPlainText());
    populate();
    const int index = m_parse.requestAt(line);
    if (index < 0) {
        m_response->showMessage(tr("No request at line %1. A request starts with a line like GET https://…").arg(line + 1), m_theme.warning);
        return;
    }
    QList<int> queue;
    QSet<int> seen;
    queueWithDependencies(index, queue, seen);
    start(queue, false);
}

void HttpPanel::sendAll()
{
    if (!m_doc)
        return;
    m_reparse->stop();
    m_parse = HttpFile::parse(m_doc->toPlainText());
    populate();
    QList<int> queue;
    for (int i = 0; i < m_parse.requests.size(); ++i)
        queue << i;
    start(queue, true);
}

void HttpPanel::start(const QList<int> &queue, bool all)
{
    stop();
    m_runAllMode = all;
    reloadEnvironments(); // the environment files may have been edited
    m_runParse = m_parse;
    m_runPath = m_path;
    m_queue = queue;
    m_runTotal = int(queue.size());
    m_runDone = 0;
    m_lastError.clear();
    setBusy(true);
    runNext();
}

void HttpPanel::runNext()
{
    if (m_queue.isEmpty()) {
        setBusy(false);
        return;
    }
    const int index = m_queue.takeFirst();
    const HttpRequestSpec spec = m_runParse.requests.at(index);
    for (const auto &p : spec.prompts) {
        bool ok = false;
        const QString answer = QInputDialog::getText(this, tr("Value for %1").arg(p.first), p.second.isEmpty() ? p.first + QLatin1Char(':') : p.second,
                                                     secretName(p.first) ? QLineEdit::Password : QLineEdit::Normal,
                                                     m_promptAnswers.value(p.first), &ok);
        if (!ok) {
            m_queue.clear();
            m_lastError = tr("the user cancelled the prompt for %1").arg(p.first);
            m_response->showMessage(tr("Cancelled."), m_theme.textMuted);
            setBusy(false);
            return;
        }
        m_promptAnswers.insert(p.first, answer);
    }
    HttpRequestData data;
    QString error, note;
    if (!HttpVars::build(spec, context(m_runPath, m_runParse), &data, &error, &note)) {
        m_last[m_runPath].insert(statusKey(spec), {-1, 0});
        m_lastError = QStringLiteral("%1: %2").arg(spec.label(), error);
        ++m_runDone;
        m_response->showMessage(QStringLiteral("<b>%1</b><br>%2").arg(spec.label().toHtmlEscaped(), error.toHtmlEscaped()), m_theme.danger);
        populate();
        // In a dependency chain the rest cannot go out without this one; "send all" goes on with the next request.
        if (m_runAllMode)
            QTimer::singleShot(0, this, &HttpPanel::runNext);
        else {
            m_queue.clear();
            setBusy(false);
        }
        return;
    }
    HttpResult base;
    base.name = spec.name;
    base.label = spec.label();
    base.file = m_runPath;
    base.requestLine = spec.line;
    base.note = note;
    const QString progress = m_runTotal > 1 ? QStringLiteral(" (%1/%2)").arg(m_runDone + 1).arg(m_runTotal) : QString();
    m_response->showBusy(tr("Sending %1 %2 …%3").arg(data.method, data.url.toString(), progress));
    m_runner->send(data, base, [this, spec](const HttpResult &r) { finishedOne(r, spec); });
}

void HttpPanel::finishedOne(const HttpResult &result, const HttpRequestSpec &spec)
{
    HttpResult r = result;
    ++m_runDone;
    if (r.ok() && !spec.saveTo.isEmpty()) {
        const QString path = saveTarget(QFileInfo(m_runPath).path(), spec.saveTo, spec.overwrite);
        QFile f(path);
        const bool written = f.open(QIODevice::WriteOnly) && f.write(r.body) == r.body.size();
        const QString saved = written ? tr("Body saved to %1").arg(QDir::toNativeSeparators(path))
                                      : tr("Could not write %1").arg(QDir::toNativeSeparators(path));
        r.note = r.note.isEmpty() ? saved : r.note + QLatin1Char('\n') + saved;
    }
    if (!spec.name.isEmpty() && r.ok())
        m_named[m_runPath].insert(spec.name, r);
    m_last[m_runPath].insert(statusKey(spec), {r.status, r.ms});
    const bool cancelled = !r.ok() && r.error == QNetworkReply::OperationCanceledError;
    if (!cancelled) {
        m_history.prepend(r);
        while (m_history.size() > kMaxHistory)
            m_history.removeLast();
    }
    m_response->showResult(r);
    emit requestFinished(r);
    if (m_path == m_runPath)
        populate();
    if (m_vars->isChecked())
        populateVariables();
    applyTheme(); // the cookie count in the tooltip
    if (cancelled || (!r.ok() && !m_runAllMode)) { // the requests waiting for this one's response cannot go out
        m_queue.clear();
        setBusy(false);
        return;
    }
    // runNext may open a prompt dialog: not from inside the reply's signal.
    QTimer::singleShot(0, this, &HttpPanel::runNext);
}

void HttpPanel::stop()
{
    m_queue.clear();
    m_runner->abort();
    setBusy(false);
}

void HttpPanel::setBusy(bool busy)
{
    m_busy = busy;
    m_stop->setVisible(busy);
    m_runAll->setVisible(!busy);
    m_runAll->setEnabled(m_doc && m_parse.requests.size() > 0);
}

QString HttpPanel::describeAt(const QString &path, const QString &text, int line, int column)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    if (line < 0 || line >= lines.size() || !lines.at(line).contains(QLatin1String("{{")))
        return {};
    return HttpVars::describeAt(lines.at(line), column, context(path, HttpFile::parse(text)));
}

void HttpPanel::itemMenu(const QPoint &pos)
{
    QListWidgetItem *it = m_items->itemAt(pos);
    if (!it || it->data(HeaderRole).toBool())
        return;
    const int i = it->data(PayloadRole).toInt();
    if (i < 0 || i >= m_parse.requests.size())
        return;
    const HttpRequestSpec spec = m_parse.requests.at(i);
    QMenu menu(this);
    menu.addAction(tr("Send"), this, [this, spec] { sendAt(spec.line); });
    menu.addAction(tr("Go to Request"), this, [this, spec] { emit locationRequested(m_path, spec.line); });
    menu.addSeparator();
    menu.addAction(tr("Copy as cURL"), this, [this, spec] {
        HttpRequestData data;
        QString error, note;
        if (HttpVars::build(spec, context(m_path, m_parse), &data, &error, &note))
            QApplication::clipboard()->setText(HttpUi::curl(data));
        else
            m_response->showMessage(error.toHtmlEscaped(), m_theme.danger);
    });
    menu.addAction(tr("Copy URL"), this, [this, spec] {
        HttpResolver r(context(m_path, m_parse));
        QApplication::clipboard()->setText(r.resolve(spec.url));
    });
    menu.exec(m_items->viewport()->mapToGlobal(pos));
}

void HttpPanel::historyMenu()
{
    QMenu menu(this);
    if (m_history.isEmpty())
        menu.addAction(tr("No responses yet"))->setEnabled(false);
    for (int i = 0; i < m_history.size(); ++i) {
        const HttpResult &r = m_history.at(i);
        const QString status = r.ok() ? QString::number(r.status) : tr("failed");
        QAction *a = menu.addAction(QStringLiteral("%1  %2 %3   %4").arg(status, r.request.method, r.label, r.time.toString(QStringLiteral("HH:mm:ss"))));
        a->setToolTip(r.request.url.toString());
        connect(a, &QAction::triggered, this, [this, i] {
            if (i < m_history.size())
                m_response->showResult(m_history.at(i));
        });
    }
    if (!m_history.isEmpty()) {
        menu.addSeparator();
        menu.addAction(tr("Clear History"), this, [this] { m_history.clear(); });
    }
    menu.setToolTipsVisible(true);
    menu.exec(m_historyBtn->mapToGlobal(QPoint(0, m_historyBtn->height())));
}
