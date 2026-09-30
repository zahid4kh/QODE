#include "GitRepository.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace {

// Everything after the n-th space of a porcelain v2 record (paths may contain spaces).
QString fieldsTail(const QByteArray &rec, int spaces)
{
    int pos = -1;
    for (int i = 0; i < spaces; ++i) {
        pos = rec.indexOf(' ', pos + 1);
        if (pos < 0)
            return {};
    }
    return QString::fromUtf8(rec.mid(pos + 1));
}

QString messageOf(const GitRepository::Result &r)
{
    const QString err = r.err.trimmed();
    return err.isEmpty() ? QString::fromUtf8(r.out).trimmed() : err;
}

} // namespace

GitRepository::GitRepository(QObject *parent)
    : QObject(parent)
{
    m_gitExe = QStandardPaths::findExecutable(QStringLiteral("git"));
    m_timer = new QTimer(this);
    m_timer->setSingleShot(true);
    m_timer->setInterval(350);
    connect(m_timer, &QTimer::timeout, this, &GitRepository::refresh);
    m_watcher = new QFileSystemWatcher(this);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, &GitRepository::onGitPathChanged);
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, &GitRepository::onGitPathChanged);
}

// --- Process plumbing ---------------------------------------------------------

void GitRepository::run(const QStringList &args, QObject *ctx, Callback cb, const QByteArray &input, int timeoutMs)
{
    auto *p = new QProcess(this);
    p->setProgram(m_gitExe.isEmpty() ? QStringLiteral("git") : m_gitExe);
    p->setArguments(args);
    p->setWorkingDirectory(m_root.isEmpty() ? m_workDir : m_root);

    // Never block on prompts or editors: fail with a message instead.
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("GIT_EDITOR"), QStringLiteral("true"));
    env.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    env.insert(QStringLiteral("LC_MESSAGES"), QStringLiteral("C"));
    env.insert(QStringLiteral("LANGUAGE"), QStringLiteral("C"));
    if (!env.contains(QStringLiteral("GIT_SSH_COMMAND")))
        env.insert(QStringLiteral("GIT_SSH_COMMAND"), QStringLiteral("ssh -o BatchMode=yes"));
    p->setProcessEnvironment(env);

    QObject *receiver = ctx ? ctx : this;
    connect(p, &QProcess::finished, receiver, [p, cb](int code, QProcess::ExitStatus status) {
        Result r;
        r.exitCode = status == QProcess::NormalExit ? code : -1;
        r.out = p->readAllStandardOutput();
        r.err = QString::fromUtf8(p->readAllStandardError());
        if (cb)
            cb(r);
    });
    connect(p, &QProcess::errorOccurred, receiver, [p, cb](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        Result r;
        r.err = QObject::tr("Unable to start git: %1").arg(p->errorString());
        if (cb)
            cb(r);
        p->deleteLater();
    });
    connect(p, &QProcess::finished, p, &QObject::deleteLater);

    p->start();
    if (!input.isEmpty())
        p->write(input);
    p->closeWriteChannel();
    if (timeoutMs > 0) {
        QTimer::singleShot(timeoutMs, p, [p] {
            if (p->state() != QProcess::NotRunning)
                p->kill();
        });
    }
}

void GitRepository::setBusy(int delta)
{
    const bool was = m_busy > 0;
    m_busy = qMax(0, m_busy + delta);
    if (was != (m_busy > 0))
        emit busyChanged(m_busy > 0);
}

void GitRepository::runOp(const QString &title, const QList<QStringList> &steps, const QByteArray &input, int timeoutMs,
                          std::function<void(bool)> done)
{
    if (!m_isRepo && steps.value(0).value(0) != QLatin1String("init"))
        return;
    setBusy(+1);
    runStep(title, steps, 0, input, timeoutMs, std::move(done));
}

