# QODE needs Qt 6 (Core, Gui, Widgets only)
lessThan(QT_MAJOR_VERSION, 6): error("QODE requires Qt 6 - build with qmake6.")

QT += core gui widgets network

# Video playback needs Qt Multimedia; without it the media panel still shows images.
qtHaveModule(multimedia) {
    QT += multimedia multimediawidgets
    DEFINES += QODE_HAS_MULTIMEDIA
}

# The live web preview embeds Chromium through Qt WebEngine; without it there is no web / Expo preview at all.
# CONFIG+=no_webengine leaves it out even when this Qt has it (scripts/build-deb-qt.sh --no-webengine, a smaller bundle).
!no_webengine:qtHaveModule(webenginewidgets) {
    QT += webenginewidgets
    DEFINES += QODE_HAS_WEBENGINE
}

# Desktop notifications (Claude Code finished or waiting) go over D-Bus; without it only the taskbar entry is flagged.
# Not on Windows: its Qt kit ships QtDBus, but there is no session bus to talk to.
!win32:qtHaveModule(dbus) {
    QT += dbus
    DEFINES += QODE_HAS_DBUS
}

CONFIG += c++17 warn_on
CONFIG -= app_bundle

TEMPLATE = app
TARGET = qode
VERSION = 2.0.0
# JarSource reads library source jars with zlib: the system's on Unix, the copy inside Qt Core on Windows.
unix: LIBS += -lz
win32: QT += zlib-private
DEFINES += QT_DEPRECATED_WARNINGS QT_NO_CAST_TO_ASCII QT_NO_CAST_FROM_BYTEARRAY
DEFINES += QODE_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += $$PWD/src

