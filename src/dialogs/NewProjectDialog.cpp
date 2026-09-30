#include "NewProjectDialog.h"

#include "filesystem/FileManager.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

NewProjectDialog::NewProjectDialog(const QString &defaultLocation, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New Project"));
    setMinimumWidth(460);

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("MyProject"));
    m_location = new QLineEdit(defaultLocation.isEmpty() ? QDir::homePath() : defaultLocation, this);
    auto *browseBtn = new QPushButton(tr("Browse…"), this);
    browseBtn->setAutoDefault(false);

    auto *locRow = new QHBoxLayout;
    locRow->addWidget(m_location, 1);
    locRow->addWidget(browseBtn);

    auto *form = new QFormLayout;
    form->addRow(tr("Project Name:"), m_name);
    form->addRow(tr("Location:"), locRow);

    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_create = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    m_create->setDefault(true);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_hint);
    layout->addWidget(buttons);

    connect(browseBtn, &QPushButton::clicked, this, &NewProjectDialog::browse);
    connect(m_name, &QLineEdit::textChanged, this, &NewProjectDialog::validate);
    connect(m_location, &QLineEdit::textChanged, this, &NewProjectDialog::validate);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    validate();
}

QString NewProjectDialog::projectName() const { return m_name->text().trimmed(); }
QString NewProjectDialog::location() const { return m_location->text().trimmed(); }

void NewProjectDialog::browse()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Project Location"), location());
    if (!dir.isEmpty())
        m_location->setText(dir);
}

void NewProjectDialog::validate()
{
    QString msg;
    if (!projectName().isEmpty())
        msg = FileManager::validateName(projectName());
    const bool nameOk = !projectName().isEmpty() && msg.isEmpty();
    if (nameOk && !location().isEmpty()) {
        const QString full = QDir(location()).filePath(projectName());
        m_hint->setText(tr("Will be created at: %1").arg(FileManager::displayPath(full)));
    } else {
        m_hint->setText(msg);
    }
    m_create->setEnabled(nameOk && !location().isEmpty());
}