void GitRepository::runStep(const QString &title, const QList<QStringList> &steps, int index, const QByteArray &input,
                            int timeoutMs, std::function<void(bool)> done)
{
    run(steps.at(index), this,
        [this, title, steps, index, timeoutMs, done](const Result &r) {
            if (!r.ok()) {
                finishOp(title, false, messageOf(r), done);
                return;
            }
            if (index + 1 < steps.size())
                runStep(title, steps, index + 1, {}, timeoutMs, done);
            else
                finishOp(title, true, QString::fromUtf8(r.out).trimmed() + (r.err.trimmed().isEmpty() ? QString() : QLatin1Char('\n') + r.err.trimmed()), done);
        },
        input, timeoutMs);
}

void GitRepository::finishOp(const QString &title, bool ok, const QString &text, const std::function<void(bool)> &done)
{
    setBusy(-1);
    if (ok)
        emit operationFinished(title, text.trimmed());
    else
        emit errorOccurred(title, text.isEmpty() ? tr("Git reported an error but printed no message.") : text);
    if (done)
        done(ok);
    refresh();
}

// --- Repository detection ------------------------------------------------------

void GitRepository::setWorkDirectory(const QString &dir)
{
    m_timer->stop();
    ++m_generation;
    m_workDir = dir;
    m_root.clear();
    m_gitDir.clear();
    m_isRepo = false;
    m_resolved = false;
    m_refreshAgain = false;
    m_changes.clear();
    m_changeIndex.clear();
    m_folderKinds.clear();
    m_ignored.clear();
    m_branches.clear();
    m_branch.clear();
    m_upstream.clear();
    m_headOid.clear();
    m_ahead = m_behind = 0;
    clearWatches();
    emit repositoryChanged();
    emit statusChanged();
    if (dir.isEmpty() || m_gitExe.isEmpty()) {
        m_resolved = true;
        m_refreshing = false;
        return;
    }
    discover();
}

void GitRepository::discover()
{
    m_refreshing = true;
    const int gen = m_generation;
    run({QStringLiteral("rev-parse"), QStringLiteral("--show-toplevel"), QStringLiteral("--absolute-git-dir")}, this,
        [this, gen](const Result &r) {
            if (gen != m_generation)
                return; // superseded by another project
            m_refreshing = false;
            const bool wasResolved = m_resolved;
            m_resolved = true;
            if (!r.ok()) {
                if (m_isRepo)
                    markNotRepo();
                else if (!wasResolved)
                    emit repositoryChanged();
                return;
            }
            const QStringList lines = QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            if (lines.size() < 2) {
                markNotRepo();
                return;
            }
            m_root = QDir::cleanPath(lines.at(0));
            m_gitDir = QDir::cleanPath(lines.at(1));
            m_isRepo = true;
            watchGitDir();
            emit repositoryChanged();
            refresh();
        });
}

void GitRepository::markNotRepo()
{
    m_isRepo = false;
    m_root.clear();
    m_changes.clear();
    m_changeIndex.clear();
    m_folderKinds.clear();
    m_ignored.clear();
    m_branches.clear();
    m_branch.clear();
    m_headOid.clear();
    clearWatches();
    emit repositoryChanged();
    emit statusChanged();
}

void GitRepository::clearWatches()
{
    if (!m_watcher->files().isEmpty())
        m_watcher->removePaths(m_watcher->files());
    if (!m_watcher->directories().isEmpty())
        m_watcher->removePaths(m_watcher->directories());
}

void GitRepository::watchGitDir()
{
    clearWatches();
    // Watching the directory catches the index being replaced by rename.
    const QStringList paths{m_gitDir, m_gitDir + QStringLiteral("/refs/heads"), m_gitDir + QStringLiteral("/HEAD"),
                            m_gitDir + QStringLiteral("/index"), m_gitDir + QStringLiteral("/packed-refs")};
    for (const QString &p : paths)
        if (QFileInfo::exists(p))
            m_watcher->addPath(p);
}

void GitRepository::onGitPathChanged(const QString &path)
{
    Q_UNUSED(path);
    // Files replaced via rename lose their watch; re-arm them.
    const QStringList files{m_gitDir + QStringLiteral("/HEAD"), m_gitDir + QStringLiteral("/index"),
                            m_gitDir + QStringLiteral("/packed-refs")};
    for (const QString &f : files)
        if (QFileInfo::exists(f) && !m_watcher->files().contains(f))
            m_watcher->addPath(f);
    scheduleRefresh();
}

