// Java / Kotlin editing extras: Copy Reference, New Kotlin / Java class, Kotlin scratch files, and the completions that
// need no language server (gradle.properties & co., @file: annotations with Maven coordinates).
#include "MainWindow.h"
#include "platform/Platform.h"
#include "platform/AppPaths.h"

#include "dialogs/NewJvmClassDialog.h"
#include "editor/CodeEditor.h"
#include "editor/Document.h"
#include "editor/EditorManager.h"
#include "explorer/FileIcons.h"
#include "explorer/ProjectExplorer.h"
#include "filesystem/FileManager.h"
#include "jvm/GradleProperties.h"
#include "jvm/JvmNewFile.h"
#include "jvm/JvmReference.h"
#include "jvm/JvmTools.h"
#include "jvm/KotlinAnnotations.h"
#include "jvm/MavenCentral.h"
#include "project/ProjectManager.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextBlock>
#include <QTimer>

void MainWindow::createJvmEditActions()
{
    m_copyRefAct = new QAction(tr("Copy Reference"), this);
    m_copyRefAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_C));
    m_copyRefAct->setToolTip(tr("Copy the qualified name of the symbol at the caret (com.example.Foo#bar), or path:line"));
    Icons::bind(m_copyRefAct, QStringLiteral(":/new-icons/copy.svg"));
    connect(m_copyRefAct, &QAction::triggered, this, &MainWindow::copyReference);
    addAction(m_copyRefAct);

    m_scratchAct = new QAction(tr("New Kotlin Scratch"), this);
    m_scratchAct->setShortcut(QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Insert));
    m_scratchAct->setToolTip(tr("A Kotlin script outside the project to try things in; F5 runs it"));
    connect(m_scratchAct, &QAction::triggered, this, [this] { newScratchFile(QStringLiteral("kts")); });
    addAction(m_scratchAct);

    // File menu: the folder of the current file (or the project's main source root) decides the package.
    auto folderForNew = [this] {
        Document *doc = m_editors->currentDocument();
        if (doc && !doc->filePath().isEmpty() && JvmNewFile::isSourceFolder(QFileInfo(doc->filePath()).absolutePath()))
            return QFileInfo(doc->filePath()).absolutePath();
        if (!m_projects->hasProject())
            return QString();
        const QString root = m_projects->project().root;
        for (const char *sub : {"src/main/kotlin", "src/main/java"})
            if (QFileInfo(root + QLatin1Char('/') + QLatin1String(sub)).isDir())
                return root + QLatin1Char('/') + QLatin1String(sub);
        return QString();
    };
    m_newKotlinAct = new QAction(FileIcons::forFile(QStringLiteral("x.kt")), tr("New Kotlin Class/File…"), this);
    m_newKotlinAct->setToolTip(tr("A Kotlin class, interface, data class, object … in the current file's package"));
    connect(m_newKotlinAct, &QAction::triggered, this, [this, folderForNew] {
        const QString dir = folderForNew();
        if (dir.isEmpty()) {
            QMessageBox::information(this, tr("New Kotlin Class/File"), tr("Open a file below src/main/kotlin (or src/main/java), or use the explorer's context menu on a package folder."));
            return;
        }
        newJvmClass(dir, true);
    });
    m_newJavaAct = new QAction(FileIcons::forFile(QStringLiteral("x.java")), tr("New Java Class…"), this);
    m_newJavaAct->setToolTip(tr("A Java class, interface, record, enum … in the current file's package"));
    connect(m_newJavaAct, &QAction::triggered, this, [this, folderForNew] {
        const QString dir = folderForNew();
        if (dir.isEmpty()) {
            QMessageBox::information(this, tr("New Java Class"), tr("Open a file below src/main/java, or use the explorer's context menu on a package folder."));
            return;
        }
        newJvmClass(dir, false);
    });

    m_scratchMenu = new QMenu(tr("Scratch Files"), this);
    connect(m_scratchMenu, &QMenu::aboutToShow, this, &MainWindow::fillScratchMenu);
}

// --- Copy Reference -------------------------------------------------------------------------------------------------

void MainWindow::copyReference()
{
    Document *doc = m_editors->currentDocument();
    CodeEditor *ed = m_editors->currentEditor();
    if (!doc || !ed || doc->filePath().isEmpty())
        return;
    const QTextCursor c = ed->textCursor();
    QString ref = JvmReference::at(doc->filePath(), ed->toPlainText(), c.blockNumber(), c.positionInBlock());
    if (ref.isEmpty()) {
        // Anything else (or nothing to name): the file relative to the project, and the line.
        QString path = doc->filePath();
        if (m_projects->hasProject() && Platform::isUnder(path, m_projects->project().root))
            path = path.mid(m_projects->project().root.size() + 1);
        ref = QStringLiteral("%1:%2").arg(path).arg(c.blockNumber() + 1);
    }
    QApplication::clipboard()->setText(ref);
    statusBar()->showMessage(tr("Copied %1").arg(ref), 4000);
}

