# QODE needs Qt 6 (Core, Gui, Widgets only)
lessThan(QT_MAJOR_VERSION, 6): error("QODE requires Qt 6 - build with qmake6.")

QT += core gui widgets network

# Video playback needs Qt Multimedia; without it the media panel still shows images.
qtHaveModule(multimedia) {
    QT += multimedia multimediawidgets
    DEFINES += QODE_HAS_MULTIMEDIA
}

CONFIG += c++17 warn_on
CONFIG -= app_bundle

TEMPLATE = app
TARGET = qode
VERSION = 0.7.0
LIBS += -lz # JarSource reads library source jars
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
    src/expo/ExpoConfigDialog.cpp \
    src/webdev/WebProject.cpp \
    src/webdev/DevServer.cpp \
    src/webdev/DevServerBar.cpp \
    src/webdev/ServerDialogs.cpp \
    src/webdev/EnvDialog.cpp \
    src/main.cpp \
    src/app/Application.cpp \
    src/app/InstanceRegistry.cpp \
    src/settings/SettingsManager.cpp \
    src/settings/Theme.cpp \
    src/settings/ThemeManager.cpp \
    src/settings/Icons.cpp \
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
    src/media/MarkdownHtml.cpp \
    src/media/MediaPanel.cpp \
    src/explorer/ProjectExplorer.cpp \
    src/explorer/FileIcons.cpp \
    src/explorer/GitItemDelegate.cpp \
    src/git/GitDiff.cpp \
    src/git/GitRepository.cpp \
    src/git/GitPanel.cpp \
    src/git/BranchButton.cpp \
    src/git/BranchPopup.cpp \
    src/git/DiffDialog.cpp \
    src/git/PatchDialog.cpp \
    src/dialogs/NewProjectDialog.cpp \
    src/dialogs/NewFileDialog.cpp \
    src/dialogs/UnsavedChangesDialog.cpp \
    src/dialogs/UnusedImportsDialog.cpp \
    src/dialogs/ReferencesDialog.cpp \
    src/dialogs/LspLogDialog.cpp \
    src/dialogs/RunConfigDialog.cpp \
    src/dialogs/ThemeEditorDialog.cpp \
    src/dialogs/CompilerFlagsDialog.cpp \
    src/dialogs/LspInstallDialog.cpp \
    src/dialogs/JdtlsInstallDialog.cpp \
    src/dialogs/LspRemoveDialog.cpp \
    src/dialogs/NpmInstallDialog.cpp \
    src/dialogs/PipInstallDialog.cpp \
    src/dialogs/PackagesDialog.cpp \
    src/editor/Language.cpp \
    src/editor/SyntaxHighlighter.cpp \
    src/editor/Document.cpp \
    src/editor/Breadcrumbs.cpp \
    src/editor/CodeEditor.cpp \
    src/editor/CompletionPopup.cpp \
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
    src/terminal/TerminalView.cpp \
    src/terminal/Terminal.cpp \
    src/terminal/TerminalPanel.cpp \
    src/lsp/LspClient.cpp \
    src/lsp/JarSource.cpp \
    src/lsp/LspInstaller.cpp \
    src/lsp/JdtlsInstaller.cpp \
    src/lsp/LspManager.cpp \
    src/lsp/LspServers.cpp \
    src/lsp/NpmInstaller.cpp \
    src/lsp/PipInstaller.cpp \
    src/python/PythonTools.cpp \
    src/python/PackageManager.cpp \
    src/window/Island.cpp \
    src/window/SideSections.cpp \
    src/window/MainWindow.cpp

HEADERS += \
    src/refactor/MoveRefactor.h \
    src/refactor/MoveController.h \
    src/explorer/ExplorerTree.h \
    src/refactor/RefactorCtx.h \
    src/expo/JsonDoc.h \
    src/expo/ExpoProject.h \
    src/expo/ExpoSchema.h \
    src/expo/ExpoBar.h \
    src/expo/ExpoConfigDialog.h \
    src/webdev/WebProject.h \
    src/webdev/DevServer.h \
    src/webdev/DevServerBar.h \
    src/webdev/ServerDialogs.h \
    src/webdev/EnvDialog.h \
    src/app/Application.h \
    src/app/InstanceRegistry.h \
    src/settings/SettingsManager.h \
    src/settings/Theme.h \
    src/settings/ThemeManager.h \
    src/settings/Icons.h \
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
    src/media/MarkdownHtml.h \
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
    src/git/DiffDialog.h \
    src/git/PatchDialog.h \
    src/dialogs/NewProjectDialog.h \
    src/dialogs/NewFileDialog.h \
    src/dialogs/UnsavedChangesDialog.h \
    src/dialogs/UnusedImportsDialog.h \
    src/dialogs/ReferencesDialog.h \
    src/dialogs/LspLogDialog.h \
    src/dialogs/RunConfigDialog.h \
    src/dialogs/ThemeEditorDialog.h \
    src/dialogs/CompilerFlagsDialog.h \
    src/dialogs/LspInstallDialog.h \
    src/dialogs/JdtlsInstallDialog.h \
    src/dialogs/LspRemoveDialog.h \
    src/dialogs/NpmInstallDialog.h \
    src/dialogs/PipInstallDialog.h \
    src/dialogs/PackagesDialog.h \
    src/editor/Language.h \
    src/editor/SyntaxHighlighter.h \
    src/editor/Document.h \
    src/editor/Breadcrumbs.h \
    src/editor/CodeEditor.h \
    src/editor/CompletionPopup.h \
    src/editor/Snippets.h \
    src/editor/Emmet.h \
    src/editor/HoverPopup.h \
    src/editor/MiniMap.h \
    src/editor/FindBar.h \
    src/editor/EditorGroup.h \
    src/editor/EditorManager.h \
    src/editor/WelcomePage.h \
    src/terminal/TerminalScreen.h \
    src/terminal/ShellProcess.h \
    src/terminal/TerminalView.h \
    src/terminal/Terminal.h \
    src/terminal/TerminalPanel.h \
    src/lsp/LspClient.h \
    src/lsp/JarSource.h \
    src/lsp/LspInstaller.h \
    src/lsp/JdtlsInstaller.h \
    src/lsp/LspManager.h \
    src/lsp/LspServers.h \
    src/lsp/NpmInstaller.h \
    src/lsp/PipInstaller.h \
    src/python/PythonTools.h \
    src/python/PackageManager.h \
    src/lsp/LspTypes.h \
    src/window/Island.h \
    src/window/SideSections.h \
    src/window/MainWindow.h

RESOURCES += resources/qode.qrc

# Keep generated files out of the source tree
OBJECTS_DIR = $$OUT_PWD/.obj
MOC_DIR = $$OUT_PWD/.moc
RCC_DIR = $$OUT_PWD/.rcc
UI_DIR = $$OUT_PWD/.ui

# make install
isEmpty(PREFIX): PREFIX = /usr/local
target.path = $$PREFIX/bin
desktop.files = resources/qode.desktop
desktop.path = $$PREFIX/share/applications
icon.files = resources/icons/qode.svg
icon.path = $$PREFIX/share/icons/hicolor/scalable/apps
INSTALLS += target desktop icon
for(sz, 16 32 48 64 128 256 512) {
    icon$${sz}.files = resources/icons/qode-$${sz}.png
    icon$${sz}.path = $$PREFIX/share/icons/hicolor/$${sz}x$${sz}/apps
    INSTALLS += icon$${sz}
}
