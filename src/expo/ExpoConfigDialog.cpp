#include "ExpoConfigDialog.h"

#include "ExpoSchema.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSaveFile>
#include <QStyle>
#include <QScrollArea>
#include <QStackedWidget>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <functional>

namespace {

using Type = JsonValue::Type;

QString typeLabel(Type t)
{
    switch (t) {
    case JsonValue::String: return QObject::tr("text");
    case JsonValue::Number: return QObject::tr("number");
    case JsonValue::Bool: return QObject::tr("switch");
    case JsonValue::Null: return QObject::tr("null");
    case JsonValue::Object: return QObject::tr("object");
    case JsonValue::Array: return QObject::tr("list");
    }
    return {};
}

QComboBox *typeCombo(QWidget *parent, bool scalarsOnly, Type current)
{
    auto *c = new QComboBox(parent);
    c->setObjectName(QStringLiteral("type"));
    c->setToolTip(QObject::tr("The type of this value"));
    c->setFixedWidth(88);
    QList<Type> types{JsonValue::String, JsonValue::Number, JsonValue::Bool, JsonValue::Null};
    if (!scalarsOnly)
        types << JsonValue::Object << JsonValue::Array;
    for (Type t : types)
        c->addItem(typeLabel(t), int(t));
    c->setCurrentIndex(qMax(0, c->findData(int(current))));
    return c;
}

Type typeFromSchema(const QJsonObject &entry)
{
    const QString t = entry.value(QStringLiteral("type")).toString();
    if (t == QLatin1String("object"))
        return JsonValue::Object;
    if (t == QLatin1String("array"))
        return JsonValue::Array;
    if (t == QLatin1String("boolean"))
        return JsonValue::Bool;
    if (t == QLatin1String("number") || t == QLatin1String("integer"))
        return JsonValue::Number;
    return JsonValue::String;
}

QStringList choicesOf(const QJsonObject &entry)
{
    QStringList out;
    for (const QJsonValue &v : entry.value(QStringLiteral("enum")).toArray())
        if (v.isString())
            out << v.toString();
    return out;
}

bool validNumber(const QString &t)
{
    static const QRegularExpression re(QStringLiteral("^-?(0|[1-9]\\d*)(\\.\\d+)?([eE][+-]?\\d+)?$"));
    return re.match(t).hasMatch();
}

} // namespace

// ---- the value editors ----------------------------------------------------------------------------------------

// Editor of one value. `changed` is called after every edit.
class ExpoNode : public QWidget
{
public:
    using QWidget::QWidget;
    virtual JsonValue value() const = 0;
    virtual void check(QStringList &problems, const QString &where) const { Q_UNUSED(problems) Q_UNUSED(where) }
    std::function<void()> changed;

protected:
    void notify()
    {
        if (changed)
            changed();
    }
};

class ExpoScalarNode : public ExpoNode
{
public:
    ExpoScalarNode(const JsonValue &v, const QStringList &choices, QWidget *parent) : ExpoNode(parent), m_type(v.type)
    {
        auto *l = new QHBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
        switch (v.type) {
        case JsonValue::String:
            if (!choices.isEmpty()) {
                m_combo = new QComboBox(this);
                m_combo->setObjectName(QStringLiteral("val"));
                m_combo->setEditable(true);
                m_combo->addItems(choices);
                m_combo->setCurrentText(v.text);
                connect(m_combo, &QComboBox::editTextChanged, this, [this] { notify(); });
                l->addWidget(m_combo, 1);
                break;
            }
            [[fallthrough]];
        case JsonValue::Number:
            m_edit = new QLineEdit(v.text, this);
            m_edit->setObjectName(QStringLiteral("val"));
            m_edit->setPlaceholderText(v.type == JsonValue::Number ? QObject::tr("number") : QObject::tr("empty"));
            m_edit->setCursorPosition(0);
            if (v.type == JsonValue::Number)
                m_edit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("-?[0-9]*\\.?[0-9]*([eE][+-]?[0-9]*)?")), m_edit));
            connect(m_edit, &QLineEdit::textEdited, this, [this] {
                if (m_type == JsonValue::Number)
                    m_edit->setProperty("invalid", !validNumber(m_edit->text()));
                m_edit->style()->unpolish(m_edit);
                m_edit->style()->polish(m_edit);
                notify();
            });
            l->addWidget(m_edit, 1);
            break;
        case JsonValue::Bool:
            m_check = new QCheckBox(this);
            m_check->setObjectName(QStringLiteral("switch"));
            m_check->setChecked(v.boolean);
            m_check->setText(v.boolean ? QObject::tr("true") : QObject::tr("false"));
            connect(m_check, &QCheckBox::toggled, this, [this](bool on) {
                m_check->setText(on ? QObject::tr("true") : QObject::tr("false"));
                notify();
            });
            l->addWidget(m_check);
            l->addStretch(1);
            break;
        default: {
            auto *n = new QLabel(QStringLiteral("null"), this);
            n->setObjectName(QStringLiteral("nullLabel"));
            l->addWidget(n);
            l->addStretch(1);
        }
        }
    }
    JsonValue value() const override
    {
        JsonValue v;
        v.type = m_type;
        if (m_type == JsonValue::String)
            v.text = m_combo ? m_combo->currentText() : m_edit->text();
        else if (m_type == JsonValue::Number)
            v.text = m_edit->text();
        else if (m_type == JsonValue::Bool)
            v.boolean = m_check->isChecked();
        return v;
    }
    void check(QStringList &problems, const QString &where) const override
    {
        if (m_type == JsonValue::Number && !validNumber(m_edit->text()))
            problems << QObject::tr("%1: “%2” is not a valid number").arg(where, m_edit->text());
    }