// --- New Kotlin / Java class ------------------------------------------------------------------------------------------

void MainWindow::newJvmClass(const QString &dir, bool kotlin)
{
    NewJvmClassDialog dlg(kotlin, dir, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const JvmNewFile::Result r = dlg.result();
    if (!QDir().mkpath(QFileInfo(r.path).absolutePath())) {
        QMessageBox::warning(this, dlg.windowTitle(), tr("Could not create the folder %1").arg(FileManager::displayPath(QFileInfo(r.path).absolutePath())));
        return;
    }
    QString error;
    if (!FileManager::writeFile(r.path, r.content.toUtf8(), &error)) {
        QMessageBox::warning(this, dlg.windowTitle(), tr("Could not create %1: %2").arg(FileManager::displayPath(r.path), error));
        return;
    }
    m_editorHiddenByUser = false;
    if (m_editors->openFileAt(r.path, r.caretLine + 1, r.caretColumn + 1)) {
        setEditorColumnVisible(true);
        m_editors->focusEditor();
    }
}

// --- Scratch files ----------------------------------------------------------------------------------------------------

QString MainWindow::scratchDir() const
{
    return AppPaths::data(QStringLiteral("scratches"));
}

void MainWindow::newScratchFile(const QString &kind)
{
    const QString dir = scratchDir();
    if (!QDir().mkpath(dir)) {
        QMessageBox::warning(this, tr("New Scratch File"), tr("Could not create %1").arg(FileManager::displayPath(dir)));
        return;
    }
    QString path, text;
    int caretLine = 0;
    const bool java = kind == QLatin1String("java");
    for (int n = 1;; ++n) {
        const QString suffix = n == 1 ? QString() : QString::number(n);
        path = java ? QStringLiteral("%1/Scratch%2.java").arg(dir, suffix)
                    : QStringLiteral("%1/scratch%2%3.%4").arg(dir, suffix.isEmpty() ? QString() : QStringLiteral("_") + suffix, QString(), kind);
        if (!QFileInfo::exists(path))
            break;
    }
    if (java) {
        const QString cls = QFileInfo(path).completeBaseName();
        text = QStringLiteral("// Java scratch: ▶ beside main runs it. Kept in QODE's data folder, not in the project.\n\n"
                              "public class %1 {\n    public static void main(String[] args) {\n        \n    }\n}\n").arg(cls);
        caretLine = 4;
    } else if (kind == QLatin1String("main.kts")) {
        text = QStringLiteral("// Kotlin scratch with dependencies: F5 runs it. Kept in QODE's data folder.\n"
                              "@file:DependsOn(\"org.jetbrains.kotlinx:kotlinx-coroutines-core:1.10.2\")\n\n"
                              "import kotlinx.coroutines.*\n\nrunBlocking {\n    \n}\n");
        caretLine = 6;
    } else {
        text = QStringLiteral("// Kotlin scratch: F5 runs it (kotlinc -script). Kept in QODE's data folder, not in the project.\n\n\n");
        caretLine = 2;
    }
    QString error;
    if (!FileManager::writeFile(path, text.toUtf8(), &error)) {
        QMessageBox::warning(this, tr("New Scratch File"), tr("Could not create %1: %2").arg(FileManager::displayPath(path), error));
        return;
    }
    m_editorHiddenByUser = false;
    if (m_editors->openFileAt(path, caretLine + 1, java ? 9 : kind == QLatin1String("main.kts") ? 5 : 1)) {
        setEditorColumnVisible(true);
        m_editors->focusEditor();
    }
}

void MainWindow::fillScratchMenu()
{
    m_scratchMenu->clear();
    m_scratchAct->setIcon(FileIcons::forFile(QStringLiteral("x.kts")));
    m_scratchMenu->addAction(m_scratchAct);
    m_scratchMenu->addAction(tr("New Kotlin Scratch with Dependencies (.main.kts)"), this, [this] { newScratchFile(QStringLiteral("main.kts")); })
        ->setToolTip(tr("@file:DependsOn pulls libraries from Maven Central"));
    m_scratchMenu->addAction(tr("New Java Scratch"), this, [this] { newScratchFile(QStringLiteral("java")); });
    const QDir dir(scratchDir());
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.kts"), QStringLiteral("*.java")}, QDir::Files, QDir::Time);
    m_scratchMenu->addSeparator();
    if (files.isEmpty()) {
        m_scratchMenu->addAction(tr("No scratch files yet"))->setEnabled(false);
    } else {
        for (int i = 0; i < files.size() && i < 20; ++i) {
            const QFileInfo fi = files.at(i);
            const QString when = QLocale().toString(fi.lastModified(), QLocale::ShortFormat);
            m_scratchMenu->addAction(QStringLiteral("%1  —  %2").arg(fi.fileName(), when), this, [this, path = fi.filePath()] {
                m_editorHiddenByUser = false;
                if (m_editors->openFile(path)) {
                    setEditorColumnVisible(true);
                    m_editors->focusEditor();
                }
            });
        }
    }
    m_scratchMenu->addSeparator();
    m_scratchMenu->addAction(tr("Open Scratches Folder"), this, [this] {
        QDir().mkpath(scratchDir());
        FileManager::revealInFileManager(scratchDir());
    });
}

// --- Completion without a server ------------------------------------------------------------------------------------

void MainWindow::wireJvmEdit(Document *doc, CodeEditor *ed)
{
    const QString path = doc->filePath();
    if (path.isEmpty())
        return;
    QPointer<CodeEditor> guard(ed);

    // gradle.properties / local.properties / gradle-wrapper.properties: keys and values.
    if (const GradleProperties::File kind = GradleProperties::kindOf(path); kind != GradleProperties::File::None) {
        ed->setLocalCompletions([this, kind, path](const QString &beforeWord) {
            QString root = m_projects->hasProject() ? m_projects->project().root : QString();
            if (root.isEmpty() || !Platform::isUnder(path, root))
                root = QFileInfo(path).absolutePath();
            if (kind == GradleProperties::File::Wrapper) // gradle/wrapper/gradle-wrapper.properties: the build folder
                root = QDir::cleanPath(QFileInfo(path).absolutePath() + QStringLiteral("/../.."));
            QVector<LspCompletionItem> items;
            for (const GradleProperties::Entry &e : GradleProperties::complete(kind, beforeWord, root)) {
                LspCompletionItem it;
                it.label = e.label;
                it.detail = e.detail;
                it.kind = e.value ? 12 : e.group ? 9 : 10;
                items << it;
            }
            return items;
        });
        return;
    }

    // Kotlin: @file: targets, annotations in files no server reads, @file:DependsOn coordinates.
    const bool kotlin = (path.endsWith(QLatin1String(".kt")) || path.endsWith(QLatin1String(".kts"))) && !path.endsWith(QLatin1String(".gradle.kts"));
    if (!kotlin)
        return;
    ed->setLocalCompletions([this, guard, doc = QPointer<Document>(doc), path](const QString &beforeWord) {
        QVector<LspCompletionItem> items;
        if (!guard || !doc)
            return items;
        const KotlinAnnotations::Where where = KotlinAnnotations::contextOf(beforeWord);
        if (where.context == KotlinAnnotations::Context::None)
            return items;
        const QTextCursor c = guard->textCursor();
        const int line = c.blockNumber(), column = c.positionInBlock();
        // The editor matches the word being typed against the start of an item: an argument item ("com.google.code.gson:
        // gson:2.11.0" for "gson") is matched from where that word occurs in it.
        const QString blockText = c.block().text();
        int wordStart = column;
        while (wordStart > 0 && (blockText.at(wordStart - 1).isLetterOrNumber() || blockText.at(wordStart - 1) == QLatin1Char('_')))
            --wordStart;
        const QString word = blockText.mid(wordStart, column - wordStart);
        auto argEdit = [&](LspCompletionItem &it, const QString &text) {
            it.hasEdit = true;
            it.edit.startLine = it.edit.endLine = line;
            it.edit.startColumn = where.argStart;
            it.edit.endColumn = column;
            it.edit.text = text;
            int at = word.isEmpty() ? 0 : int(it.label.indexOf(word, 0, Qt::CaseInsensitive));
            static const QRegularExpression boundary(QStringLiteral("[.:/_-]"));
            if (!word.isEmpty()) // prefer an occurrence at a segment start (":gson" over "com.google.code.gson")
                for (int i = int(it.label.indexOf(word, 0, Qt::CaseInsensitive)); i > 0; i = int(it.label.indexOf(word, i + 1, Qt::CaseInsensitive)))
                    if (boundary.match(it.label.mid(i - 1, 1)).hasMatch()) {
                        at = i;
                        break;
                    }
            it.filterText = it.label.mid(qMax(0, at));
        };
        QSet<QString> seen;
        for (const KotlinAnnotations::Entry &e : KotlinAnnotations::complete(path, where, m_lsp->isServed(doc))) {
            LspCompletionItem it;
            it.label = e.label;
            it.detail = e.detail;
            it.kind = e.kind;
            it.snippet = e.snippet;
            it.insertText = e.insert;
            if (e.replaceArg)
                argEdit(it, e.insert.isEmpty() ? e.label : e.insert);
            seen.insert(e.insert.isEmpty() ? e.label : e.insert);
            items << it;
        }
        if (where.context != KotlinAnnotations::Context::DependsOn)
            return items;
        // Maven Central: what was found for the argument typed so far; a search starts after a short pause.
        const QString query = c.block().text().mid(where.argStart, column - where.argStart).trimmed();
        const QStringList parts = query.split(QLatin1Char(':'));
        const bool versions = parts.size() >= 3;
        const QString key = versions ? parts.at(0) + QLatin1Char(':') + parts.at(1) + QLatin1Char(':') : query;
        if (key.size() >= 3 && !m_scriptSearchHits.contains(key))
            scriptDependencySearch(guard, key);
        for (const QString &hit : m_scriptSearchHits.value(key)) {
            const QString coordinate = hit.section(QLatin1Char('\t'), 0, 0);
            if (seen.contains(coordinate))
                continue;
            LspCompletionItem it;
            it.label = versions ? coordinate.section(QLatin1Char(':'), 2) : coordinate;
            it.detail = hit.section(QLatin1Char('\t'), 1);
            it.kind = versions ? 12 : 9;
            it.sortText = QStringLiteral("~"); // after the local repository's
            argEdit(it, coordinate);
            items << it;
        }
        if (versions) { // one list, newest first, whether downloaded or not
            std::stable_sort(items.begin(), items.end(), [](const LspCompletionItem &a, const LspCompletionItem &b) {
                return MavenCentral::compareVersions(a.label, b.label) > 0;
            });
            for (int i = 0; i < items.size(); ++i)
                items[i].sortText = QStringLiteral("%1").arg(i, 4, 10, QLatin1Char('0'));
        }
        return items;
    });
}

// One Maven Central lookup at a time, 350 ms after typing stops: artifacts for a search text, versions for "g:a:".
// The answer is cached; if the caret is still in that argument the completion list is asked for again.
void MainWindow::scriptDependencySearch(CodeEditor *editor, const QString &query)
{
    if (m_scriptSearchPending.contains(query))
        return;
    if (!m_scriptCentral)
        m_scriptCentral = new MavenCentral(this);
    if (!m_scriptSearchTimer) {
        m_scriptSearchTimer = new QTimer(this);
        m_scriptSearchTimer->setSingleShot(true);
        m_scriptSearchTimer->setInterval(350);
        connect(m_scriptSearchTimer, &QTimer::timeout, this, [this] {
            const QString query = m_scriptSearchQuery;
            QPointer<CodeEditor> ed = m_scriptSearchEditor;
            if (query.isEmpty() || m_scriptSearchHits.contains(query))
                return;
            m_scriptSearchPending.insert(query);
            auto finish = [this, query, ed](const QStringList &hits) {
                m_scriptSearchPending.remove(query);
                m_scriptSearchHits.insert(query, hits);
                if (!ed || !ed->hasFocus())
                    return;
                const QTextCursor c = ed->textCursor();
                const QString before = c.block().text().left(c.positionInBlock());
                if (KotlinAnnotations::contextOf(before).context == KotlinAnnotations::Context::DependsOn)
                    ed->triggerCompletion();
            };
            const QStringList parts = query.split(QLatin1Char(':'));
            if (query.endsWith(QLatin1Char(':')) && parts.size() == 3) {
                m_scriptCentral->versions(parts.at(0), parts.at(1), [finish, parts](const QStringList &versions, const QString &) {
                    QStringList hits;
                    for (const QString &v : versions.mid(0, 40))
                        hits << QStringLiteral("%1:%2:%3\tMaven Central").arg(parts.at(0), parts.at(1), v);
                    finish(hits);
                });
            } else {
                m_scriptCentral->search(query, [finish](const QList<MavenCentral::Artifact> &found, const QString &) {
                    QStringList hits;
                    for (const MavenCentral::Artifact &a : found)
                        hits << QStringLiteral("%1:%2\tMaven Central").arg(a.coordinate(), a.latest);
                    finish(hits);
                });
            }
        });
    }
    m_scriptSearchQuery = query;
    m_scriptSearchEditor = editor;
    m_scriptSearchTimer->start();
}
