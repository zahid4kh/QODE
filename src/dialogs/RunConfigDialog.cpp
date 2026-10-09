#include "RunConfigDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

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
    m_preview->setTextFormat(Qt::RichText); // built in updatePreview from escaped text
    m_preview->setWordWrap(true);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_intro = new QLabel(this);
    m_intro->setWordWrap(true);
    m_wide = new QCheckBox(tr("Run the whole project with this command, whichever file is open"), this);
    connect(m_wide, &QCheckBox::toggled, this, &RunConfigDialog::updateIntro);
    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->hide();
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_chipStyle = QStringLiteral("font-family:'JetBrains Mono'; color:%1; background-color:%2;").arg(t.accent.name(), t.editorBg.name());
    m_quoteColor = t.accent.name();
    m_mutedColor = t.textMuted.name();
    auto chip = [this](const QString &name) { return QStringLiteral("<span style=\"%1\">&nbsp;%2&nbsp;</span>").arg(m_chipStyle, name); };
    auto *vars = new QLabel(tr("Variables: %1 full path · %2 its folder · %3 file name without extension · %4 project root.<br>"
                               "Paths are quoted for you.")
                                .arg(chip(QStringLiteral("{file}")), chip(QStringLiteral("{dir}")), chip(QStringLiteral("{name}")), chip(QStringLiteral("{project}"))),
                            this);
    vars->setTextFormat(Qt::RichText);
    vars->setObjectName(QStringLiteral("emptyText"));
    vars->setWordWrap(true);
    m_command->setFont(QFont(QStringLiteral("JetBrains Mono"), font().pointSize()));
    m_preview->setStyleSheet(QStringLiteral("QLabel { background: %1; border: 1px solid %2; border-radius: 6px; padding: 8px 10px; color: %3; }")
                                 .arg(t.editorBg.name(), t.border.name(), t.editorFg.name()));
    m_wide->setStyleSheet(QStringLiteral("QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid %1; border-radius: 3px; background: %2; }"
                                         "QCheckBox::indicator:checked { background: %3; border-color: %3; }")
                              .arg(t.textMuted.name(), t.editorBg.name(), t.accent.name()));
    setContentsMargins(6, 6, 6, 6);

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
    layout->setSpacing(10);

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
    if (cmd.isEmpty()) {
        m_preview->clear();
        m_preview->hide();
        return;
    }
    // Quoted paths (inserted by expand) get the accent colour so they stand out from the command itself.
    const QString plain = expand(cmd, m_file, m_root);
    QString html;
    bool quoted = false;
    for (const QChar ch : plain) {
        if (ch == QLatin1Char('\'')) {
            html += quoted ? QStringLiteral("'</span>") : QStringLiteral("<span style=\"color:%1;\">'").arg(m_quoteColor);
            quoted = !quoted;
        } else if (ch == QLatin1Char('<')) {
            html += QStringLiteral("&lt;");
        } else if (ch == QLatin1Char('>')) {
            html += QStringLiteral("&gt;");
        } else if (ch == QLatin1Char('&')) {
            html += QStringLiteral("&amp;");
        } else {
            html += ch;
        }
    }
    if (quoted)
        html += QStringLiteral("</span>");
    m_preview->setText(tr("<span style=\"color:%1;\">Will run</span><br><span style=\"font-family:'JetBrains Mono';\">%2</span>").arg(m_mutedColor, html));
    m_preview->show();
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