private:
    Type m_type;
    QLineEdit *m_edit = nullptr;
    QComboBox *m_combo = nullptr;
    QCheckBox *m_check = nullptr;
};

class ExpoRow;

// An object (rows with names) or an array (numbered rows) plus the "add" strip below them.
class ExpoContainerNode : public ExpoNode
{
public:
    struct Info
    {
        QString origin, key;
        JsonValue value;
    };
    ExpoContainerNode(Type type, const QJsonObject *schema, const QStringList &path, bool scalarsOnly, QWidget *parent);

    void addRow(const QString &key, const JsonValue &v, bool isNew);
    void removeRow(ExpoRow *row);
    void touched() { notify(); updateAddBar(); }
    int count() const;
    QStringList keys() const;
    QJsonObject propSchema(const QString &key) const;
    JsonValue value() const override;
    QList<Info> infos() const;
    void check(QStringList &problems, const QString &where) const override;
    const QJsonObject *schema() const { return m_schema; }
    const QStringList &path() const { return m_path; }
    bool isArray() const { return m_type == JsonValue::Array; }
    bool scalarsOnly() const { return m_scalarsOnly; }

private:
    void submitAdd();
    void updateAddBar();
    void renumber();
    QList<ExpoRow *> rows() const;

    Type m_type;
    const QJsonObject *m_schema;
    QStringList m_path;
    bool m_scalarsOnly;
    QVBoxLayout *m_rows;
    QLineEdit *m_newKey = nullptr;
    QComboBox *m_newType;
    QCompleter *m_completer = nullptr;
};

// "name | editor | type | remove"; objects and arrays also fold open to their own container below.
class ExpoRow : public QFrame
{
public:
    ExpoRow(ExpoContainerNode *owner, const QString &key, const JsonValue &v, bool isNew)
        : QFrame(owner), m_owner(owner), m_origin(isNew ? QString() : key), m_isItem(owner->isArray())
    {
        setObjectName(QStringLiteral("row"));
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(4);
        auto *head = new QHBoxLayout;
        head->setSpacing(6);
        m_caret = new QToolButton(this);
        m_caret->setObjectName(QStringLiteral("caret"));
        m_caret->setAutoRaise(true);
        m_caret->setFixedSize(20, 24);
        m_caret->setIconSize(QSize(14, 14));
        head->addWidget(m_caret);
        if (m_isItem) {
            m_index = new QLabel(this);
            m_index->setObjectName(QStringLiteral("index"));
            m_index->setMinimumWidth(34);
            head->addWidget(m_index);
        } else {
            m_key = new QLineEdit(key, this);
            m_key->setObjectName(QStringLiteral("key"));
            m_key->setMinimumWidth(190);
            m_key->setMaximumWidth(320);
            m_key->setCursorPosition(0);
            m_key->setPlaceholderText(QObject::tr("name"));
            const QJsonObject entry = owner->propSchema(key);
            const QString desc = entry.value(QStringLiteral("description")).toString();
            if (!desc.isEmpty())
                m_key->setToolTip(QStringLiteral("<qt>%1</qt>").arg(desc.toHtmlEscaped()));
            connect(m_key, &QLineEdit::textEdited, this, [this] { m_owner->touched(); });
            head->addWidget(m_key);
        }
        m_slot = new QHBoxLayout;
        m_slot->setContentsMargins(0, 0, 0, 0);
        head->addLayout(m_slot, 1);
        m_badge = new QLabel(this);
        m_badge->setObjectName(QStringLiteral("badge"));
        head->addWidget(m_badge, 1);
        m_type = typeCombo(this, owner->scalarsOnly(), v.type);
        head->addWidget(m_type);
        auto *del = new QToolButton(this);
        del->setObjectName(QStringLiteral("del"));
        del->setAutoRaise(true);
        del->setFixedSize(26, 26);
        del->setIconSize(QSize(14, 14));
        del->setToolTip(QObject::tr("Remove"));
        Icons::bind(del, QStringLiteral(":/new-icons/trash-2.svg"));
        head->addWidget(del);
        outer->addLayout(head);

        m_body = new QFrame(this);
        m_body->setObjectName(QStringLiteral("nested"));
        auto *bl = new QVBoxLayout(m_body);
        bl->setContentsMargins(14, 2, 0, 2);
        outer->addWidget(m_body);

        connect(del, &QToolButton::clicked, this, [this] { m_owner->removeRow(this); });
        connect(m_type, &QComboBox::activated, this, [this] { retype(Type(m_type->currentData().toInt())); });
        connect(m_caret, &QToolButton::clicked, this, [this] { setOpen(!m_body->isVisible()); });
        build(v);
    }

