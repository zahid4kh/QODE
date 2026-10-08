#include "PipInstallDialog.h"

#include "lsp/LspServers.h"
#include "lsp/PipInstaller.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

PipInstallDialog::PipInstallDialog(QWidget *parent) : QDialog(parent), m_installer(new PipInstaller(this))
{
    setWindowTitle(tr("Set Up Python Language Tools"));
    setMinimumWidth(580);
    auto *layout = new QVBoxLayout(this);

    const bool uv = PipInstaller::usesUv();
    QString commands;
    for (const QString &c : PipInstaller::plannedCommands())
        commands += QStringLiteral("<li><b>%1</b></li>").arg(c.toHtmlEscaped());

    auto *info = new QLabel(
        tr("QODE will set up the language tools for <b>Python</b>:"
           "<ul>"
           "<li><b>basedpyright</b>: syntax and type errors, undefined names, hover, go to definition, completion, rename and references</li>"
           "<li><b>Ruff</b>: lint problems as you type, quick fixes and fast formatting</li>"
           "</ul>"
           "How it works:"
           "<ul>"
           "%1"
           "<li>Everything goes into <b>%2</b> (about 300 MB); nothing is installed globally and no administrator rights are needed</li>"
           "<li>Uses <b>%3</b>%4</li>"
           "<li>Needs no Node.js: basedpyright brings its own. Removing the tools later deletes that one folder</li>"
           "</ul>")
            .arg(commands, PipInstaller::installRoot().toHtmlEscaped(), uv ? tr("uv") : tr("python3 and pip"),
                 uv ? tr(" (found on this computer; if it fails QODE falls back to python3 and pip)")
                    : tr(" (the Python 3 on this computer, with its venv module)")),
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

    connect(m_start, &QPushButton::clicked, this, &PipInstallDialog::begin);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_installer->isRunning())
            m_installer->cancel();
        else
            reject();
    });
    connect(m_installer, &PipInstaller::output, this, [this](const QString &text) {
        const QString t = text.trimmed();
        if (!t.isEmpty())
            m_log->appendPlainText(t);
    });
    connect(m_installer, &PipInstaller::finished, this,
            [this] { finish(true, tr("Done. The Python language server (basedpyright) and Ruff are installed.")); });
    connect(m_installer, &PipInstaller::failed, this, [this](const QString &error) { finish(false, error); });
}

void PipInstallDialog::reject()
{
    if (m_installer->isRunning())
        m_installer->cancel();
    QDialog::reject();
}

void PipInstallDialog::begin()
{
    m_start->setEnabled(false);
    m_status->setText(tr("Installing… this can take a minute."));
    m_status->show();
    m_bar->show();
    m_log->clear();
    m_log->show();
    m_installer->start();
}

void PipInstallDialog::finish(bool ok, const QString &message)
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
