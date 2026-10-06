# {{name}} needs Qt 6 (build with qmake6)
lessThan(QT_MAJOR_VERSION, 6): error("{{name}} requires Qt 6 - build with qmake6.")

QT += core gui widgets

CONFIG += c++17 warn_on
CONFIG -= app_bundle

TEMPLATE = app
TARGET = {{nameId}}
VERSION = 1.0.0
DEFINES += QT_DEPRECATED_WARNINGS

INCLUDEPATH += $$PWD/src

# New files must be listed here.
SOURCES += \
    src/main.cpp \
    src/MainWindow.cpp

HEADERS += \
    src/MainWindow.h
{{#if resources}}

RESOURCES += resources/app.qrc
{{/if}}
