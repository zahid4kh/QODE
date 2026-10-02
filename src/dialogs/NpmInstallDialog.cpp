#include "NpmInstallDialog.h"

#include "lsp/LspServers.h"
#include "lsp/NpmInstaller.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

NpmInstallDialog::NpmInstallDialog(QWidget *parent) : QDialog(parent), m_installer(new NpmInstaller(this))
{
    setWindowTitle(tr("Set Up Web Language Servers"));
    setMinimumWidth(560);
    auto *layout = new QVBoxLayout(this);

    auto *info = new QLabel(
        tr("QODE will set up the language servers for <b>TypeScript / JavaScript, HTML, CSS (SCSS, Less) and JSON</b> "
           "with npm:"
           "<ul>"
           "<li>Runs <b>%1</b></li>"
           "<li>Everything goes into <b>%2</b> (about 130 MB); nothing is installed globally and no administrator rights are needed</li>"
           "<li>Needs <b>Node.js</b> with npm on this computer</li>"
           "<li>All four servers are installed together; removing them later deletes that one folder</li>"
           "</ul>")
            .arg(NpmInstaller::commandLine().toHtmlEscaped(), NpmInstaller::installRoot().toHtmlEscaped()),
        this);
    info->setWordWrap(true);
    info->setTextFormat(Qt::RichText);
    layout->addWidget(info);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_status->hide();
    layout->addWidget(m_status);
    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 0);
    m_bar->setTextVisible(false);
    m_bar->hide();
    layout->addWidget(m_bar);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMinimumHeight(140);
    m_log->setMaximumBlockCount(500);
    m_log->hide();
    layout->addWidget(m_log);

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    m_start = new QPushButton(tr("Download and Set Up"), this);
    m_start->setDefault(true);
    m_cancel = new QPushButton(tr("Cancel"), this);
    row->addWidget(m_start);
    row->addWidget(m_cancel);
    layout->addLayout(row);

    connect(m_start, &QPushButton::clicked, this, &NpmInstallDialog::begin);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_installer->isRunning())
            m_installer->cancel();
        else
            reject();
    });
    connect(m_installer, &NpmInstaller::output, this, [this](const QString &text) {
        const QString t = text.trimmed();
        if (!t.isEmpty())
            m_log->appendPlainText(t);
    });
    connect(m_installer, &NpmInstaller::finished, this,
            [this] { finish(true, tr("Done. The TypeScript / JavaScript, HTML, CSS and JSON language servers are installed.")); });
    connect(m_installer, &NpmInstaller::failed, this, [this](const QString &error) { finish(false, error); });
}

void NpmInstallDialog::reject()
{
    if (m_installer->isRunning())
        m_installer->cancel();
    QDialog::reject();
}

void NpmInstallDialog::begin()
{
    m_start->setEnabled(false);
    m_status->setText(tr("Installing with npm… this can take a minute."));
    m_status->show();
    m_bar->show();
    m_log->clear();
    m_log->show();
    m_installer->start();
}

void NpmInstallDialog::finish(bool ok, const QString &message)
{
    m_status->setText(message);
    m_status->show();
    m_bar->hide();
    if (ok) {
        m_start->hide();
        m_cancel->setText(tr("Close"));
        disconnect(m_cancel, nullptr, this, nullptr);
        connect(m_cancel, &QPushButton::clicked, this, &QDialog::accept);
        m_cancel->setDefault(true);
    } else {
        m_start->setText(tr("Try Again"));
        m_start->setEnabled(true);
        m_cancel->setText(tr("Close"));
    }
    adjustSize();
}
