#include "JdtlsInstallDialog.h"

#include "lsp/JdtlsInstaller.h"
#include "lsp/LspServers.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>

JdtlsInstallDialog::JdtlsInstallDialog(QWidget *parent) : QDialog(parent), m_installer(new JdtlsInstaller(this))
{
    setWindowTitle(tr("Set Up Java Language Server"));
    setMinimumWidth(520);
    auto *layout = new QVBoxLayout(this);

    const LspServers::JavaRuntime java = LspServers::findJava(21);
    QString javaLine;
    if (!java.executable.isEmpty())
        javaLine = tr("<li>Java %1 found (%2): the server will run on it</li>").arg(java.major).arg(java.executable.toHtmlEscaped());
    else
        javaLine = tr("<li><b>%1</b> The server runs on a JDK, which is not installed by QODE: install one first "
                      "(for example <i>sudo apt install openjdk-21-jdk</i>); you can still set the server up now.</li>")
                       .arg(java.major > 0 ? tr("Java 21 or newer is required, but the newest found is Java %1.").arg(java.major)
                                           : tr("Java 21 or newer is required, and no Java was found."));
    auto *info = new QLabel(
        tr("QODE will download the <b>Eclipse JDT Language Server</b> (jdtls) and set it up for you:"
           "<ul>"
           "<li>Download about 50 MB from <i>download.eclipse.org</i>, checked against its published SHA-256 checksum</li>"
           "<li>Unpack it into <b>%1</b></li>"
           "%2"
           "<li>Nothing outside your home folder is changed and no administrator rights are needed</li>"
           "</ul>")
            .arg(JdtlsInstaller::installRoot().toHtmlEscaped(), javaLine),
        this);
    info->setWordWrap(true);
    info->setTextFormat(Qt::RichText);
    layout->addWidget(info);

    m_latest = new QCheckBox(tr("Use the latest milestone instead of the tested version (%1)").arg(JdtlsInstaller::pinnedVersion()), this);
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

    connect(m_start, &QPushButton::clicked, this, &JdtlsInstallDialog::begin);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_installer->isRunning())
            m_installer->cancel();
        else
            reject();
    });
    connect(m_installer, &JdtlsInstaller::stageChanged, m_stage, &QLabel::setText);
    connect(m_installer, &JdtlsInstaller::progress, this, [this](qint64 done, qint64 total) {
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
    connect(m_installer, &JdtlsInstaller::finished, this, [this](const QString &exe, const QString &note) {
        m_executable = exe;
        finish(true, tr("Done. The Java language server is installed.") + (note.isEmpty() ? QString() : QLatin1Char('\n') + note));
    });
    connect(m_installer, &JdtlsInstaller::failed, this, [this](const QString &error) { finish(false, error); });
}

void JdtlsInstallDialog::begin()
{
    m_start->setEnabled(false);
    m_latest->setEnabled(false);
    m_stage->show();
    m_bar->setRange(0, 0);
    m_bar->setTextVisible(true);
    m_bar->show();
    m_installer->start(m_latest->isChecked());
}

void JdtlsInstallDialog::finish(bool ok, const QString &message)
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
        m_latest->setEnabled(true);
        m_cancel->setText(tr("Close"));
    }
    adjustSize();
}
