#include "EnvDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

const QRegularExpression &entryPattern()
{
    static const QRegularExpression re(QStringLiteral(R"(^(\s*export\s+)?([A-Za-z_][A-Za-z0-9_.]*)\s*=\s*(.*)$)"));
    return re;
}

// The value of the right-hand side of KEY=..., the way dotenv reads it.
QString parseValue(const QString &raw)
{
    const QString t = raw.trimmed();
    if (t.isEmpty())
        return {};
    const QChar q = t.at(0);
    if (q == QLatin1Char('\'') || q == QLatin1Char('"') || q == QLatin1Char('`')) {
        const int end = t.indexOf(q, 1);
        if (end > 0) {
            QString v = t.mid(1, end - 1);
            if (q == QLatin1Char('"'))
                v.replace(QStringLiteral("\\n"), QStringLiteral("\n")).replace(QStringLiteral("\\\""), QStringLiteral("\""));
            return v;
        }
    }
    const int hash = t.indexOf(QRegularExpression(QStringLiteral(R"(\s#)")));
    return (hash >= 0 ? t.left(hash) : t).trimmed();
}

QString formatValue(const QString &v)
{
    static const QRegularExpression plain(QStringLiteral(R"(^[A-Za-z0-9_\-./:@,+%=]*$)"));
    if (plain.match(v).hasMatch())
        return v;
    if (!v.contains(QLatin1Char('\'')) && !v.contains(QLatin1Char('\n')))
        return QLatin1Char('\'') + v + QLatin1Char('\''); // literal: no $ expansion
    QString e = v;
    e.replace(QStringLiteral("\\"), QStringLiteral("\\\\")).replace(QStringLiteral("\""), QStringLiteral("\\\""))
        .replace(QStringLiteral("\n"), QStringLiteral("\\n"));
    return QLatin1Char('"') + e + QLatin1Char('"');
}

} // namespace

EnvDialog::EnvDialog(const QString &dir, bool serverActive, QWidget *parent) : QDialog(parent), m_dir(dir)
{
    setWindowTitle(tr("Environment Variables"));
    resize(620, 460);
    auto *layout = new QVBoxLayout(this);

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("File"), this));
    m_files = new QComboBox(this);
    QStringList names = QDir(dir).entryList({QStringLiteral(".env"), QStringLiteral(".env.*")}, QDir::Files | QDir::Hidden, QDir::Name);
    for (const QString &d : {QStringLiteral(".env"), QStringLiteral(".env.local")})
        if (!names.contains(d))
            names << d;
    names.sort();
    for (const QString &n : std::as_const(names))
        m_files->addItem(QFile::exists(pathOf(n)) ? n : tr("%1 (new)").arg(n), n);
    m_files->setCurrentIndex(qMax(0, m_files->findData(QStringLiteral(".env.local")) >= 0 && QFile::exists(pathOf(QStringLiteral(".env.local")))
                                          ? m_files->findData(QStringLiteral(".env.local"))
                                          : m_files->findData(QStringLiteral(".env"))));
    top->addWidget(m_files, 1);
    auto *open = new QPushButton(tr("Open in Editor"), this);
    open->setToolTip(tr("Edit the file as text instead"));
    top->addWidget(open);
    layout->addLayout(top);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    layout->addWidget(m_note);

    m_table = new QTableWidget(0, 2, this);
    m_table->setHorizontalHeaderLabels({tr("Name"), tr("Value")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_table->setColumnWidth(0, 220);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(m_table, 1);

    auto *rows = new QHBoxLayout;
    auto *add = new QPushButton(tr("Add Variable"), this);
    auto *remove = new QPushButton(tr("Remove"), this);
    rows->addWidget(add);
    rows->addWidget(remove);
    rows->addStretch(1);
    layout->addLayout(rows);

    m_restart = new QCheckBox(tr("Restart the dev server after saving"), this);
    m_restart->setChecked(true);
    m_restart->setVisible(serverActive);
    layout->addWidget(m_restart);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *saveBtn = new QPushButton(tr("Save"), this);
    saveBtn->setDefault(true);
    auto *close = new QPushButton(tr("Close"), this);
    buttons->addWidget(saveBtn);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, [this] {
        addRow();
        m_table->setCurrentCell(m_table->rowCount() - 1, 0);
        m_table->editItem(m_table->item(m_table->rowCount() - 1, 0));
        m_dirty = true;
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const auto rows = m_table->selectionModel()->selectedRows();
        for (int i = rows.size() - 1; i >= 0; --i)
            m_table->removeRow(rows.at(i).row());
        m_dirty = true;
    });
    connect(m_table, &QTableWidget::itemChanged, this, [this] {
        if (!m_loading)
            m_dirty = true;
    });
    connect(m_files, &QComboBox::activated, this, [this] {
        if (m_files->currentData().toString() == m_loaded)
            return;
        if (confirmDiscard())
            loadFile(currentName());
        else
            m_files->setCurrentIndex(m_files->findData(m_loaded));
    });
    connect(open, &QPushButton::clicked, this, [this] {
        const QString path = pathOf(currentName());
        if (!QFile::exists(path)) {
            QFile f(path); // an empty file so the editor has something to open
            f.open(QIODevice::WriteOnly);
        }
        emit openFileRequested(path);
        accept();
    });
    connect(saveBtn, &QPushButton::clicked, this, [this] {
        if (save())
            accept();
    });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    loadFile(currentName());
}

