#pragma once

#include <QList>
#include <QPair>
#include <QString>

// A JSON value that remembers the order of object keys and the exact text of numbers (QJsonObject sorts its keys, which
// would reshuffle app.json on every save). Qt Core only.
struct JsonValue
{
    enum Type { Null, Bool, Number, String, Object, Array };
    Type type = Null;
    bool boolean = false;
    QString text; // the string, or the number as written
    QList<QPair<QString, JsonValue>> members; // Object
    QList<JsonValue> items;                   // Array

    static JsonValue ofType(Type t);
    JsonValue *find(const QString &key);
    const JsonValue *find(const QString &key) const;
    void set(const QString &key, const JsonValue &v); // replaces in place or appends
    void remove(const QString &key);
    bool isContainer() const { return type == Object || type == Array; }

    // Strict JSON (a UTF-8 BOM is skipped). Returns false and sets `error` ("line 3: ...") on failure.
    static bool parse(const QString &text, JsonValue *out, QString *error);
    // `indent` is the indentation unit ("  " or "\t"); no trailing newline.
    QString toJson(const QString &indent = QStringLiteral("  ")) const;
};
