#include "NewFileDialog.h"

#include "filesystem/FileManager.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

NewFileDialog::NewFileDialog(Kind kind, const QString &directory, const QString &initialName, QWidget *parent)
    : QDialog(parent)
    , m_directory(directory)
{
    QString title, okText;
    switch (kind) {
    case Kind::File:   title = tr("Create File");   okText = tr("Create"); break;
    case Kind::Folder: title = tr("Create Folder"); okText = tr("Create"); break;
    case Kind::Rename: title = tr("Rename");        okText = tr("Rename"); break;
    }
    setWindowTitle(title);
    setMinimumWidth(380);

    m_name = new QLineEdit(initialName, this);
    if (kind == Kind::File)
        m_name->setPlaceholderText(tr("main.cpp"));
    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);

    auto *form = new QFormLayout;
    form->addRow(tr("Name:"), m_name);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_ok = buttons->addButton(okText, QDialogButtonBox::AcceptRole);
    m_ok->setDefault(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("In: %1").arg(FileManager::displayPath(directory)), this));
    layout->addLayout(form);
    layout->addWidget(m_hint);
    layout->addWidget(buttons);

    connect(m_name, &QLineEdit::textChanged, this, &NewFileDialog::validate);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (kind == Kind::Rename) {
        // Select the base name, leaving the extension untouched.
        const int dot = initialName.lastIndexOf(QLatin1Char('.'));
        m_name->setSelection(0, dot > 0 ? dot : initialName.size());
    }
    validate();
}

QString NewFileDialog::name() const { return m_name->text().trimmed(); }

void NewFileDialog::validate()
{
    QString err;
    if (!name().isEmpty()) {
        err = FileManager::validateName(name());
        if (err.isEmpty() && QFileInfo::exists(QDir(m_directory).filePath(name())))
            err = tr("\"%1\" already exists.").arg(name());
    }
    m_hint->setText(err);
    m_ok->setEnabled(!name().isEmpty() && err.isEmpty());
}