QString EnvDialog::currentName() const
{
    return m_files->currentData().toString();
}

QString EnvDialog::pathOf(const QString &name) const
{
    return m_dir + QLatin1Char('/') + name;
}

void EnvDialog::addRow(const QString &key, const QString &value)
{
    const bool was = m_loading;
    m_loading = true;
    const int r = m_table->rowCount();
    m_table->insertRow(r);
    m_table->setItem(r, 0, new QTableWidgetItem(key));
    m_table->setItem(r, 1, new QTableWidgetItem(value));
    m_loading = was;
}

bool EnvDialog::confirmDiscard()
{
    if (!m_dirty)
        return true;
    return QMessageBox::question(this, tr("Environment Variables"), tr("Discard the unsaved changes to %1?").arg(m_loaded)) ==
           QMessageBox::Yes;
}

void EnvDialog::loadFile(const QString &name)
{
    m_loading = true;
    m_table->setRowCount(0);
    m_rawLines.clear();
    m_original.clear();
    m_loaded = name;
    QFile f(pathOf(name));
    if (f.open(QIODevice::ReadOnly)) {
        m_rawLines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
        if (m_rawLines.size() > 1 && m_rawLines.last().isEmpty())
            m_rawLines.removeLast();
        for (QString line : std::as_const(m_rawLines)) {
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            const auto m = entryPattern().match(line);
            if (line.trimmed().startsWith(QLatin1Char('#')) || !m.hasMatch())
                continue;
            m_original.append({m.captured(2), parseValue(m.captured(3))});
        }
    }
    for (const Entry &e : std::as_const(m_original))
        addRow(e.key, e.value);
    m_loading = false;
    m_dirty = false;
    m_note->setText(QFile::exists(pathOf(name))
                        ? tr("%1 in %2. Comments and untouched lines are kept as they are.").arg(name, QDir::toNativeSeparators(m_dir))
                        : tr("%1 does not exist yet; it is created when you save.").arg(name));
}

bool EnvDialog::save()
{
    QList<Entry> now;
    static const QRegularExpression valid(QStringLiteral(R"(^[A-Za-z_][A-Za-z0-9_.]*$)"));
    for (int r = 0; r < m_table->rowCount(); ++r) {
        const QString k = m_table->item(r, 0)->text().trimmed();
        const QString v = m_table->item(r, 1)->text();
        if (k.isEmpty() && v.isEmpty())
            continue;
        if (!valid.match(k).hasMatch()) {
            QMessageBox::warning(this, tr("Environment Variables"),
                                 tr("\"%1\" is not a valid variable name (letters, digits and underscores; it cannot start with a digit).").arg(k));
            m_table->setCurrentCell(r, 0);
            return false;
        }
        now.append({k, v});
    }
    QHash<QString, QString> values;
    for (const Entry &e : std::as_const(now))
        values.insert(e.key, e.value);
    QHash<QString, QString> before;
    for (const Entry &e : std::as_const(m_original))
        before.insert(e.key, e.value);

    QStringList out;
    QSet<QString> written;
    for (const QString &raw : std::as_const(m_rawLines)) {
        const auto m = entryPattern().match(raw.endsWith(QLatin1Char('\r')) ? raw.chopped(1) : raw);
        if (raw.trimmed().startsWith(QLatin1Char('#')) || !m.hasMatch()) {
            out << raw; // comments, blank lines, anything we do not understand
            continue;
        }
        const QString key = m.captured(2);
        if (!values.contains(key) || written.contains(key))
            continue; // removed (or a duplicate of a key already written)
        written.insert(key);
        if (values.value(key) == before.value(key))
            out << raw; // untouched
        else
            out << m.captured(1) + key + QLatin1Char('=') + formatValue(values.value(key));
    }
    for (const Entry &e : std::as_const(now))
        if (!written.contains(e.key)) {
            written.insert(e.key);
            out << e.key + QLatin1Char('=') + formatValue(e.value);
        }

    QSaveFile f(pathOf(m_loaded));
    if (!f.open(QIODevice::WriteOnly) || f.write((out.join(QLatin1Char('\n')) + (out.isEmpty() ? QString() : QStringLiteral("\n"))).toUtf8()) < 0 ||
        !f.commit()) {
        QMessageBox::warning(this, tr("Environment Variables"), tr("Could not write %1: %2").arg(m_loaded, f.errorString()));
        return false;
    }
    m_dirty = false;
    emit saved(m_restart->isVisible() && m_restart->isChecked());
    return true;
}
