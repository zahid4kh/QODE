#include "spring/SpringConfigDialog.h"

#include <QDialogButtonBox>
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
    m_port = new QSpinBox(this);
    m_port->setRange(0, 65535);
    m_port->setSpecialValueText(tr("Application default"));
    m_port->setValue(o.port);
    form->addRow(tr("Server port"), m_port);
    m_arguments = new QLineEdit(o.arguments, this);
    m_arguments->setPlaceholderText(tr("e.g. --debug --logging.level.root=DEBUG"));
    form->addRow(tr("Arguments"), m_arguments);
    m_command = new QLineEdit(o.command, this);
    m_command->setPlaceholderText(tr("Optional: replaces the generated command"));
    form->addRow(tr("Custom command"), m_command);
    layout->addLayout(form);

    m_preview = new QLabel(this);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setWordWrap(true);
    layout->addWidget(m_preview);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    for (QLineEdit *e : {m_profiles, m_arguments, m_command})
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
    return o;
}

void SpringConfigDialog::updatePreview()
{
    m_preview->setText(tr("Runs in <b>%1</b>:<br><tt>%2</tt>").arg(m_project.workDir.toHtmlEscaped(), m_project.command(options()).toHtmlEscaped()));
}