// --- Status ---------------------------------------------------------------------

void GitRepository::scheduleRefresh()
{
    if (m_workDir.isEmpty() || m_gitExe.isEmpty())
        return;
    m_timer->start();
}

void GitRepository::refresh()
{
    if (m_workDir.isEmpty() || m_gitExe.isEmpty())
        return;
    if (m_refreshing) {
        m_refreshAgain = true;
        return;
    }
    if (!m_isRepo) {
        discover(); // picks up a repository created outside of QODE (e.g. `git init` in the terminal)
        return;
    }
    m_refreshing = true;
    const int gen = m_generation;
    run({QStringLiteral("--no-optional-locks"), QStringLiteral("status"), QStringLiteral("--porcelain=v2"), QStringLiteral("-z"),
         QStringLiteral("--branch"), QStringLiteral("--untracked-files=all")},
        this, [this, gen](const Result &r) {
            if (gen != m_generation)
                return;
            if (!r.ok()) {
                // The repository vanished (or is unusable); start over from detection.
                m_refreshing = false;
                markNotRepo();
                return;
            }
            parseStatus(r.out);
            loadIgnored();
        });
}

void GitRepository::parseStatus(const QByteArray &out)
{
    m_branch.clear();
    m_upstream.clear();
    m_headOid.clear();
    m_detached = false;
    m_hasCommits = true;
    m_ahead = m_behind = 0;

    QList<GitFileChange> changes;
    const QList<QByteArray> recs = out.split('\0');
    for (int i = 0; i < recs.size(); ++i) {
        const QByteArray &r = recs.at(i);
        if (r.size() < 2)
            continue;
        if (r.startsWith("# ")) {
            const QString line = QString::fromUtf8(r.mid(2));
            if (line.startsWith(QLatin1String("branch.oid "))) {
                m_headOid = line.mid(11);
                m_hasCommits = m_headOid != QLatin1String("(initial)");
                if (!m_hasCommits)
                    m_headOid.clear();
            } else if (line.startsWith(QLatin1String("branch.head "))) {
                m_branch = line.mid(12);
                m_detached = m_branch == QLatin1String("(detached)");
            } else if (line.startsWith(QLatin1String("branch.upstream "))) {
                m_upstream = line.mid(16);
            } else if (line.startsWith(QLatin1String("branch.ab "))) {
                const QStringList ab = line.mid(10).split(QLatin1Char(' '));
                if (ab.size() == 2) {
                    m_ahead = ab.at(0).mid(1).toInt();
                    m_behind = ab.at(1).mid(1).toInt();
                }
            }
            continue;
        }
        GitFileChange c;
        const char type = r.at(0);
        if (type == '1' || type == '2' || type == 'u') {
            c.staged = QLatin1Char(r.at(2));
            c.unstaged = QLatin1Char(r.at(3));
            if (type == '1') {
                c.relPath = fieldsTail(r, 8);
            } else if (type == '2') {
                c.relPath = fieldsTail(r, 9);
                if (i + 1 < recs.size())
                    c.origRelPath = QString::fromUtf8(recs.at(++i));
            } else {
                c.relPath = fieldsTail(r, 10);
                c.conflicted = true;
            }
        } else if (type == '?') {
            c.relPath = QString::fromUtf8(r.mid(2));
            c.untracked = true;
            c.staged = c.unstaged = QLatin1Char('?');
        } else {
            continue;
        }
        if (c.relPath.isEmpty())
            continue;
        c.path = m_root + QLatin1Char('/') + c.relPath;
        changes.append(c);
    }
    if (m_detached && !m_headOid.isEmpty())
        m_branch = m_headOid.left(7);

    m_changes = changes;
    m_changeIndex.clear();
    m_folderKinds.clear();
    for (int i = 0; i < m_changes.size(); ++i) {
        const GitFileChange &c = m_changes.at(i);
        m_changeIndex.insert(c.path, i);
        const GitKind k = c.kind();
        QString d = c.path;
        while (true) {
            const int s = d.lastIndexOf(QLatin1Char('/'));
            if (s <= 0)
                break;
            d.truncate(s);
            auto it = m_folderKinds.find(d);
            if (it == m_folderKinds.end())
                m_folderKinds.insert(d, k);
            else if (k > it.value())
                it.value() = k;
            if (d.size() <= m_root.size())
                break;
        }
    }
}

