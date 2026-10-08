#include "JsonDoc.h"

namespace {

struct Parser
{
    const QString &s;
    int i = 0;
    QString error;

    bool fail(const QString &what)
    {
        int line = 1;
        for (int k = 0; k < i && k < s.size(); ++k)
            line += s.at(k) == QLatin1Char('\n');
        error = QStringLiteral("line %1: %2").arg(line).arg(what);
        return false;
    }
    void skip()
    {
        while (i < s.size() && s.at(i).isSpace())
            ++i;
    }
    bool literal(const char *word)
    {
        const QLatin1String w(word);
        if (s.mid(i, w.size()) != w)
            return false;
        i += w.size();
        return true;
    }
    bool string(QString *out)
    {
        ++i; // opening quote
        out->clear();
        while (i < s.size()) {
            const QChar c = s.at(i++);
            if (c == QLatin1Char('"'))
                return true;
            if (c != QLatin1Char('\\')) {
                if (c.unicode() < 0x20)
                    return fail(QStringLiteral("control character in string"));
                out->append(c);
                continue;
            }
            if (i >= s.size())
                break;
            const QChar e = s.at(i++);
            switch (e.unicode()) {
            case '"': out->append(QLatin1Char('"')); break;
            case '\\': out->append(QLatin1Char('\\')); break;
            case '/': out->append(QLatin1Char('/')); break;
            case 'b': out->append(QLatin1Char('\b')); break;
            case 'f': out->append(QLatin1Char('\f')); break;
            case 'n': out->append(QLatin1Char('\n')); break;
            case 'r': out->append(QLatin1Char('\r')); break;
            case 't': out->append(QLatin1Char('\t')); break;
            case 'u': {
                bool ok = false;
                const ushort cp = s.mid(i, 4).toUShort(&ok, 16);
                if (!ok || i + 4 > s.size())
                    return fail(QStringLiteral("bad \\u escape"));
                out->append(QChar(cp));
                i += 4;
                break;
            }
            default: return fail(QStringLiteral("bad escape in string"));
            }
        }
        return fail(QStringLiteral("unterminated string"));
    }
    bool value(JsonValue *v, int depth)
    {
        if (depth > 200)
            return fail(QStringLiteral("nested too deeply"));
        skip();
        if (i >= s.size())
            return fail(QStringLiteral("unexpected end"));
        const QChar c = s.at(i);
        if (c == QLatin1Char('{')) {
            ++i;
            v->type = JsonValue::Object;
            skip();
            if (i < s.size() && s.at(i) == QLatin1Char('}')) {
                ++i;
                return true;
            }
            for (;;) {
                skip();
                if (i >= s.size() || s.at(i) != QLatin1Char('"'))
                    return fail(QStringLiteral("expected a property name"));
                QString key;
                if (!string(&key))
                    return false;
                skip();
                if (i >= s.size() || s.at(i) != QLatin1Char(':'))
                    return fail(QStringLiteral("expected ':'"));
                ++i;
                JsonValue child;
                if (!value(&child, depth + 1))
                    return false;
                v->set(key, child);
                skip();
                if (i < s.size() && s.at(i) == QLatin1Char(',')) {
                    ++i;
                    continue;
                }
                if (i < s.size() && s.at(i) == QLatin1Char('}')) {
                    ++i;
                    return true;
                }
                return fail(QStringLiteral("expected ',' or '}'"));
            }
        }
        if (c == QLatin1Char('[')) {
            ++i;
            v->type = JsonValue::Array;
            skip();
            if (i < s.size() && s.at(i) == QLatin1Char(']')) {
                ++i;
                return true;
            }
            for (;;) {
                JsonValue child;
                if (!value(&child, depth + 1))
                    return false;
                v->items.append(child);
                skip();
                if (i < s.size() && s.at(i) == QLatin1Char(',')) {
                    ++i;
                    continue;
                }
                if (i < s.size() && s.at(i) == QLatin1Char(']')) {
                    ++i;
                    return true;
                }
                return fail(QStringLiteral("expected ',' or ']'"));
            }
        }
        if (c == QLatin1Char('"')) {
            v->type = JsonValue::String;
            return string(&v->text);
        }
        if (literal("true")) {
            v->type = JsonValue::Bool;
            v->boolean = true;
            return true;
        }
        if (literal("false")) {
            v->type = JsonValue::Bool;
            return true;
        }
        if (literal("null")) {
            v->type = JsonValue::Null;
            return true;
        }
        const int start = i;
        while (i < s.size() && (s.at(i).isDigit() || QStringLiteral("+-.eE").contains(s.at(i))))
            ++i;
        bool ok = false;
        s.mid(start, i - start).toDouble(&ok);
        if (!ok)
            return fail(QStringLiteral("unexpected character"));
        v->type = JsonValue::Number;
        v->text = s.mid(start, i - start);
        return true;
    }
};

QString quoted(const QString &t)
{
    QString out = QStringLiteral("\"");
    for (const QChar c : t) {
        switch (c.unicode()) {
        case '"': out += QStringLiteral("\\\""); break;
        case '\\': out += QStringLiteral("\\\\"); break;
        case '\n': out += QStringLiteral("\\n"); break;
        case '\r': out += QStringLiteral("\\r"); break;
        case '\t': out += QStringLiteral("\\t"); break;
        case '\b': out += QStringLiteral("\\b"); break;
        case '\f': out += QStringLiteral("\\f"); break;
        default:
            if (c.unicode() < 0x20)
                out += QStringLiteral("\\u%1").arg(c.unicode(), 4, 16, QLatin1Char('0'));
            else
                out += c;
        }
    }
    return out + QLatin1Char('"');
}

void write(const JsonValue &v, const QString &indent, int level, QString *out)
{
    switch (v.type) {
    case JsonValue::Null: *out += QStringLiteral("null"); break;
    case JsonValue::Bool: *out += v.boolean ? QStringLiteral("true") : QStringLiteral("false"); break;
    case JsonValue::Number: *out += v.text.isEmpty() ? QStringLiteral("0") : v.text; break;
    case JsonValue::String: *out += quoted(v.text); break;
    case JsonValue::Object:
    case JsonValue::Array: {
        const bool obj = v.type == JsonValue::Object;
        const int n = obj ? v.members.size() : v.items.size();
        if (n == 0) {
            *out += obj ? QStringLiteral("{}") : QStringLiteral("[]");
            break;
        }
        const QString pad = indent.repeated(level + 1);
        *out += obj ? QLatin1Char('{') : QLatin1Char('[');
        for (int k = 0; k < n; ++k) {
            *out += QLatin1Char('\n') + pad;
            if (obj) {
                *out += quoted(v.members.at(k).first) + QStringLiteral(": ");
                write(v.members.at(k).second, indent, level + 1, out);
            } else {
                write(v.items.at(k), indent, level + 1, out);
            }
            if (k + 1 < n)
                *out += QLatin1Char(',');
        }
        *out += QLatin1Char('\n') + indent.repeated(level) + (obj ? QLatin1Char('}') : QLatin1Char(']'));
        break;
    }
    }
}

} // namespace

