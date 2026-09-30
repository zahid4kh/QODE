QT += core gui widgets

CONFIG += c++17 warn_on
CONFIG -= app_bundle

TEMPLATE = app
TARGET = qode
VERSION = 0.1.0
DEFINES += QT_DEPRECATED_WARNINGS QT_NO_CAST_TO_ASCII QODE_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += $$PWD/src

SOURCES += \
    src/main.cpp \
    src/app/Application.cpp \
    src/settings/SettingsManager.cpp \
    src/settings/Theme.cpp \
    src/filesystem/FileManager.cpp \
    src/project/ProjectManager.cpp \
    src/project/ProjectModel.cpp \
    src/explorer/ProjectExplorer.cpp \
    src/dialogs/NewProjectDialog.cpp \
    src/dialogs/NewFileDialog.cpp \
    src/dialogs/UnsavedChangesDialog.cpp \
    src/editor/Language.cpp \
    src/editor/SyntaxHighlighter.cpp \
    src/editor/Document.cpp \
    src/editor/CodeEditor.cpp \
    src/editor/FindBar.cpp \
    src/editor/EditorManager.cpp \
    src/terminal/TerminalScreen.cpp \
    src/terminal/ShellProcess.cpp \
    src/terminal/TerminalView.cpp \
    src/terminal/Terminal.cpp \
    src/window/MainWindow.cpp

HEADERS += \
    src/app/Application.h \
    src/settings/SettingsManager.h \
    src/settings/Theme.h \
    src/filesystem/FileManager.h \
    src/project/Project.h \
    src/project/ProjectManager.h \
    src/project/ProjectModel.h \
    src/explorer/ProjectExplorer.h \
    src/dialogs/NewProjectDialog.h \
    src/dialogs/NewFileDialog.h \
    src/dialogs/UnsavedChangesDialog.h \
    src/editor/Language.h \
    src/editor/SyntaxHighlighter.h \
    src/editor/Document.h \
    src/editor/CodeEditor.h \
    src/editor/FindBar.h \
    src/editor/EditorManager.h \
    src/terminal/TerminalScreen.h \
    src/terminal/ShellProcess.h \
    src/terminal/TerminalView.h \
    src/terminal/Terminal.h \
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