void GitRepository::loadIgnored()
{
    const int gen = m_generation;
    run({QStringLiteral("ls-files"), QStringLiteral("--others"), QStringLiteral("--ignored"), QStringLiteral("--exclude-standard"),
         QStringLiteral("--directory"), QStringLiteral("-z")},
        this, [this, gen](const Result &r) {
            if (gen != m_generation)
                return;
            m_ignored.clear();
            if (r.ok()) {
                for (const QByteArray &e : r.out.split('\0')) {
                    if (e.isEmpty())
                        continue;
                    QString rel = QString::fromUtf8(e);
                    while (rel.endsWith(QLatin1Char('/')))
                        rel.chop(1);
                    m_ignored.insert(m_root + QLatin1Char('/') + rel);
                }
            }
            loadBranches();
        });
}

void GitRepository::loadBranches()
{
    const int gen = m_generation;
    run({QStringLiteral("for-each-ref"), QStringLiteral("--format=%(HEAD)\t%(refname)\t%(upstream:short)"),
         QStringLiteral("refs/heads"), QStringLiteral("refs/remotes")},
        this, [this, gen](const Result &r) {
            if (gen != m_generation)
                return;
            m_branches.clear();
            for (const QString &line : QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
                const QStringList f = line.split(QLatin1Char('\t'));
                if (f.size() < 2)
                    continue;
                GitBranchInfo b;
                b.current = f.at(0) == QLatin1String("*");
                QString ref = f.at(1);
                if (ref.startsWith(QLatin1String("refs/heads/"))) {
                    b.name = ref.mid(11);
                } else if (ref.startsWith(QLatin1String("refs/remotes/"))) {
                    b.name = ref.mid(13);
                    b.remote = true;
                    if (b.name.endsWith(QLatin1String("/HEAD")))
                        continue;
                } else {
                    continue;
                }
                b.upstream = f.value(2);
                m_branches.append(b);
            }
            finishRefresh();
        });
}

void GitRepository::finishRefresh()
{
    m_refreshing = false;
    emit statusChanged();
    if (m_refreshAgain) {
        m_refreshAgain = false;
        refresh();
    }
}

// --- Cached-state queries ------------------------------------------------------

QList<GitFileChange> GitRepository::stagedChanges() const
{
    QList<GitFileChange> out;
    for (const GitFileChange &c : m_changes)
        if (c.isStaged())
            out << c;
    return out;
}

QList<GitFileChange> GitRepository::unstagedChanges() const
{
    QList<GitFileChange> out;
    for (const GitFileChange &c : m_changes)
        if (c.isUnstaged() || c.untracked)
            out << c;
    return out;
}

QList<GitFileChange> GitRepository::conflicts() const
{
    QList<GitFileChange> out;
    for (const GitFileChange &c : m_changes)
        if (c.conflicted)
            out << c;
    return out;
}

const GitFileChange *GitRepository::changeFor(const QString &absPath) const
{
    const int i = m_changeIndex.value(absPath, -1);
    return i < 0 ? nullptr : &m_changes.at(i);
}

bool GitRepository::isIgnored(const QString &absPath) const
{
    if (m_ignored.isEmpty() || m_root.isEmpty())
        return false;
    QString p = absPath;
    while (p.size() > m_root.size()) {
        if (m_ignored.contains(p))
            return true;
        const int s = p.lastIndexOf(QLatin1Char('/'));
        if (s <= 0)
            break;
        p.truncate(s);
    }
    return false;
}

GitPathState GitRepository::stateOf(const QString &absPath) const
{
    GitPathState s;
    if (!m_isRepo)
        return s;
    if (const GitFileChange *c = changeFor(absPath)) {
        s.kind = c->kind();
        s.staged = c->staged;
        s.unstaged = c->unstaged;
        s.untracked = c->untracked;
        return s;
    }
    const auto it = m_folderKinds.constFind(absPath);
    if (it != m_folderKinds.constEnd()) {
        s.kind = it.value();
        s.isFolder = true;
        return s;
    }
    if (isIgnored(absPath))
        s.kind = GitKind::Ignored;
    return s;
}

