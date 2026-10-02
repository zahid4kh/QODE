#include "CompilerFlagsDialog.h"

#include "settings/SettingsManager.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QVBoxLayout>

QString CompilerFlagsDialog::templateText()
{
    return QStringLiteral(
        "# Compiler flags for the language server, one per line. Lines starting with # are ignored.\n"
        "# {project} is replaced with the project folder.\n"
        "#\n"
        "# -std=c++20\n"
        "# -I{project}/include\n"
        "# -DMY_DEFINE=1\n");
}

CompilerFlagsDialog::CompilerFlagsDialog(const QString &text, const QString &detectedSource, const QStringList &detected, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Project Compiler Flags"));
    resize(560, 380);
    auto *layout = new QVBoxLayout(this);
    auto *info = new QLabel(tr("The language server (clangd) guesses how your code is compiled. Flags listed here are used for "
                               "this project when it has no <b>compile_commands.json</b> or <b>compile_flags.txt</b> "
                               "(CMake and Meson create the former for you)."),
                            this);
    info->setWordWrap(true);
    layout->addWidget(info);

    if (!detected.isEmpty()) {
        auto *found = new QLabel(tr("Detected from <b>%1</b> (%2 flags, always used):").arg(detectedSource.toHtmlEscaped()).arg(detected.size()), this);
        layout->addWidget(found);
        auto *list = new QPlainTextEdit(this);
        list->setReadOnly(true);
        list->setFont(SettingsManager::instance().editorFont());
        list->setLineWrapMode(QPlainTextEdit::NoWrap);
        list->setPlainText(detected.join(QLatin1Char('\n')));
        list->setMaximumHeight(110);
        layout->addWidget(list);
        layout->addWidget(new QLabel(tr("Your own flags, added after the detected ones:"), this));
    }

    m_edit = new QPlainTextEdit(this);
    m_edit->setFont(SettingsManager::instance().editorFont());
    m_edit->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_edit->setPlainText(text.trimmed().isEmpty() ? templateText() : text);
    layout->addWidget(m_edit, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    if (detected.isEmpty()) {
        auto *qt = buttons->addButton(tr("Add Qt 6 Flags"), QDialogButtonBox::ActionRole);
        qt->setToolTip(tr("Appends the include paths reported by: pkg-config --cflags Qt6Widgets"));
        connect(qt, &QPushButton::clicked, this, &CompilerFlagsDialog::addQtFlags);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QString CompilerFlagsDialog::text() const
{
    return m_edit->toPlainText();
}

void CompilerFlagsDialog::addQtFlags()
{
    QProcess p;
    p.start(QStringLiteral("pkg-config"), {QStringLiteral("--cflags"), QStringLiteral("Qt6Widgets")});
    if (!p.waitForFinished(3000) || p.exitCode() != 0) {
        QMessageBox::information(this, tr("Qt flags"),
                                 tr("Could not run \"pkg-config --cflags Qt6Widgets\". Install the Qt 6 development packages "
                                    "(and pkg-config), or add the -I lines by hand."));
        return;
    }
    const QStringList flags = QProcess::splitCommand(QString::fromUtf8(p.readAllStandardOutput()).trimmed());
    QString block = QStringLiteral("\n# Qt 6\n");
    for (const QString &f : flags)
        block += f + QLatin1Char('\n');
    QString current = m_edit->toPlainText();
    if (!current.endsWith(QLatin1Char('\n')))
        current += QLatin1Char('\n');
    m_edit->setPlainText(current + block);
}