    QString origin() const { return m_origin; }
    QString key() const { return m_key ? m_key->text() : QString(); }
    JsonValue value() const { return m_node->value(); }
    void setIndex(int i)
    {
        if (m_index)
            m_index->setText(QString::number(i));
    }
    void check(QStringList &problems, const QString &where) const { m_node->check(problems, where); }

private:
    void setOpen(bool open)
    {
        m_body->setVisible(open);
        const Theme t = Theme::byName(SettingsManager::instance().theme());
        m_caret->setIcon(Icons::tinted(open ? QStringLiteral(":/new-icons/chevron-down.svg") : QStringLiteral(":/new-icons/chevron-right.svg"),
                                       t.textMuted));
    }
    void updateBadge()
    {
        auto *c = dynamic_cast<ExpoContainerNode *>(m_node);
        if (!c) {
            m_badge->clear();
            return;
        }
        const int n = c->count();
        if (c->isArray())
            m_badge->setText(n == 1 ? QObject::tr("1 item") : QObject::tr("%1 items").arg(n));
        else
            m_badge->setText(n == 1 ? QObject::tr("1 property") : QObject::tr("%1 properties").arg(n));
    }
    void build(const JsonValue &v)
    {
        if (m_node) {
            m_node->hide();
            m_node->deleteLater();
            m_node = nullptr;
        }
        QString name = key();
        if (m_isItem)
            name = QObject::tr("item");
        const QJsonObject entry = m_isItem ? QJsonObject() : m_owner->propSchema(name);
        if (v.isContainer()) {
            QStringList path = m_owner->path();
            path << (m_isItem ? QStringLiteral("*") : name);
            auto *c = new ExpoContainerNode(v.type, m_owner->schema(), path, false, m_body);
            for (const auto &m : v.members)
                c->addRow(m.first, m.second, false);
            for (const JsonValue &it : v.items)
                c->addRow(QString(), it, false);
            m_body->layout()->addWidget(c);
            m_node = c;
            m_caret->setVisible(true);
            m_badge->setVisible(true);
            setOpen(true);
        } else {
            m_node = new ExpoScalarNode(v, choicesOf(entry), this);
            m_slot->addWidget(m_node, 1);
            m_caret->setVisible(false);
            m_badge->setVisible(false);
            m_body->hide();
        }
        m_node->changed = [this] {
            updateBadge();
            m_owner->touched();
        };
        updateBadge();
        m_type->setCurrentIndex(qMax(0, m_type->findData(int(v.type))));
    }
    void retype(Type t)
    {
        const JsonValue old = m_node->value();
        if (old.type == t)
            return;
        JsonValue v = JsonValue::ofType(t);
        if (t == JsonValue::String) {
            v.text = old.type == JsonValue::Number ? old.text : old.type == JsonValue::Bool ? (old.boolean ? QStringLiteral("true") : QStringLiteral("false")) : QString();
        } else if (t == JsonValue::Number) {
            v.text = old.type == JsonValue::String && validNumber(old.text) ? old.text : QStringLiteral("0");
        } else if (t == JsonValue::Bool) {
            v.boolean = old.type == JsonValue::String && old.text == QLatin1String("true");
        }
        build(v);
        m_owner->touched();
    }

    ExpoContainerNode *m_owner;
    QString m_origin;
    bool m_isItem;
    QToolButton *m_caret;
    QLabel *m_index = nullptr;
    QLineEdit *m_key = nullptr;
    QHBoxLayout *m_slot;
    QLabel *m_badge;
    QComboBox *m_type;
    QFrame *m_body;
    ExpoNode *m_node = nullptr;
};