SOURCES += \
    src/refactor/MoveRefactor.cpp \
    src/refactor/MoveController.cpp \
    src/explorer/ExplorerTree.cpp \
    src/refactor/RefactorWeb.cpp \
    src/refactor/RefactorPython.cpp \
    src/refactor/RefactorJvm.cpp \
    src/refactor/RefactorNative.cpp \
    src/refactor/RefactorRust.cpp \
    src/expo/JsonDoc.cpp \
    src/expo/ExpoProject.cpp \
    src/expo/ExpoSchema.cpp \
    src/expo/ExpoBar.cpp \
    src/expo/ExpoRoutes.cpp \
    src/expo/ExpoWebServer.cpp \
    src/expo/ExpoConfigDialog.cpp \
    src/webdev/WebProject.cpp \
    src/webdev/DevServer.cpp \
    src/webdev/DevServerBar.cpp \
    src/webdev/ServerDialogs.cpp \
    src/webdev/EnvDialog.cpp \
    src/webdev/WebPreview.cpp \
    src/main.cpp \
    src/app/Application.cpp \
    src/app/DesktopNotify.cpp \
    src/app/InstanceRegistry.cpp \
    src/app/StartupProfile.cpp \
    src/settings/SettingsManager.cpp \
    src/settings/Theme.cpp \
    src/settings/ThemeManager.cpp \
    src/settings/Icons.cpp \
    src/cpp/CppTools.cpp \
    src/format/Formatter.cpp \
    src/search/ProjectSearch.cpp \
    src/search/SearchPanel.cpp \
    src/tasks/TasksPanel.cpp \
    src/palette/FuzzyMatcher.cpp \
    src/palette/PalettePopup.cpp \
    src/filesystem/FileManager.cpp \
    src/project/ProjectFiles.cpp \
    src/project/ProjectManager.cpp \
    src/project/QmakeProject.cpp \
    src/project/JvmProject.cpp \
    src/project/VersionCatalog.cpp \
    src/project/ProjectTemplates.cpp \
    src/project/GradleProject.cpp \
    src/project/ProjectModel.cpp \
    src/project/PythonEnv.cpp \
    src/media/MarkdownPreview.cpp \
    src/media/CodeImage.cpp \
    src/media/CodeImageDialog.cpp \
    src/media/CopyButton.cpp \
    src/media/MarkdownView.cpp \
    src/media/MarkdownHtml.cpp \
    src/agent/AgentJournal.cpp \
    src/agent/AgentPanel.cpp \
    src/agent/AgentPanelMcp.cpp \
    src/agent/AgentPanelUsage.cpp \
    src/mcp/McpClient.cpp \
    src/mcp/McpConfig.cpp \
    src/mcp/McpDialog.cpp \
    src/mcp/McpManager.cpp \
    src/agent/AgentRun.cpp \
    src/agent/AgentRunClaude.cpp \
    src/agent/AgentSession.cpp \
    src/agent/AgentTools.cpp \
    src/agent/AgentToolsServers.cpp \
    src/agent/AgentToolsPreview.cpp \
    src/agent/AgentToolsHttp.cpp \
    src/agent/AgentToolsDebug.cpp \
    src/agent/CodeChunks.cpp \
    src/agent/CodeIndex.cpp \
    src/agent/AgentTurnView.cpp \
    src/ai/AiHub.cpp \
    src/claudecode/ClaudeCodeChanges.cpp \
    src/claudecode/ClaudeCodeCli.cpp \
    src/claudecode/ClaudeCodeHistory.cpp \
    src/claudecode/ClaudeCodeInfo.cpp \
    src/claudecode/ClaudeCodePanel.cpp \
    src/claudecode/ClaudeCodePrompt.cpp \
    src/claudecode/ClaudeCodeSession.cpp \
    src/claudecode/ClaudeCodeTasks.cpp \
    src/claudecode/ContextMeter.cpp \
    src/ai/AiChat.cpp \
    src/ai/AiChangeCard.cpp \
    src/ai/AiEdits.cpp \
    src/ai/AiEnv.cpp \
    src/ai/AiInput.cpp \
    src/ai/AiQuote.cpp \
    src/ai/AiKeyDialog.cpp \
    src/ai/AiWorkspace.cpp \
    src/ai/ClaudeClient.cpp \
    src/ai/GeminiClient.cpp \
    src/gradle/GradlePanel.cpp \
    src/maven/MavenPanel.cpp \
    src/maven/MavenProject.cpp \
    src/spring/SpringProject.cpp \
    src/spring/SpringServer.cpp \
    src/spring/ActuatorView.cpp \
    src/spring/SpringProperties.cpp \
    src/spring/SpringScaffold.cpp \
    src/spring/SpringStarters.cpp \
    src/spring/AddStarterDialog.cpp \
    src/spring/SpringLogView.cpp \
    src/spring/SpringPanel.cpp \
    src/http/HttpEnv.cpp \
    src/http/HttpFile.cpp \
    src/http/HttpPanel.cpp \
    src/http/HttpResponseView.cpp \
    src/http/HttpRunner.cpp \
    src/http/HttpUi.cpp \
    src/http/HttpVars.cpp \
    src/spring/HttpClientView.cpp \
    src/spring/SpringEndpoints.cpp \
    src/spring/SpringBar.cpp \
    src/ktor/AddKtorPluginDialog.cpp \
    src/ktor/KtorBar.cpp \
    src/ktor/KtorConfigDialog.cpp \
    src/ktor/KtorConfigView.cpp \
    src/ktor/KtorLive.cpp \
    src/ktor/KtorPanel.cpp \
    src/ktor/KtorPlugins.cpp \
    src/ktor/KtorProject.cpp \
    src/ktor/KtorReloader.cpp \
    src/ktor/KtorRoutesView.cpp \
    src/ktor/KtorServer.cpp \
    src/ktor/KtorSource.cpp \
    src/project/BuildFileEdit.cpp \
    src/window/MainWindowCpp.cpp \
    src/window/MainWindowFix.cpp \
    src/window/MainWindowJvm.cpp \
    src/window/MainWindowJvmEdit.cpp \
    src/window/MainWindowMaven.cpp \
    src/window/MainWindowUi.cpp \
    src/window/MainWindowLsp.cpp \
    src/window/MainWindowGit.cpp \
    src/window/MainWindowPanels.cpp \
    src/window/MainWindowDebug.cpp \
    src/window/MainWindowInline.cpp \
    src/ai/InlineClient.cpp \
    src/ai/InlineEdit.cpp \
    src/window/MainWindowKtor.cpp \
    src/spring/SpringConfigDialog.cpp \
    src/gradle/GradleTasks.cpp \
    src/gradle/GradleEvents.cpp \
    src/gradle/GradleBuildView.cpp \
    src/jvm/JvmTools.cpp \
    src/jvm/GradleProperties.cpp \
    src/jvm/JvmNewFile.cpp \
    src/jvm/JvmReference.cpp \
    src/jvm/KotlinAnnotations.cpp \
    src/jvm/JdkScanner.cpp \
    src/jvm/JdkDialog.cpp \
    src/jvm/MavenCentral.cpp \
    src/jvm/DependencyEdit.cpp \
    src/jvm/DependencyDialog.cpp \
    src/jvm/ScriptCheck.cpp \
    src/debug/DebugWorker.cpp \
    src/debug/DebugSession.cpp \
    src/debug/DebugPanel.cpp \
    src/media/MediaPanel.cpp \
    src/explorer/ProjectExplorer.cpp \
    src/explorer/FileIcons.cpp \
    src/explorer/GitItemDelegate.cpp \
    src/git/GitDiff.cpp \
    src/git/GitRepository.cpp \
    src/git/GitPanel.cpp \
    src/git/BranchButton.cpp \
    src/git/BranchPopup.cpp \
    src/editor/ZoomPopup.cpp \
    src/git/DiffDialog.cpp \
    src/git/PatchDialog.cpp \
    src/dialogs/NewProjectDialog.cpp \
    src/dialogs/NewFileDialog.cpp \
    src/dialogs/NewJvmClassDialog.cpp \
    src/dialogs/UnsavedChangesDialog.cpp \
    src/dialogs/UnusedImportsDialog.cpp \
    src/dialogs/CallHierarchyDialog.cpp \
    src/dialogs/ReferencesDialog.cpp \
    src/dialogs/LspLogDialog.cpp \
    src/dialogs/RunConfigDialog.cpp \
    src/dialogs/ThemeEditorDialog.cpp \
    src/dialogs/CompilerFlagsDialog.cpp \
    src/dialogs/IncludePathsDialog.cpp \
    src/dialogs/LspInstallDialog.cpp \
    src/dialogs/JdtlsInstallDialog.cpp \
    src/dialogs/LspRemoveDialog.cpp \
    src/dialogs/NpmInstallDialog.cpp \
    src/dialogs/PipInstallDialog.cpp \
    src/dialogs/PackagesDialog.cpp \
    src/editor/Language.cpp \
    src/editor/SyntaxHighlighter.cpp \
    src/editor/Document.cpp \
    src/editor/TextEncoding.cpp \
    src/editor/Breadcrumbs.cpp \
    src/editor/CsvData.cpp \
    src/editor/CsvTable.cpp \
    src/editor/CodeEditor.cpp \
    src/editor/CompletionPopup.cpp \
    src/editor/HoverRender.cpp \
    src/editor/SignaturePopup.cpp \
    src/editor/CodeEditorAssist.cpp \
    src/editor/CodeEditorPredict.cpp \
    src/editor/CodeEditorFold.cpp \
    src/editor/CodeEditorTyping.cpp \
    src/editor/CodeEditorCompletion.cpp \
    src/editor/CodeEditorGutter.cpp \
    src/editor/CodeEditorMulti.cpp \
    src/editor/CodeEditorLinks.cpp \
    src/editor/CodeEditorLens.cpp \
    src/editor/EditorConfig.cpp \
    src/history/LocalHistory.cpp \
    src/history/LocalHistoryDialog.cpp \
    src/git/DiffPane.cpp \
    src/window/MainWindowEditing.cpp \
    src/window/MainWindowLens.cpp \
    src/editor/EditPredict.cpp \
    src/editor/Snippets.cpp \
    src/editor/Emmet.cpp \
    src/editor/HoverPopup.cpp \
    src/editor/MiniMap.cpp \
    src/editor/FindBar.cpp \
    src/editor/EditorGroup.cpp \
    src/editor/EditorManager.cpp \
    src/editor/WelcomePage.cpp \
    src/terminal/TerminalScreen.cpp \
    src/terminal/ShellProcess.cpp \
    src/terminal/ShellIntegration.cpp \
    src/terminal/ShellSyntax.cpp \
    src/terminal/TerminalView.cpp \
    src/terminal/ProblemScanner.cpp \
    src/terminal/Terminal.cpp \
    src/terminal/TerminalPanel.cpp \
    src/lsp/LspClient.cpp \
    src/lsp/JarSource.cpp \
    src/lsp/LspInstaller.cpp \
    src/lsp/JdtlsInstaller.cpp \
    src/lsp/LspManager.cpp \
    src/lsp/LspServers.cpp \
    src/lsp/LspSync.cpp \
    src/lsp/NpmInstaller.cpp \
    src/lsp/PipInstaller.cpp \
    src/python/PythonTools.cpp \
    src/python/PackageManager.cpp \
    src/window/Island.cpp \
    src/window/SideSections.cpp \
    src/window/MainWindow.cpp \
    src/window/MainWindowAssist.cpp \
    src/window/MainWindowIde.cpp \
    src/window/MainWindowIdeServers.cpp \
    src/window/MainWindowIdePreview.cpp \
    src/window/MainWindowIdeDebug.cpp \
    src/window/MainWindowHttp.cpp

