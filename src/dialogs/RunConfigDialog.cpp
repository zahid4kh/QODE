#include "RunConfigDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
QString shellQuote(const QString &s)
{
    QString q = s;
    q.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + q + QLatin1Char('\'');
}
} // namespace

QString RunConfigDialog::keyFor(const QString &filePath)
{
    const QFileInfo fi(filePath);
    const QString suffix = fi.suffix().toLower();
    return suffix.isEmpty() ? fi.fileName() : suffix;
}

QString RunConfigDialog::suggestion(const QString &filePath)
{
    static const QHash<QString, QString> map = {
        {QStringLiteral("py"), QStringLiteral("python3 {file}")},
        {QStringLiteral("js"), QStringLiteral("node {file}")},
        {QStringLiteral("mjs"), QStringLiteral("node {file}")},
        {QStringLiteral("ts"), QStringLiteral("npx tsx {file}")},
        {QStringLiteral("sh"), QStringLiteral("bash {file}")},
        {QStringLiteral("rb"), QStringLiteral("ruby {file}")},
        {QStringLiteral("php"), QStringLiteral("php {file}")},
        {QStringLiteral("pl"), QStringLiteral("perl {file}")},
        {QStringLiteral("lua"), QStringLiteral("lua {file}")},
        {QStringLiteral("go"), QStringLiteral("go run {file}")},
        {QStringLiteral("rs"), QStringLiteral("cargo run")},
        {QStringLiteral("java"), QStringLiteral("java {file}")},
        {QStringLiteral("c"), QStringLiteral("gcc {file} -o /tmp/{name} && /tmp/{name}")},
        {QStringLiteral("cpp"), QStringLiteral("g++ -std=c++17 {file} -o /tmp/{name} && /tmp/{name}")},
        {QStringLiteral("cc"), QStringLiteral("g++ -std=c++17 {file} -o /tmp/{name} && /tmp/{name}")},
    };
    return map.value(keyFor(filePath));
}

QString RunConfigDialog::expand(const QString &command, const QString &filePath, const QString &projectRoot)
{
    const QFileInfo fi(filePath);
    QString out = command;
    out.replace(QStringLiteral("{file}"), shellQuote(filePath));
    out.replace(QStringLiteral("{dir}"), shellQuote(fi.absolutePath()));
    out.replace(QStringLiteral("{name}"), shellQuote(fi.completeBaseName()));
    out.replace(QStringLiteral("{project}"), shellQuote(projectRoot));
    return out;
}

RunConfigDialog::RunConfigDialog(const QString &filePath, const QString &command, QWidget *parent)
    : QDialog(parent)
    , m_file(filePath)
{
    setWindowTitle(tr("Run Configuration"));
    setMinimumWidth(520);

    const QString key = keyFor(filePath);
    m_command = new QLineEdit(command, this);
    m_command->setPlaceholderText(suggestion(filePath).isEmpty() ? tr("command to run this file") : suggestion(filePath));
    m_command->setClearButtonEnabled(true);
    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("emptyText"));
    m_preview->setWordWrap(true);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_intro = new QLabel(this);
    m_intro->setWordWrap(true);
    m_wide = new QCheckBox(tr("Run the whole project with this command, whichever file is open"), this);
    connect(m_wide, &QCheckBox::toggled, this, &RunConfigDialog::updateIntro);
    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->hide();
    auto *vars = new QLabel(tr("Variables: <b>{file}</b> full path · <b>{dir}</b> its folder · <b>{name}</b> file name without "
                               "extension · <b>{project}</b> project root. Paths are quoted for you."), this);
    vars->setObjectName(QStringLiteral("emptyText"));
    vars->setWordWrap(true);

    m_picker = new QComboBox(this);
    auto *form = m_form = new QFormLayout;
    form->addRow(tr("Detected:"), m_picker);
    form->setRowVisible(m_picker, false);
    form->addRow(tr("Command:"), m_command);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    QPushButton *ok = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    ok->setDefault(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_intro);
    layout->addWidget(m_wide);
    layout->addWidget(m_note);
    layout->addLayout(form);
    layout->addWidget(vars);
    layout->addWidget(m_preview);
    layout->addWidget(buttons);

    connect(m_command, &QLineEdit::textChanged, this, &RunConfigDialog::updatePreview);
    connect(m_command, &QLineEdit::textChanged, this, [this](const QString &text) {
        int idx = 0; // 0 = custom
        for (int i = 0; i < m_configs.size(); ++i)
            if (m_configs[i].second == text.trimmed())
                idx = i + 1;
        QSignalBlocker block(m_picker);
        m_picker->setCurrentIndex(idx);
    });
    connect(m_picker, &QComboBox::activated, this, [this](int i) {
        if (i > 0 && i <= m_configs.size())
            m_command->setText(m_configs[i - 1].second);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    updateIntro();
    updatePreview();
}

void RunConfigDialog::updateIntro()
{
    m_intro->setText(m_wide->isChecked()
                         ? tr("Command run in the terminal, from the project folder, when you press Run in this project.")
                         : tr("Command run in the terminal when you press Run for <b>%1</b> files.").arg(keyFor(m_file).toHtmlEscaped()));
}

void RunConfigDialog::setProjectWide(bool checked)
{
    m_wide->setChecked(checked);
}

bool RunConfigDialog::projectWide() const
{
    return m_wide->isChecked();
}

QString RunConfigDialog::command() const
{
    return m_command->text().trimmed();
}

void RunConfigDialog::updatePreview()
{
    const QString cmd = command().isEmpty() ? suggestion(m_file) : command();
    m_preview->setText(cmd.isEmpty() ? QString() : tr("Will run: %1").arg(expand(cmd, m_file, m_root).toHtmlEscaped()));
}

void RunConfigDialog::setNote(const QString &html)
{
    m_note->setText(html);
    m_note->setVisible(!html.isEmpty());
}

void RunConfigDialog::setProjectRoot(const QString &root)
{
    m_root = root;
    updatePreview();
}

void RunConfigDialog::setSuggestions(const QList<QPair<QString, QString>> &configs)
{
    m_configs = configs;
    m_picker->clear();
    m_picker->addItem(tr("Custom command"));
    for (const auto &c : configs)
        m_picker->addItem(c.first);
    m_form->setRowVisible(m_picker, !configs.isEmpty());
    int idx = 0;
    for (int i = 0; i < configs.size(); ++i)
        if (configs[i].second == command())
            idx = i + 1;
    m_picker->setCurrentIndex(idx);
}