ExpoContainerNode::ExpoContainerNode(Type type, const QJsonObject *schema, const QStringList &path, bool scalarsOnly, QWidget *parent)
    : ExpoNode(parent), m_type(type), m_schema(schema), m_path(path), m_scalarsOnly(scalarsOnly)
{
    auto *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(6);
    auto *rowsHost = new QWidget(this);
    m_rows = new QVBoxLayout(rowsHost);
    m_rows->setContentsMargins(0, 0, 0, 0);
    m_rows->setSpacing(6);
    l->addWidget(rowsHost);

    auto *bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("addBar"));
    auto *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(8, 6, 8, 6);
    bl->setSpacing(6);
    auto *plus = new QLabel(bar);
    plus->setPixmap(Icons::pixmap(QStringLiteral(":/new-icons/plus.svg"), Theme::byName(SettingsManager::instance().theme()).textMuted, 14));
    bl->addWidget(plus);
    m_newType = typeCombo(bar, scalarsOnly, JsonValue::String);
    if (type == JsonValue::Object) {
        m_newKey = new QLineEdit(bar);
        m_newKey->setObjectName(QStringLiteral("val"));
        m_newKey->setPlaceholderText(scalarsOnly ? QObject::tr("Add a property: name, then Enter") : QObject::tr("Add a property or section: name, then Enter"));
        m_newKey->setClearButtonEnabled(true);
        bl->addWidget(m_newKey, 1);
        connect(m_newKey, &QLineEdit::returnPressed, this, [this] { submitAdd(); });
        // A name the schema knows picks the matching type.
        connect(m_newKey, &QLineEdit::textChanged, this, [this](const QString &t) {
            const QJsonObject e = propSchema(t.trimmed());
            if (e.isEmpty())
                return;
            const int idx = m_newType->findData(int(typeFromSchema(e)));
            if (idx >= 0)
                m_newType->setCurrentIndex(idx);
        });
    } else {
        auto *hint = new QLabel(QObject::tr("Add an item"), bar);
        hint->setObjectName(QStringLiteral("hint"));
        bl->addWidget(hint, 1);
    }
    bl->addWidget(m_newType);
    auto *add = new QPushButton(type == JsonValue::Object ? QObject::tr("Add") : QObject::tr("Add item"), bar);
    add->setObjectName(QStringLiteral("addBtn"));
    bl->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] { submitAdd(); });
    l->addWidget(bar);
}

QList<ExpoRow *> ExpoContainerNode::rows() const
{
    QList<ExpoRow *> out;
    for (int i = 0; i < m_rows->count(); ++i)
        if (auto *r = dynamic_cast<ExpoRow *>(m_rows->itemAt(i)->widget()))
            out << r;
    return out;
}

int ExpoContainerNode::count() const
{
    return rows().size();
}

QStringList ExpoContainerNode::keys() const
{
    QStringList out;
    for (ExpoRow *r : rows())
        out << r->key();
    return out;
}

QJsonObject ExpoContainerNode::propSchema(const QString &key) const
{
    if (!m_schema || m_schema->isEmpty() || key.isEmpty() || m_type != JsonValue::Object)
        return {};
    const QJsonObject e = ExpoSchema::propertiesAt(*m_schema, m_path).value(key).toObject();
    return e.isEmpty() ? e : ExpoSchema::resolveRef(*m_schema, e);
}

void ExpoContainerNode::addRow(const QString &key, const JsonValue &v, bool isNew)
{
    auto *row = new ExpoRow(this, key, v, isNew);
    m_rows->addWidget(row);
    renumber();
    updateAddBar();
}

void ExpoContainerNode::removeRow(ExpoRow *row)
{
    m_rows->removeWidget(row);
    row->hide();
    row->deleteLater();
    renumber();
    notify();
    QMetaObject::invokeMethod(this, [this] { updateAddBar(); }, Qt::QueuedConnection);
}

void ExpoContainerNode::renumber()
{
    int i = 0;
    for (ExpoRow *r : rows())
        r->setIndex(i++);
}

// The names of schema properties not used yet complete the "add" field.
void ExpoContainerNode::updateAddBar()
{
    if (!m_newKey || !m_schema || m_schema->isEmpty())
        return;
    QStringList names;
    const QJsonObject props = ExpoSchema::propertiesAt(*m_schema, m_path);
    const QStringList used = keys();
    for (auto it = props.begin(); it != props.end(); ++it) {
        if (it.key().startsWith(QLatin1Char('_')) || used.contains(it.key()))
            continue;
        const bool container = typeFromSchema(ExpoSchema::resolveRef(*m_schema, it.value().toObject())) >= JsonValue::Object;
        if (m_scalarsOnly && container)
            continue;
        names << it.key();
    }
    names.sort(Qt::CaseInsensitive);
    delete m_completer;
    m_completer = new QCompleter(names, this);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setFilterMode(Qt::MatchContains);
    m_newKey->setCompleter(m_completer);
}

void ExpoContainerNode::submitAdd()
{
    QString key;
    if (m_newKey) {
        key = m_newKey->text().trimmed();
        if (key.isEmpty())
            return;
        if (keys().contains(key)) {
            QToolTip::showText(m_newKey->mapToGlobal(QPoint(0, m_newKey->height())), QObject::tr("“%1” already exists here").arg(key), m_newKey);
            return;
        }
    }
    Type t = Type(m_newType->currentData().toInt());
    JsonValue v = JsonValue::ofType(t);
    if (t == JsonValue::String) {
        const QStringList choices = choicesOf(propSchema(key));
        if (!choices.isEmpty())
            v.text = choices.first();
    }
    addRow(key, v, true);
    if (m_newKey)
        m_newKey->clear();
    notify();
}

