#include "LspInstallDialog.h"

#include "lsp/LspInstaller.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

LspInstallDialog::LspInstallDialog(QWidget *parent) : QDialog(parent), m_installer(new LspInstaller(this))
{
    setWindowTitle(tr("Set Up Kotlin Language Server"));
    setMinimumWidth(520);
    auto *layout = new QVBoxLayout(this);

    auto *info = new QLabel(
        tr("QODE will download JetBrains' <b>Kotlin language server</b> (Alpha) and set it up for you:"
           "<ul>"
           "<li>Download about 370 MB from <i>download.jetbrains.com</i>, checked against its published SHA-256 checksum</li>"
           "<li>Unpack it into <b>%1</b> (about 1.2 GB on disk)</li>"
           "<li>No Java needed: the server brings its own runtime</li>"
           "<li>Nothing outside your home folder is changed and no administrator rights are needed</li>"
           "</ul>")
            .arg(LspInstaller::installRoot().toHtmlEscaped()),
        this);
    info->setWordWrap(true);
    info->setTextFormat(Qt::RichText);
    layout->addWidget(info);

    m_link = new QCheckBox(tr("Also link it as %1").arg(LspInstaller::linkPath()), this);
    m_link->setChecked(true);
    m_link->setToolTip(tr("Lets you run \"kotlin-lsp\" in a terminal. QODE finds the server without it."));
    layout->addWidget(m_link);
    m_latest = new QCheckBox(tr("Use the latest release instead of the tested version (%1)").arg(LspInstaller::pinnedVersion()), this);
    layout->addWidget(m_latest);

    m_stage = new QLabel(this);
    m_stage->setWordWrap(true);
    m_stage->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_stage->hide();
    layout->addWidget(m_stage);
    m_bar = new QProgressBar(this);
    m_bar->hide();
    layout->addWidget(m_bar);

    auto *row = new QHBoxLayout;
    row->addStretch(1);
    m_start = new QPushButton(tr("Download and Set Up"), this);
    m_start->setDefault(true);
    m_cancel = new QPushButton(tr("Cancel"), this);
    row->addWidget(m_start);
    row->addWidget(m_cancel);
    layout->addLayout(row);

    connect(m_start, &QPushButton::clicked, this, &LspInstallDialog::begin);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_installer->isRunning())
            m_installer->cancel();
        else
            reject();
    });
    connect(m_installer, &LspInstaller::stageChanged, m_stage, &QLabel::setText);
    connect(m_installer, &LspInstaller::progress, this, [this](qint64 done, qint64 total) {
        if (done < 0) {
            m_bar->setRange(0, 1);
            m_bar->setValue(0);
        } else if (total <= 0) {
            m_bar->setRange(0, 0); // busy
        } else {
            m_bar->setRange(0, 1000);
            m_bar->setValue(int(done * 1000 / total));
            m_bar->setFormat(tr("%1 of %2").arg(QLocale().formattedDataSize(done), QLocale().formattedDataSize(total)));
        }
    });
    connect(m_installer, &LspInstaller::finished, this, [this](const QString &exe, const QString &note) {
        m_executable = exe;
        finish(true, tr("Done. The Kotlin language server is installed.") + (note.isEmpty() ? QString() : QLatin1Char('\n') + note));
    });
    connect(m_installer, &LspInstaller::failed, this, [this](const QString &error) { finish(false, error); });
}

void LspInstallDialog::begin()
{
    m_start->setEnabled(false);
    m_link->setEnabled(false);
    m_latest->setEnabled(false);
    m_stage->show();
    m_bar->setRange(0, 0);
    m_bar->setTextVisible(true);
    m_bar->show();
    m_installer->start(m_latest->isChecked(), m_link->isChecked());
}

void LspInstallDialog::finish(bool ok, const QString &message)
{
    m_stage->setText(message);
    m_stage->show();
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
        m_link->setEnabled(true);
        m_latest->setEnabled(true);
        m_cancel->setText(tr("Close"));
    }
    adjustSize();
}