JsonValue JsonValue::ofType(Type t)
{
    JsonValue v;
    v.type = t;
    if (t == Number)
        v.text = QStringLiteral("0");
    return v;
}

JsonValue *JsonValue::find(const QString &key)
{
    for (auto &m : members)
        if (m.first == key)
            return &m.second;
    return nullptr;
}

const JsonValue *JsonValue::find(const QString &key) const
{
    for (const auto &m : members)
        if (m.first == key)
            return &m.second;
    return nullptr;
}

void JsonValue::set(const QString &key, const JsonValue &v)
{
    if (JsonValue *e = find(key))
        *e = v;
    else
        members.append({key, v});
}

void JsonValue::remove(const QString &key)
{
    for (int k = 0; k < members.size(); ++k)
        if (members.at(k).first == key) {
            members.removeAt(k);
            return;
        }
}

bool JsonValue::parse(const QString &text, JsonValue *out, QString *error)
{
    Parser p{text, 0, QString()};
    if (text.startsWith(QChar(0xFEFF)))
        p.i = 1;
    JsonValue v;
    if (!p.value(&v, 0)) {
        if (error)
            *error = p.error;
        return false;
    }
    p.skip();
    if (p.i < text.size()) {
        p.fail(QStringLiteral("unexpected text after the end of the document"));
        if (error)
            *error = p.error;
        return false;
    }
    *out = v;
    return true;
}

QString JsonValue::toJson(const QString &indent) const
{
    QString out;
    write(*this, indent, 0, &out);
    return out;
}