JsonValue ExpoContainerNode::value() const
{
    JsonValue v = JsonValue::ofType(m_type);
    for (ExpoRow *r : rows()) {
        if (m_type == JsonValue::Object)
            v.set(r->key(), r->value());
        else
            v.items.append(r->value());
    }
    return v;
}

QList<ExpoContainerNode::Info> ExpoContainerNode::infos() const
{
    QList<Info> out;
    for (ExpoRow *r : rows())
        out.append({r->origin(), r->key(), r->value()});
    return out;
}

void ExpoContainerNode::check(QStringList &problems, const QString &where) const
{
    QStringList seen;
    int i = 0;
    for (ExpoRow *r : rows()) {
        const QString name = m_type == JsonValue::Object ? r->key() : QStringLiteral("[%1]").arg(i);
        const QString here = where.isEmpty() ? name : where + (m_type == JsonValue::Object ? QStringLiteral(" › ") : QString()) + name;
        if (m_type == JsonValue::Object) {
            if (name.isEmpty())
                problems << QObject::tr("%1: a property has no name").arg(where.isEmpty() ? QObject::tr("General") : where);
            else if (seen.contains(name))
                problems << QObject::tr("%1: “%2” appears twice").arg(where.isEmpty() ? QObject::tr("General") : where, name);
            seen << name;
        }
        r->check(problems, here);
        ++i;
    }
}

// ---- the dialog ------------------------------------------------------------------------------------------------

namespace {
QString rgba(const QColor &c, int alpha)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha);
}
} // namespace

ExpoConfigDialog::ExpoConfigDialog(const ExpoApp &app, const QJsonObject &schema, QWidget *parent)
    : QDialog(parent), m_app(app), m_schema(schema)
{
    setWindowTitle(tr("App Configuration — %1").arg(app.name));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 680);
    setMinimumSize(640, 460);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 16, 18, 14);
    outer->setSpacing(12);

    auto *head = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(1);
    auto *title = new QLabel(tr("App Configuration"), this);
    title->setObjectName(QStringLiteral("title"));
    auto *sub = new QLabel(this);
    sub->setObjectName(QStringLiteral("sub"));
    sub->setText(tr("%1  ·  %2%3").arg(app.name, QFileInfo(app.appJson).fileName(),
                                       app.sdk.isEmpty() ? QString() : tr("  ·  SDK %1").arg(app.sdk.section(QLatin1Char('.'), 0, 0))));
    titles->addWidget(title);
    titles->addWidget(sub);
    head->addLayout(titles, 1);
    auto *schemaPill = new QLabel(m_schema.isEmpty() ? tr("No schema") : tr("Schema ✓"), this);
    schemaPill->setObjectName(QStringLiteral("pill"));
    schemaPill->setToolTip(m_schema.isEmpty() ? tr("The Expo schema is not downloaded yet: no descriptions or name suggestions.")
                                              : tr("Descriptions, name suggestions and choices come from Expo's published schema."));
    head->addWidget(schemaPill, 0, Qt::AlignTop);
    outer->addLayout(head);

    auto *body = new QHBoxLayout;
    body->setSpacing(14);
    m_nav = new QListWidget(this);
    m_nav->setObjectName(QStringLiteral("nav"));
    m_nav->setFixedWidth(200);
    m_nav->setFrameShape(QFrame::NoFrame);
    body->addWidget(m_nav);
    m_pages = new QStackedWidget(this);
    body->addWidget(m_pages, 1);
    outer->addLayout(body, 1);

    auto *foot = new QHBoxLayout;
    m_dirty = new QLabel(this);
    m_dirty->setObjectName(QStringLiteral("sub"));
    foot->addWidget(m_dirty, 1);
    auto *openBtn = new QPushButton(tr("Open in Editor"), this);
    openBtn->setToolTip(tr("Edit %1 as text").arg(QFileInfo(app.appJson).fileName()));
    m_cancel = new QPushButton(tr("Cancel"), this);
    m_save = new QPushButton(tr("Save"), this);
    m_save->setObjectName(QStringLiteral("primaryBtn"));
    m_save->setDefault(true);
    foot->addWidget(openBtn);
    foot->addWidget(m_cancel);
    foot->addWidget(m_save);
    outer->addLayout(foot);

    connect(openBtn, &QPushButton::clicked, this, [this] {
        emit openInEditorRequested(m_app.appJson);
        reject();
    });
    connect(m_cancel, &QPushButton::clicked, this, &ExpoConfigDialog::reject);
    connect(m_save, &QPushButton::clicked, this, &ExpoConfigDialog::save);
    connect(m_nav, &QListWidget::currentRowChanged, this, [this](int row) {
        QListWidgetItem *it = m_nav->item(row);
        if (!it)
            return;
        if (it->data(Qt::UserRole + 1).toBool()) { // "+ New section"
            newSection();
            return;
        }
        for (const Section &sec : std::as_const(m_sections))
            if (sec.item == it)
                m_pages->setCurrentWidget(sec.page);
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &ExpoConfigDialog::applyTheme);
    applyTheme();
    if (!load())
        return;
    updateDirty();
}

