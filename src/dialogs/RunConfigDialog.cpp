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

QList<QPair<QString, QString>> RunConfigDialog::presets(const QString &filePath)
{
    using P = QPair<QString, QString>;
    const QString key = keyFor(filePath);
    auto one = [](const char *name, const char *cmd) { return QList<P>{P(QString::fromLatin1(name), QString::fromLatin1(cmd))}; };
    // C and C++ binaries are written next to the source file.
    if (key == QLatin1String("c"))
        return {P(QStringLiteral("Compile with gcc and run"), QStringLiteral("gcc {file} -o {dir}/{name} && {dir}/{name}")),
                P(QStringLiteral("gcc with warnings (-Wall -Wextra)"), QStringLiteral("gcc -Wall -Wextra {file} -o {dir}/{name} && {dir}/{name}")),
                P(QStringLiteral("gcc, link math library (-lm)"), QStringLiteral("gcc {file} -o {dir}/{name} -lm && {dir}/{name}")),
                P(QStringLiteral("Compile with clang and run"), QStringLiteral("clang {file} -o {dir}/{name} && {dir}/{name}"))};
    if (key == QLatin1String("cpp") || key == QLatin1String("cc") || key == QLatin1String("cxx"))
        return {P(QStringLiteral("Compile with g++ (C++17) and run"), QStringLiteral("g++ -std=c++17 {file} -o {dir}/{name} && {dir}/{name}")),
                P(QStringLiteral("g++ C++20"), QStringLiteral("g++ -std=c++20 {file} -o {dir}/{name} && {dir}/{name}")),
                P(QStringLiteral("g++ C++23"), QStringLiteral("g++ -std=c++23 {file} -o {dir}/{name} && {dir}/{name}")),
                P(QStringLiteral("g++ with warnings (-Wall -Wextra)"), QStringLiteral("g++ -std=c++17 -Wall -Wextra {file} -o {dir}/{name} && {dir}/{name}")),
                P(QStringLiteral("Compile with clang++ and run"), QStringLiteral("clang++ -std=c++17 {file} -o {dir}/{name} && {dir}/{name}"))};
    static const QHash<QString, QList<P>> map = {
        {QStringLiteral("py"), one("Run with python3", "python3 {file}")},
        {QStringLiteral("js"), one("Run with node", "node {file}")},
        {QStringLiteral("mjs"), one("Run with node", "node {file}")},
        {QStringLiteral("ts"), one("Run with tsx", "npx tsx {file}")},
        {QStringLiteral("sh"), one("Run with bash", "bash {file}")},
        {QStringLiteral("rb"), one("Run with ruby", "ruby {file}")},
        {QStringLiteral("php"), one("Run with php", "php {file}")},
        {QStringLiteral("pl"), one("Run with perl", "perl {file}")},
        {QStringLiteral("lua"), one("Run with lua", "lua {file}")},
        {QStringLiteral("go"), one("go run", "go run {file}")},
        {QStringLiteral("rs"), one("cargo run", "cargo run")},
        {QStringLiteral("java"), one("Run with java", "java {file}")},
    };
    return map.value(key);
}

QString RunConfigDialog::suggestion(const QString &filePath)
{
    const auto list = presets(filePath);
    return list.isEmpty() ? QString() : list.first().second;
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
    m_command->setPlaceholderText(tr("command to run this file"));
    m_command->setClearButtonEnabled(true);
    m_preview = new QLabel(this);
    m_preview->setObjectName(QStringLiteral("emptyText"));
    m_preview->setTextFormat(Qt::PlainText); // the command holds & and quotes
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
    form->addRow(tr("Preset:"), m_picker);
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
    setSuggestions({});
    if (m_command->text().trimmed().isEmpty() && !m_configs.isEmpty())
        m_command->setText(m_configs.first().second); // start from the first preset instead of an empty box
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
    const QString cmd = command();
    m_preview->setText(cmd.isEmpty() ? QString() : tr("Will run: %1").arg(expand(cmd, m_file, m_root)));
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
    // The project's detected configurations come first, then the presets for this file type.
    m_configs = configs;
    for (const auto &p : presets(m_file)) {
        bool known = false;
        for (const auto &c : m_configs)
            known = known || c.second == p.second;
        if (!known)
            m_configs << p;
    }
    m_picker->clear();
    m_picker->addItem(tr("Custom command"));
    for (const auto &c : m_configs)
        m_picker->addItem(c.first);
    m_form->setRowVisible(m_picker, !m_configs.isEmpty());
    int idx = 0;
    for (int i = 0; i < m_configs.size(); ++i)
        if (m_configs[i].second == command())
            idx = i + 1;
    m_picker->setCurrentIndex(idx);
}