HEADERS += \
    src/refactor/MoveRefactor.h \
    src/refactor/MoveController.h \
    src/explorer/ExplorerTree.h \
    src/refactor/RefactorCtx.h \
    src/expo/JsonDoc.h \
    src/expo/ExpoProject.h \
    src/expo/ExpoSchema.h \
    src/expo/ExpoBar.h \
    src/expo/ExpoRoutes.h \
    src/expo/ExpoWebServer.h \
    src/expo/ExpoConfigDialog.h \
    src/webdev/WebProject.h \
    src/webdev/DevServer.h \
    src/webdev/DevServerBar.h \
    src/webdev/ServerDialogs.h \
    src/webdev/EnvDialog.h \
    src/webdev/WebPreview.h \
    src/app/Application.h \
    src/platform/AppPaths.h \
    src/platform/Platform.h \
    src/platform/ProcessTree.h \
    src/app/DesktopNotify.h \
    src/app/InstanceRegistry.h \
    src/app/StartupProfile.h \
    src/settings/SettingsManager.h \
    src/settings/Theme.h \
    src/settings/ThemeManager.h \
    src/settings/Icons.h \
    src/cpp/CppTools.h \
    src/ai/AiModels.h \
    src/ai/AiPricing.h \
    src/ai/InlineClient.h \
    src/ai/InlineEdit.h \
    src/format/Formatter.h \
    src/search/ProjectSearch.h \
    src/search/SearchPanel.h \
    src/tasks/TasksPanel.h \
    src/palette/FuzzyMatcher.h \
    src/palette/PalettePopup.h \
    src/filesystem/FileManager.h \
    src/project/Project.h \
    src/project/ProjectFiles.h \
    src/project/ProjectManager.h \
    src/project/QmakeProject.h \
    src/project/JvmProject.h \
    src/project/VersionCatalog.h \
    src/project/ProjectTemplates.h \
    src/project/GradleProject.h \
    src/project/ProjectModel.h \
    src/project/PythonEnv.h \
    src/media/MarkdownPreview.h \
    src/media/CodeImage.h \
    src/media/CodeImageDialog.h \
    src/media/CopyButton.h \
    src/media/MarkdownView.h \
    src/media/MarkdownHtml.h \
    src/agent/AgentJournal.h \
    src/agent/AgentPanel.h \
    src/mcp/McpClient.h \
    src/mcp/McpConfig.h \
    src/mcp/McpDialog.h \
    src/mcp/McpManager.h \
    src/agent/AgentRun.h \
    src/agent/AgentSession.h \
    src/agent/AgentTools.h \
    src/agent/AgentToolHelpers.h \
    src/agent/CodeChunks.h \
    src/agent/CodeIndex.h \
    src/agent/AgentTurnView.h \
    src/ai/AiHub.h \
    src/claudecode/ClaudeCodeChanges.h \
    src/claudecode/ClaudeCodeCli.h \
    src/claudecode/ClaudeCodeHistory.h \
    src/claudecode/ClaudeCodeInfo.h \
    src/claudecode/ClaudeCodePanel.h \
    src/claudecode/ClaudeCodePrompt.h \
    src/claudecode/ClaudeCodeSession.h \
    src/claudecode/ClaudeCodeTasks.h \
    src/claudecode/ContextMeter.h \
    src/ai/AiChat.h \
    src/ai/AiChangeCard.h \
    src/ai/AiEdits.h \
    src/ai/AiEnv.h \
    src/ai/AiInput.h \
    src/ai/AiQuote.h \
    src/ai/AiKeyDialog.h \
    src/ai/AiWorkspace.h \
    src/ai/ClaudeClient.h \
    src/ai/GeminiClient.h \
    src/gradle/GradlePanel.h \
    src/maven/MavenPanel.h \
    src/maven/MavenProject.h \
    src/spring/SpringProject.h \
    src/spring/SpringServer.h \
    src/spring/ActuatorView.h \
    src/spring/SpringProperties.h \
    src/spring/SpringScaffold.h \
    src/spring/SpringStarters.h \
    src/spring/AddStarterDialog.h \
    src/spring/SpringLogView.h \
    src/spring/SpringPanel.h \
    src/http/HttpEnv.h \
    src/http/HttpFile.h \
    src/http/HttpPanel.h \
    src/http/HttpResponseView.h \
    src/http/HttpRunner.h \
    src/http/HttpUi.h \
    src/http/HttpVars.h \
    src/spring/HttpClientView.h \
    src/spring/SpringEndpoints.h \
    src/spring/SpringBar.h \
    src/ktor/AddKtorPluginDialog.h \
    src/ktor/KtorBar.h \
    src/ktor/KtorConfigDialog.h \
    src/ktor/KtorConfigView.h \
    src/ktor/KtorLive.h \
    src/ktor/KtorPanel.h \
    src/ktor/KtorPlugins.h \
    src/ktor/KtorProject.h \
    src/ktor/KtorReloader.h \
    src/ktor/KtorRoutesView.h \
    src/ktor/KtorServer.h \
    src/ktor/KtorSource.h \
    src/project/BuildFileEdit.h \
    src/spring/SpringConfigDialog.h \
    src/gradle/GradleTasks.h \
    src/gradle/GradleEvents.h \
    src/gradle/GradleBuildView.h \
    src/jvm/JvmTools.h \
    src/jvm/GradleProperties.h \
    src/jvm/JvmNewFile.h \
    src/jvm/JvmReference.h \
    src/jvm/KotlinAnnotations.h \
    src/jvm/JdkScanner.h \
    src/jvm/JdkDialog.h \
    src/jvm/MavenCentral.h \
    src/jvm/DependencyEdit.h \
    src/jvm/DependencyDialog.h \
    src/jvm/ScriptCheck.h \
    src/debug/JdwpWire.h \
    src/debug/DebugTypes.h \
    src/debug/DebugWorker.h \
    src/debug/DebugSession.h \
    src/debug/DebugPanel.h \
    src/media/MediaPanel.h \
    src/explorer/ProjectExplorer.h \
    src/explorer/FileIcons.h \
    src/explorer/GitItemDelegate.h \
    src/git/GitTypes.h \
    src/git/GitDiff.h \
    src/git/GitRepository.h \
    src/git/GitPanel.h \
    src/git/BranchButton.h \
    src/git/BranchPopup.h \
    src/editor/ZoomPopup.h \
    src/git/DiffDialog.h \
    src/git/PatchDialog.h \
    src/dialogs/NewProjectDialog.h \
    src/dialogs/NewFileDialog.h \
    src/dialogs/NewJvmClassDialog.h \
    src/dialogs/UnsavedChangesDialog.h \
    src/dialogs/UnusedImportsDialog.h \
    src/dialogs/CallHierarchyDialog.h \
    src/dialogs/ReferencesDialog.h \
    src/dialogs/LspLogDialog.h \
    src/dialogs/RunConfigDialog.h \
    src/dialogs/ThemeEditorDialog.h \
    src/dialogs/CompilerFlagsDialog.h \
    src/dialogs/IncludePathsDialog.h \
    src/dialogs/LspInstallDialog.h \
    src/dialogs/JdtlsInstallDialog.h \
    src/dialogs/LspRemoveDialog.h \
    src/dialogs/NpmInstallDialog.h \
    src/dialogs/PipInstallDialog.h \
    src/dialogs/PackagesDialog.h \
    src/editor/Language.h \
    src/editor/SyntaxHighlighter.h \
    src/editor/Document.h \
    src/editor/TextEncoding.h \
    src/editor/Breadcrumbs.h \
    src/editor/CsvData.h \
    src/editor/CsvTable.h \
    src/editor/CodeEditor.h \
    src/editor/CodeEditorText.h \
    src/editor/EditorConfig.h \
    src/history/LocalHistory.h \
    src/history/LocalHistoryDialog.h \
    src/git/DiffPane.h \
    src/editor/CompletionPopup.h \
    src/editor/HoverRender.h \
    src/editor/SignaturePopup.h \
    src/editor/Snippets.h \
    src/editor/EditPredict.h \
    src/editor/Emmet.h \
    src/editor/HoverPopup.h \
    src/editor/MiniMap.h \
    src/editor/FindBar.h \
    src/editor/EditorGroup.h \
    src/editor/EditorManager.h \
    src/editor/WelcomePage.h \
    src/terminal/TerminalScreen.h \
    src/terminal/ShellProcess.h \
    src/terminal/ShellIntegration.h \
    src/terminal/ShellSyntax.h \
    src/terminal/TerminalView.h \
    src/terminal/ProblemScanner.h \
    src/terminal/Terminal.h \
    src/terminal/TerminalPanel.h \
    src/lsp/LspClient.h \
    src/lsp/JarSource.h \
    src/lsp/LspInstaller.h \
    src/lsp/JdtlsInstaller.h \
    src/lsp/LspManager.h \
    src/lsp/LspServers.h \
    src/lsp/LspSync.h \
    src/lsp/NpmInstaller.h \
    src/lsp/PipInstaller.h \
    src/python/PythonTools.h \
    src/python/PackageManager.h \
    src/lsp/LspTypes.h \
    src/window/Island.h \
    src/window/SideSections.h \
    src/window/MainWindow.h