void ExpoConfigDialog::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    setStyleSheet(QStringLiteral(R"(
QLabel#title { font-size: 15pt; font-weight: 700; }
QLabel#sub { color: %1; }
QLabel#pill { color: %1; border: 1px solid %3; border-radius: 9px; padding: 2px 9px; font-size: 8pt; }
QLabel#secTitle { font-size: 13pt; font-weight: 700; }
QLabel#secText { color: %1; }
QLabel#index, QLabel#badge, QLabel#hint, QLabel#nullLabel { color: %1; }
QLabel#index { font-family: monospace; }
QListWidget#nav { background: %2; border: 1px solid %3; border-radius: 10px; padding: 6px; outline: 0; }
QListWidget#nav::item { padding: 8px 10px; border-radius: 6px; margin: 1px 0; }
QListWidget#nav::item:hover { background: %5; }
QListWidget#nav::item:selected { background: %4; color: %6; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QFrame#card { background: %2; border: 1px solid %3; border-radius: 10px; }
QFrame#nested { background: transparent; border: none; border-left: 2px solid %3; margin-left: 8px; }
QFrame#row { background: transparent; border: none; }
QFrame#addBar { background: %7; border: 1px dashed %3; border-radius: 8px; }
QLineEdit#key { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 4px 6px; font-weight: 600; }
QLineEdit#key:hover { border-color: %3; }
QLineEdit#key:focus { border-color: %4; background: %7; }
QLineEdit#val, QComboBox#val { background: %7; border: 1px solid %3; border-radius: 6px; padding: 4px 8px; }
QLineEdit#val:focus, QComboBox#val:focus { border-color: %4; }
QLineEdit#val[invalid="true"] { border-color: %8; }
QComboBox#type { background: transparent; border: 1px solid %3; border-radius: 6px; padding: 3px 6px; color: %1; font-size: 8pt; }
QToolButton#del { border-radius: 6px; }
QToolButton#del:hover { background: %9; }
QToolButton#caret { border: none; }
QCheckBox#switch { spacing: 8px; }
QPushButton#addBtn { padding: 4px 12px; }
)")
                      .arg(t.textMuted.name(), t.panel.name(), t.border.name(), t.accent.name(), rgba(t.accent, 28), t.onAccent().name(),
                           t.editorBg.name(), t.danger.name(), rgba(t.danger, 40)));
}

QString ExpoConfigDialog::indentUnit() const
{
    static const QRegularExpression lead(QStringLiteral("\\n([ \\t]+)\\S"));
    const auto m = lead.match(m_originalText);
    if (!m.hasMatch())
        return QStringLiteral("  ");
    const QString ws = m.captured(1);
    return ws.startsWith(QLatin1Char('\t')) ? QStringLiteral("\t") : QString(qBound(1, ws.size(), 8), QLatin1Char(' '));
}

void ExpoConfigDialog::showError(const QString &message)
{
    m_nav->hide();
    auto *page = new QLabel(message, this);
    page->setObjectName(QStringLiteral("secText"));
    page->setWordWrap(true);
    page->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_pages->addWidget(page);
    m_save->hide();
    m_cancel->setText(tr("Close"));
}

bool ExpoConfigDialog::load()
{
    QFile f(m_app.appJson);
    if (!f.open(QIODevice::ReadOnly)) {
        showError(tr("Could not read %1: %2").arg(m_app.appJson, f.errorString()));
        return false;
    }
    m_originalText = QString::fromUtf8(f.readAll());
    QString error;
    if (!JsonValue::parse(m_originalText, &m_root, &error) || m_root.type != JsonValue::Object) {
        showError(tr("%1 is not plain JSON that this form can edit (%2).\n\nUse “Open in Editor” to fix it first. Comments and trailing "
                     "commas are not supported here.")
                      .arg(QFileInfo(m_app.appJson).fileName(), error.isEmpty() ? tr("it is not an object") : error));
        return false;
    }
    JsonValue *expo = m_root.find(QStringLiteral("expo"));
    m_bare = !(expo && expo->type == JsonValue::Object);
    const JsonValue scope = m_bare ? m_root : *expo;
    for (const auto &m : scope.members)
        m_originalOrder << m.first;

    // General: every plain value of the scope.
    JsonValue general = JsonValue::ofType(JsonValue::Object);
    for (const auto &m : scope.members)
        if (!m.second.isContainer())
            general.members.append(m);
    addSection(QString(), general);
    for (const auto &m : scope.members)
        if (m.second.isContainer())
            addSection(m.first, m.second);
    addNavTail();
    m_nav->setCurrentRow(0);
    m_original = collect().toJson();
    return true;
}