QString GitRepository::relativePath(const QString &absPath) const
{
    if (m_root.isEmpty() || absPath.isEmpty())
        return {};
    QString p = QFileInfo(absPath).canonicalFilePath();
    if (p.isEmpty()) // e.g. deleted file: canonicalise the directory instead
        p = QFileInfo(QFileInfo(absPath).absolutePath()).canonicalFilePath() + QLatin1Char('/') + QFileInfo(absPath).fileName();
    if (!p.startsWith(m_root + QLatin1Char('/')))
        return {};
    return p.mid(m_root.size() + 1);
}

QString GitRepository::absolutePath(const QString &relPath) const
{
    return m_root + QLatin1Char('/') + relPath;
}

QStringList GitRepository::relPaths(const QStringList &absPaths, bool includeRenameSources) const
{
    QStringList out;
    for (const QString &a : absPaths) {
        const QString rel = relativePath(a);
        if (rel.isEmpty())
            continue;
        out << rel;
        if (includeRenameSources)
            if (const GitFileChange *c = changeFor(a))
                if (!c->origRelPath.isEmpty())
                    out << c->origRelPath;
    }
    out.removeDuplicates();
    return out;
}

// --- Operations -------------------------------------------------------------------

void GitRepository::init()
{
    if (m_workDir.isEmpty() || m_gitExe.isEmpty())
        return;
    setBusy(+1);
    run({QStringLiteral("init")}, this, [this](const Result &r) {
        setBusy(-1);
        if (!r.ok()) {
            emit errorOccurred(tr("Initialize Repository"), messageOf(r));
            return;
        }
        emit operationFinished(tr("Initialize Repository"), QString::fromUtf8(r.out).trimmed());
        m_root.clear();
        discover();
    });
}

void GitRepository::stage(const QStringList &absPaths)
{
    const QStringList rels = relPaths(absPaths, false);
    if (rels.isEmpty())
        return;
    runOp(tr("Stage"), {QStringList{QStringLiteral("--literal-pathspecs"), QStringLiteral("add"), QStringLiteral("--")} + rels});
}

