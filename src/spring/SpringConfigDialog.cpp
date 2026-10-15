#include "spring/SpringConfigDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

SpringConfigDialog::SpringConfigDialog(const SpringProject &project, const SpringProject::Options &o, QWidget *parent)
    : QDialog(parent), m_project(project)
{
    setWindowTitle(tr("Spring Boot"));
    setMinimumWidth(520);
    auto *layout = new QVBoxLayout(this);
    auto *info = new QLabel(tr("<b>%1</b> — %2. Leave a field empty to use what the application configures itself.")
                                .arg(project.appName.toHtmlEscaped(), project.maven ? tr("Maven") : tr("Gradle")),
                            this);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto *form = new QFormLayout;
    m_profiles = new QLineEdit(o.profiles, this);
    m_profiles->setPlaceholderText(tr("e.g. dev,local (spring.profiles.active)"));
    form->addRow(tr("Active profiles"), m_profiles);
    const QStringList known = project.profiles();
    if (!known.isEmpty()) {
        auto *row = new QHBoxLayout;
        for (const QString &p : known) {
            auto *cb = new QCheckBox(p, this);
            cb->setToolTip(tr("application-%1 found in the resources").arg(p));
            m_profileChecks << cb;
            row->addWidget(cb);
            connect(cb, &QCheckBox::toggled, this, &SpringConfigDialog::profilesFromChecks);
        }
        row->addStretch(1);
        form->addRow(QString(), row);
    }
    m_port = new QSpinBox(this);
    m_port->setRange(0, 65535);
    m_port->setSpecialValueText(tr("Application default"));
    m_port->setValue(o.port);
    form->addRow(tr("Server port"), m_port);
    m_arguments = new QLineEdit(o.arguments, this);
    m_arguments->setPlaceholderText(tr("e.g. --debug --logging.level.root=DEBUG"));
    form->addRow(tr("Arguments"), m_arguments);
    m_jvm = new QLineEdit(o.jvmArgs, this);
    m_jvm->setPlaceholderText(tr("e.g. -Xmx512m -Dspring.devtools.restart.enabled=false"));
    form->addRow(tr("JVM options"), m_jvm);
    m_env = new QPlainTextEdit(o.env, this);
    m_env->setPlaceholderText(tr("One per line: DB_URL=jdbc:postgresql://localhost/app"));
    m_env->setFixedHeight(80);
    form->addRow(tr("Environment"), m_env);
    m_command = new QLineEdit(o.command, this);
    m_command->setPlaceholderText(tr("Optional: replaces the generated command"));
    form->addRow(tr("Custom command"), m_command);
    layout->addLayout(form);

    m_preview = new QLabel(this);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setWordWrap(true);
    layout->addWidget(m_preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    // Quick access to the configuration files themselves.
    const QStringList files = QDir(project.resourcesDir()).entryList({QStringLiteral("application*.properties"), QStringLiteral("application*.yml"), QStringLiteral("application*.yaml")}, QDir::Files, QDir::Name);
    if (!files.isEmpty()) {
        auto *open = new QPushButton(tr("Open Configuration File…"), this);
        auto *menu = new QMenu(open);
        for (const QString &f : files) {
            const QString path = project.resourcesDir() + QLatin1Char('/') + f;
            menu->addAction(f, this, [this, path] { emit openFileRequested(path); });
        }
        open->setMenu(menu);
        buttons->addButton(open, QDialogButtonBox::ActionRole);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    connect(m_profiles, &QLineEdit::textChanged, this, &SpringConfigDialog::checksFromProfiles);
    connect(m_env, &QPlainTextEdit::textChanged, this, &SpringConfigDialog::updatePreview);
    checksFromProfiles();
    for (QLineEdit *e : {m_profiles, m_arguments, m_command, m_jvm})
        connect(e, &QLineEdit::textChanged, this, &SpringConfigDialog::updatePreview);
    connect(m_port, &QSpinBox::valueChanged, this, &SpringConfigDialog::updatePreview);
    updatePreview();
}

SpringProject::Options SpringConfigDialog::options() const
{
    SpringProject::Options o;
    o.profiles = m_profiles->text().trimmed();
    o.port = m_port->value();
    o.arguments = m_arguments->text().trimmed();
    o.command = m_command->text().trimmed();
    o.jvmArgs = m_jvm->text().trimmed();
    o.env = m_env->toPlainText().trimmed();
    return o;
}

void SpringConfigDialog::profilesFromChecks()
{
    if (m_syncing)
        return;
    QStringList on;
    for (QCheckBox *cb : std::as_const(m_profileChecks))
        if (cb->isChecked())
            on << cb->text();
    // Profiles typed by hand that have no file stay.
    QStringList keep;
    for (const QString &p : m_profiles->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        bool isKnown = false;
        for (QCheckBox *cb : std::as_const(m_profileChecks))
            isKnown = isKnown || cb->text() == p.trimmed();
        if (!isKnown)
            keep << p.trimmed();
    }
    m_syncing = true;
    m_profiles->setText((on + keep).join(QLatin1Char(',')));
    m_syncing = false;
}

void SpringConfigDialog::checksFromProfiles()
{
    if (m_syncing)
        return;
    QStringList typed;
    for (const QString &p : m_profiles->text().split(QLatin1Char(','), Qt::SkipEmptyParts))
        typed << p.trimmed();
    m_syncing = true;
    for (QCheckBox *cb : std::as_const(m_profileChecks))
        cb->setChecked(typed.contains(cb->text()));
    m_syncing = false;
}

void SpringConfigDialog::updatePreview()
{
    m_preview->setText(tr("Runs in <b>%1</b>:<br><tt>%2</tt>").arg(m_project.workDir.toHtmlEscaped(), m_project.command(options()).toHtmlEscaped()));
}