ExpoConfigDialog::Section &ExpoConfigDialog::addSection(const QString &key, const JsonValue &value)
{
    Section s;
    s.key = key;
    const bool general = key.isEmpty();
    s.item = new QListWidgetItem(general ? tr("General") : key);
    // Inserted before the trailing "+ New section" entry, if it exists already.
    const int tail = m_nav->count() && m_nav->item(m_nav->count() - 1)->data(Qt::UserRole + 1).toBool() ? m_nav->count() - 1 : m_nav->count();
    m_nav->insertItem(tail, s.item);

    auto *scroll = new QScrollArea(m_pages);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *content = new QWidget;
    auto *col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 8, 0);
    col->setSpacing(12);

    auto *head = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto *t = new QLabel(general ? tr("General") : key, content);
    t->setObjectName(QStringLiteral("secTitle"));
    titles->addWidget(t);
    QString desc = general ? tr("Name, version, icon and the other plain settings of the app.") : QString();
    if (!general && !m_schema.isEmpty()) {
        const QJsonObject raw = ExpoSchema::propertiesAt(m_schema, {}).value(key).toObject();
        desc = raw.value(QStringLiteral("description")).toString();
        if (desc.isEmpty())
            desc = ExpoSchema::resolveRef(m_schema, raw).value(QStringLiteral("description")).toString();
    }
    if (!desc.isEmpty()) {
        // Expo's descriptions are Markdown: show links as links.
        static const QRegularExpression link(QStringLiteral("\\[([^\\]]+)\\]\\((https?://[^)]+)\\)"));
        QString html = desc.toHtmlEscaped();
        html.replace(link, QStringLiteral("<a href=\"\\2\">\\1</a>"));
        auto *d = new QLabel(html, content);
        d->setTextFormat(Qt::RichText);
        d->setOpenExternalLinks(true);
        d->setObjectName(QStringLiteral("secText"));
        d->setWordWrap(true);
        titles->addWidget(d);
    }
    head->addLayout(titles, 1);
    if (!general) {
        auto *del = new QPushButton(tr("Delete Section"), content);
        connect(del, &QPushButton::clicked, this, [this, key] { deleteSection(key); });
        head->addWidget(del, 0, Qt::AlignTop);
    }
    col->addLayout(head);

    auto *card = new QFrame(content);
    card->setObjectName(QStringLiteral("card"));
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(14, 12, 14, 12);
    s.node = new ExpoContainerNode(value.type, &m_schema, general ? QStringList() : QStringList{key}, general, card);
    for (const auto &m : value.members)
        s.node->addRow(m.first, m.second, false);
    for (const JsonValue &it : value.items)
        s.node->addRow(QString(), it, false);
    s.node->changed = [this] { updateDirty(); };
    cl->addWidget(s.node);
    col->addWidget(card);
    col->addStretch(1);
    scroll->setWidget(content);
    s.page = scroll;
    m_pages->addWidget(scroll);
    m_sections.append(s);
    return m_sections.last();
}

void ExpoConfigDialog::addNavTail()
{
    auto *tail = new QListWidgetItem(tr("＋  New section…"));
    tail->setData(Qt::UserRole + 1, true);
    tail->setForeground(palette().placeholderText());
    m_nav->addItem(tail);
}

void ExpoConfigDialog::newSection()
{
    QWidget *back = m_pages->currentWidget();
    auto restore = [this, back] {
        for (const Section &sec : std::as_const(m_sections))
            if (sec.page == back) {
                QSignalBlocker b(m_nav);
                m_nav->setCurrentItem(sec.item);
                return;
            }
    };
    QDialog dlg(this);
    dlg.setWindowTitle(tr("New Section"));
    auto *l = new QVBoxLayout(&dlg);
    l->addWidget(new QLabel(tr("A section holds a group of related settings (an object, like “android”) or a list (like “plugins”)."), &dlg));
    auto *name = new QLineEdit(&dlg);
    name->setPlaceholderText(tr("Section name, e.g. extra"));
    QStringList known;
    const QJsonObject props = m_schema.isEmpty() ? QJsonObject() : ExpoSchema::propertiesAt(m_schema, {});
    for (auto it = props.begin(); it != props.end(); ++it)
        if (!it.key().startsWith(QLatin1Char('_')) && typeFromSchema(ExpoSchema::resolveRef(m_schema, it.value().toObject())) >= JsonValue::Object)
            known << it.key();
    for (const Section &s : std::as_const(m_sections))
        known.removeAll(s.key);
    auto *completer = new QCompleter(known, name);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    name->setCompleter(completer);
    auto *type = new QComboBox(&dlg);
    type->addItem(typeLabel(JsonValue::Object), int(JsonValue::Object));
    type->addItem(typeLabel(JsonValue::Array), int(JsonValue::Array));
    connect(name, &QLineEdit::textChanged, &dlg, [&](const QString &text) {
        const QJsonObject e = props.value(text.trimmed()).toObject();
        if (!e.isEmpty())
            type->setCurrentIndex(typeFromSchema(ExpoSchema::resolveRef(m_schema, e)) == JsonValue::Array ? 1 : 0);
    });
    l->addWidget(name);
    l->addWidget(type);
    auto *row = new QHBoxLayout;
    row->addStretch(1);
    auto *cancel = new QPushButton(tr("Cancel"), &dlg);
    auto *ok = new QPushButton(tr("Add"), &dlg);
    ok->setObjectName(QStringLiteral("primaryBtn"));
    ok->setDefault(true);
    row->addWidget(cancel);
    row->addWidget(ok);
    l->addLayout(row);
    connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
    if (dlg.exec() != QDialog::Accepted) {
        restore();
        return;
    }
    const QString key = name->text().trimmed();
    if (key.isEmpty()) {
        restore();
        return;
    }
    for (const Section &s : std::as_const(m_sections))
        if (s.key == key) {
            m_nav->setCurrentItem(s.item); // already there: just show it
            return;
        }
    Section &s = addSection(key, JsonValue::ofType(Type(type->currentData().toInt())));
    m_nav->setCurrentItem(s.item);
    updateDirty();
}