void GitRepository::unstage(const QStringList &absPaths)
{
    const QStringList rels = relPaths(absPaths, true);
    if (rels.isEmpty())
        return;
    if (m_hasCommits)
        runOp(tr("Unstage"), {QStringList{QStringLiteral("--literal-pathspecs"), QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral("--")} + rels});
    else
        runOp(tr("Unstage"), {QStringList{QStringLiteral("--literal-pathspecs"), QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-r"), QStringLiteral("-q"), QStringLiteral("--")} + rels});
}

void GitRepository::stageAll(QObject *ctx, std::function<void(bool)> done)
{
    QPointer<QObject> guard(ctx);
    runOp(tr("Stage All"), {{QStringLiteral("add"), QStringLiteral("-A")}}, {}, 0, [guard, done](bool ok) {
        if (guard && done)
            done(ok);
    });
}

void GitRepository::unstageAll()
{
    if (m_hasCommits)
        runOp(tr("Unstage All"), {{QStringLiteral("restore"), QStringLiteral("--staged"), QStringLiteral(".")}});
    else
        runOp(tr("Unstage All"), {{QStringLiteral("rm"), QStringLiteral("--cached"), QStringLiteral("-r"), QStringLiteral("-q"), QStringLiteral(".")}});
}

void GitRepository::discard(const QStringList &absPaths)
{
    QStringList tracked, untracked;
    for (const QString &a : absPaths) {
        const GitFileChange *c = changeFor(a);
        if (!c || c->conflicted)
            continue;
        if (c->untracked)
            untracked << c->relPath;
        else if (c->isUnstaged())
            tracked << c->relPath;
    }
    QList<QStringList> steps;
    if (!tracked.isEmpty())
        steps.append(QStringList{QStringLiteral("--literal-pathspecs"), QStringLiteral("restore"), QStringLiteral("--")} + tracked);
    if (!untracked.isEmpty())
        steps.append(QStringList{QStringLiteral("--literal-pathspecs"), QStringLiteral("clean"), QStringLiteral("-f"), QStringLiteral("-q"), QStringLiteral("--")} + untracked);
    if (!steps.isEmpty())
        runOp(tr("Discard Changes"), steps);
}

void GitRepository::commit(const QString &message, bool amend, QObject *ctx, std::function<void(bool)> done)
{
    QStringList args{QStringLiteral("commit")};
    QByteArray input;
    if (amend)
        args << QStringLiteral("--amend");
    if (amend && message.trimmed().isEmpty()) {
        args << QStringLiteral("--no-edit");
    } else {
        args << QStringLiteral("-F") << QStringLiteral("-");
        input = message.toUtf8();
    }
    QPointer<QObject> guard(ctx);
    runOp(tr("Commit"), {args}, input, 0, [guard, done](bool ok) {
        if (guard && done)
            done(ok);
    });
}

void GitRepository::checkout(const QString &branch)
{
    // "origin/topic" => create a local tracking branch.
    const bool remote = std::any_of(m_branches.cbegin(), m_branches.cend(), [&](const GitBranchInfo &b) { return b.remote && b.name == branch; });
    if (remote)
        runOp(tr("Checkout"), {{QStringLiteral("checkout"), QStringLiteral("--track"), branch}});
    else
        runOp(tr("Checkout"), {{QStringLiteral("checkout"), branch}});
}

void GitRepository::createBranch(const QString &name)
{
    runOp(tr("Create Branch"), {{QStringLiteral("checkout"), QStringLiteral("-b"), name}});
}

void GitRepository::deleteBranch(const QString &name, bool force)
{
    runOp(tr("Delete Branch"), {{QStringLiteral("branch"), force ? QStringLiteral("-D") : QStringLiteral("-d"), name}});
}

void GitRepository::fetch()
{
    runOp(tr("Fetch"), {{QStringLiteral("fetch"), QStringLiteral("--all"), QStringLiteral("--prune")}}, {}, 120000);
}

void GitRepository::pull()
{
    runOp(tr("Pull"), {{QStringLiteral("pull"), QStringLiteral("--no-edit")}}, {}, 120000);
}

void GitRepository::push()
{
    if (!m_isRepo)
        return;
    if (!m_upstream.isEmpty()) {
        runOp(tr("Push"), {{QStringLiteral("push")}}, {}, 120000);
        return;
    }
    // No upstream yet: publish the branch to a remote.
    run({QStringLiteral("remote")}, this, [this](const Result &r) {
        const QStringList remotes = QString::fromUtf8(r.out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (remotes.isEmpty()) {
            emit errorOccurred(tr("Push"), tr("This repository has no remote. Add one first, e.g.\n\ngit remote add origin <url>"));
            return;
        }
        const QString remote = remotes.contains(QLatin1String("origin")) ? QStringLiteral("origin") : remotes.first();
        runOp(tr("Push"), {{QStringLiteral("push"), QStringLiteral("-u"), remote, m_branch}}, {}, 120000);
    });
}

void GitRepository::stash(bool includeUntracked)
{
    QStringList args{QStringLiteral("stash"), QStringLiteral("push")};
    if (includeUntracked)
        args << QStringLiteral("-u");
    runOp(tr("Stash"), {args});
}

void GitRepository::stashPop()
{
    runOp(tr("Pop Stash"), {{QStringLiteral("stash"), QStringLiteral("pop")}});
}

void GitRepository::addToGitignore(const QString &absPath)
{
    QString rel = relativePath(absPath);
    if (rel.isEmpty())
        return;
    if (QFileInfo(absPath).isDir())
        rel += QLatin1Char('/');
    const QString file = m_root + QStringLiteral("/.gitignore");
    QFile f(file);
    QByteArray existing;
    if (f.open(QIODevice::ReadOnly)) {
        existing = f.readAll();
        f.close();
    }
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        emit errorOccurred(tr("Add to .gitignore"), tr("Unable to write %1:\n%2").arg(file, f.errorString()));
        return;
    }
    if (!existing.isEmpty() && !existing.endsWith('\n'))
        f.write("\n");
    f.write(('/' + rel + '\n').toUtf8());
    f.close();
    emit operationFinished(tr("Add to .gitignore"), tr("Added /%1 to .gitignore").arg(rel));
    refresh();
}

// --- Queries --------------------------------------------------------------------------

void GitRepository::readBlob(const QString &spec, QObject *ctx, Callback cb)
{
    if (!m_isRepo) {
        Result r;
        cb(r);
        return;
    }
    run({QStringLiteral("cat-file"), QStringLiteral("blob"), spec}, ctx, std::move(cb));
}

void GitRepository::blame(const QString &relPath, const QByteArray &contents, QObject *ctx,
                          std::function<void(const QVector<GitBlameLine> &)> cb)
{
    if (!m_isRepo) {
        cb({});
        return;
    }
    run({QStringLiteral("blame"), QStringLiteral("--porcelain"), QStringLiteral("--contents"), QStringLiteral("-"), QStringLiteral("--"), relPath},
        ctx,
        [cb](const Result &r) {
            QVector<GitBlameLine> lines;
            if (!r.ok()) {
                cb(lines);
                return;
            }
            QHash<QString, GitBlameLine> commits;
            GitBlameLine cur;
            static const QRegularExpression header(QStringLiteral("^([0-9a-f]{40,64}) \\d+ \\d+"));
            for (const QByteArray &raw : r.out.split('\n')) {
                if (raw.startsWith('\t')) { // the source line closes the entry
                    lines.append(commits.value(cur.hash, cur));
                    continue;
                }
                const QString line = QString::fromUtf8(raw);
                if (const auto m = header.match(line); m.hasMatch()) {
                    cur = commits.value(m.captured(1));
                    cur.hash = m.captured(1);
                    continue;
                }
                bool changed = true;
                if (line.startsWith(QLatin1String("author ")))
                    cur.author = line.mid(7);
                else if (line.startsWith(QLatin1String("author-time ")))
                    cur.time = line.mid(12).toLongLong();
                else if (line.startsWith(QLatin1String("summary ")))
                    cur.summary = line.mid(8);
                else
                    changed = false;
                if (changed)
                    commits.insert(cur.hash, cur);
            }
            cb(lines);
        },
        contents);
}

void GitRepository::log(int limit, QObject *ctx, std::function<void(const QList<GitCommitInfo> &)> cb)
{
    run({QStringLiteral("log"), QStringLiteral("-n"), QString::number(limit),
         QStringLiteral("--pretty=format:%H%x1f%h%x1f%an%x1f%ar%x1f%D%x1f%s%x1e")},
        ctx, [cb](const Result &r) {
            QList<GitCommitInfo> out;
            if (r.ok()) {
                for (const QString &rec : QString::fromUtf8(r.out).split(QChar(0x1e), Qt::SkipEmptyParts)) {
                    const QStringList f = rec.trimmed().split(QChar(0x1f));
                    if (f.size() < 6)
                        continue;
                    out.append({f.at(0), f.at(1), f.at(2), f.at(3), f.at(4), f.at(5)});
                }
            }
            cb(out);
        });
}

void GitRepository::commitPatch(const QString &hash, QObject *ctx, std::function<void(const QString &)> cb)
{
    run({QStringLiteral("show"), QStringLiteral("--stat"), QStringLiteral("--patch"), QStringLiteral("--format=fuller"),
         QStringLiteral("--no-color"), hash},
        ctx, [cb](const Result &r) { cb(r.ok() ? QString::fromUtf8(r.out) : messageOf(r)); });
}

void GitRepository::lastCommitMessage(QObject *ctx, std::function<void(const QString &)> cb)
{
    run({QStringLiteral("log"), QStringLiteral("-1"), QStringLiteral("--format=%B")}, ctx,
        [cb](const Result &r) { cb(r.ok() ? QString::fromUtf8(r.out).trimmed() : QString()); });
}
