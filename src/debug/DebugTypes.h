#pragma once

#include <QList>
#include <QMetaType>
#include <QString>

// Plain data the debugger's worker thread hands to the window.

// Breakpoints of one source file, with what is needed to find its classes in the JVM.
struct DebugSourceBreakpoints {
    QString path;       // the file in the editor
    QString sourceName; // its name as compiled into the classes' SourceFile attribute ("Main.kt")
    QString package;    // its declared package ("com.example"; "" = default package)
    QList<int> lines;   // 1-based
};

struct DebugFrame {
    quint64 frameId = 0;
    QString className;  // com.example.MainKt
    QString method;     // main
    QString sourceName; // Main.kt ("" without debug information)
    QString pkgPath;    // com/example
    int line = 0;       // 1-based, 0 = unknown
    QString path;       // filled in by the window: the project file, "" = not a project file
};

struct DebugVariable {
    QString name;
    QString type;       // java.lang.String, int, com.example.User …
    QString value;      // as shown ("\"abc\"", "42", "User(name=Ann)" …)
    quint64 objectId = 0; // > 0: expandable (fields / elements)
    char tag = 0;
};

struct DebugStop {
    quint64 thread = 0;
    QString threadName;
    QString reason;        // breakpoint, step, exception, pause
    QString exception;     // the exception's class and message (reason "exception")
    QList<DebugFrame> frames;
    QList<QPair<quint64, QString>> threads; // all threads: id, name
};

Q_DECLARE_METATYPE(DebugSourceBreakpoints)
Q_DECLARE_METATYPE(DebugFrame)
Q_DECLARE_METATYPE(DebugVariable)
Q_DECLARE_METATYPE(DebugStop)