void ExpoConfigDialog::deleteSection(const QString &key)
{
    if (QMessageBox::question(this, tr("Delete Section"), tr("Delete the section “%1” and everything in it? (Nothing is written until you save.)").arg(key)) !=
        QMessageBox::Yes)
        return;
    for (int i = 0; i < m_sections.size(); ++i)
        if (m_sections[i].key == key) {
            Section s = m_sections.takeAt(i);
            m_nav->setCurrentRow(0);
            delete s.item;
            m_pages->removeWidget(s.page);
            s.page->deleteLater();
            break;
        }
    updateDirty();
}

// The edited document: the original with its (possibly changed) scope written back in the original key order.
JsonValue ExpoConfigDialog::collect() const
{
    QList<ExpoContainerNode::Info> general;
    QList<QPair<QString, JsonValue>> containers;
    for (const Section &s : m_sections) {
        if (s.key.isEmpty())
            general = s.node->infos();
        else
            containers.append({s.key, s.node->value()});
    }
    JsonValue scope = JsonValue::ofType(JsonValue::Object);
    QList<bool> usedGeneral(general.size(), false);
    for (const QString &orig : m_originalOrder) {
        bool placed = false;
        for (int i = 0; i < general.size() && !placed; ++i)
            if (!usedGeneral[i] && general[i].origin == orig) {
                scope.set(general[i].key, general[i].value);
                usedGeneral[i] = placed = true;
            }
        for (int i = 0; i < containers.size() && !placed; ++i)
            if (containers[i].first == orig) {
                scope.set(orig, containers[i].second);
                placed = true;
            }
    }
    for (int i = 0; i < general.size(); ++i)
        if (!usedGeneral[i])
            scope.set(general[i].key, general[i].value);
    for (const auto &c : containers)
        if (!scope.find(c.first))
            scope.set(c.first, c.second);

    if (m_bare)
        return scope;
    JsonValue root = m_root;
    root.set(QStringLiteral("expo"), scope);
    return root;
}

void ExpoConfigDialog::updateDirty()
{
    const bool dirty = collect().toJson() != m_original;
    m_dirty->setText(dirty ? tr("Unsaved changes") : QString());
    m_save->setEnabled(dirty);
}

void ExpoConfigDialog::reject()
{
    if (m_save->isVisible() && m_save->isEnabled() &&
        QMessageBox::question(this, tr("Discard Changes"), tr("Close without saving your changes to %1?").arg(QFileInfo(m_app.appJson).fileName())) !=
            QMessageBox::Yes)
        return;
    QDialog::reject();
}

void ExpoConfigDialog::save()
{
    QStringList problems;
    for (const Section &s : std::as_const(m_sections))
        s.node->check(problems, s.key.isEmpty() ? tr("General") : s.key);
    if (!problems.isEmpty()) {
        QMessageBox::warning(this, tr("Fix These First"), problems.mid(0, 12).join(QLatin1Char('\n')));
        return;
    }
    QString text = collect().toJson(indentUnit());
    text += QLatin1Char('\n');
    QSaveFile out(m_app.appJson);
    if (!out.open(QIODevice::WriteOnly) || out.write(text.toUtf8()) < 0 || !out.commit()) {
        QMessageBox::critical(this, tr("Save Failed"), tr("Could not write %1: %2").arg(m_app.appJson, out.errorString()));
        return;
    }
    m_original = collect().toJson();
    emit saved(m_app.appJson);
    QDialog::accept();
}