# The OS layer (src/platform/): one implementation per OS behind the same headers (AppPaths: one file for both).
SOURCES += src/platform/AppPaths.cpp
unix: SOURCES += src/platform/PlatformUnix.cpp src/platform/ProcessTreeUnix.cpp
win32: SOURCES += src/platform/PlatformWin.cpp src/platform/ProcessTreeWin.cpp src/platform/ConPtyWin.cpp
win32: HEADERS += src/platform/ConPty.h # the terminal's pseudo console (Windows only)
win32: LIBS += -ladvapi32 # PlatformWin: the user's SID

RESOURCES += resources/qode.qrc

# Keep generated files out of the source tree
OBJECTS_DIR = $$OUT_PWD/.obj
MOC_DIR = $$OUT_PWD/.moc
RCC_DIR = $$OUT_PWD/.rcc
UI_DIR = $$OUT_PWD/.ui

win32 {
    # qode.exe: icon, version resource (Properties > Details) and a manifest (UTF-8 code page, long paths).
    # Qt's win32-msvc mkspec already passes -utf-8 -permissive- -Zc:__cplusplus -MP and links as a GUI app.
    RC_ICONS = resources/icons/qode.ico
    QMAKE_TARGET_PRODUCT = QODE
    QMAKE_TARGET_DESCRIPTION = QODE
    QMAKE_TARGET_COMPANY = QODE
    QMAKE_TARGET_COPYRIGHT = QODE
    # Merged by the linker into the manifest it embeds anyway (Common Controls 6, asInvoker). Not QMAKE_MANIFEST:
    # with RC_ICONS that also puts the file into the generated .rc, and the link fails on a duplicate MANIFEST resource.
    QMAKE_LFLAGS += /MANIFESTINPUT:$$shell_quote($$system_path($$PWD/resources/windows/qode.manifest))
    # Absolute paths in the Makefile: with relative ones qmake writes header dependencies one "..\" too deep
    # (..\..\..\qt from build-win\). nmake does not notice, because Windows stops ".." at the drive root, but jom does.
    QMAKE_PROJECT_DEPTH = 0
}

# make install (Linux; Windows is packaged with windeployqt and an installer instead)
unix {
    isEmpty(PREFIX): PREFIX = /usr/local
    target.path = $$PREFIX/bin
    desktop.files = resources/qode.desktop
    desktop.path = $$PREFIX/share/applications
    icon.files = resources/icons/qode.svg
    icon.path = $$PREFIX/share/icons/hicolor/scalable/apps
    INSTALLS += target desktop icon
    # Each PNG size is installed as hicolor/<size>/apps/qode.png, the name the desktop entry's Icon= looks up.
    for(sz, $$list(16 32 48 64 128 256 512)) {
        pngicon = icon$${sz}
        $${pngicon}.path = $$PREFIX/share/icons/hicolor/$${sz}x$${sz}/apps
        $${pngicon}.extra = $(INSTALL_FILE) $$PWD/resources/icons/qode-$${sz}.png $(INSTALL_ROOT)$$PREFIX/share/icons/hicolor/$${sz}x$${sz}/apps/qode.png
        $${pngicon}.uninstall = -$(DEL_FILE) $(INSTALL_ROOT)$$PREFIX/share/icons/hicolor/$${sz}x$${sz}/apps/qode.png
        INSTALLS += $$pngicon
    }
}
