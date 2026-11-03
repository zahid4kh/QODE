#pragma once

#include "http/HttpEnv.h"
#include "http/HttpFile.h"
#include "http/HttpVars.h"
#include "settings/Theme.h"

#include <QHash>
#include <QPointer>
#include <QSet>
#include <QTextDocument>
#include <QWidget>

class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QStackedWidget;
class QTimer;
class QToolButton;
class HttpResponseView;
class HttpRunner;

// Right-hand panel for .http / .rest files: the requests of the file the editor shows, the environment picker, and the
// response of the last request sent (Ctrl+Enter or the gutter's play marker in the editor). Requests that read another
// request's response ({{login.response.body.$.token}}) send that one first when it has not answered yet. Responses live
// for the session; only the chosen environment is remembered (per project).
class HttpPanel : public QWidget
{
    Q_OBJECT
public:
    explicit HttpPanel(QWidget *parent = nullptr);

    void setProjectRoot(const QString &root);
    void setSpringBase(const QString &base); // {{$springBase}}; "" outside a Spring Boot project
    // The file the panel follows; nullptr shows how to start.
    void setDocument(const QString &path, QTextDocument *doc);
    QString documentPath() const { return m_doc ? m_path : QString(); }
    void sendAt(int line); // the request whose block holds `line` (0-based) of the current document
    void sendAll();
    bool isBusy() const { return m_busy; }
    void stopRun() { stop(); }
    // The environments of the current file's folder, and picking one as the environment list does (false: no such one).
    QStringList environments();
    bool selectEnvironment(const QString &name);
    QString currentEnvironment() const;
    // Why the last run stopped without sending (a variable that cannot be filled in, a prompt cancelled); "" otherwise.
    QString lastError() const { return m_lastError; }
    // Editor tooltip over a {{variable}} of `path` ("" when the column is not inside one).
    QString describeAt(const QString &path, const QString &text, int line, int column);

signals:
    void requestFinished(const HttpResult &result); // every request of a run that went out (the chat's http_request)
    void closeRequested();
    void locationRequested(const QString &path, int line); // 0-based
    void openTextRequested(const QString &text);
    void newFileRequested();

private:
    void applyTheme();
    void reparse();
    void populate();
    void populateVariables();
    void reloadEnvironments();
    HttpContext context(const QString &path, const HttpFileParse &parse) const;
    void queueWithDependencies(int index, QList<int> &queue, QSet<int> &seen) const;
    void start(const QList<int> &queue, bool all);
    void runNext();
    void finishedOne(const HttpResult &r, const HttpRequestSpec &spec);
    void stop();
    void setBusy(bool busy);
    void itemMenu(const QPoint &pos);
    void historyMenu();
    QString statusKey(const HttpRequestSpec &spec) const;

    Theme m_theme;
    QString m_root, m_springBase, m_path;
    QPointer<QTextDocument> m_doc;
    HttpFileParse m_parse;
    HttpEnvironments m_envs;

    // The run in progress: a snapshot of the file and the requests still to send.
    HttpFileParse m_runParse;
    QString m_runPath;
    QList<int> m_queue;
    int m_runTotal = 0, m_runDone = 0;
    bool m_busy = false;
    QString m_lastError;
    bool m_runAllMode = false; // "send all": a request that cannot be built does not stop the rest

    QHash<QString, QHash<QString, HttpResult>> m_named;   // file -> request name -> last response
    QHash<QString, QHash<QString, QPair<int, qint64>>> m_last; // file -> request key -> (status, ms); status -1 = not sent
    QHash<QString, QString> m_promptAnswers;
    QList<HttpResult> m_history;                            // newest first

    QLabel *m_logo, *m_title, *m_file, *m_envIcon;
    QComboBox *m_env;
    QToolButton *m_runAll, *m_stop, *m_cookies, *m_vars, *m_close, *m_historyBtn;
    QStackedWidget *m_listStack;
    QListWidget *m_items, *m_varList;
    HttpResponseView *m_response;
    HttpRunner *m_runner;
    QTimer *m_reparse;
};
