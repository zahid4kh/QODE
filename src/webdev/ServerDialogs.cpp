#include "ServerDialogs.h"

#include "DevServer.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSpinBox>
#include <QVBoxLayout>

ServerConfigDialog::ServerConfigDialog(const WebProject &project, const QJsonObject &config, QWidget *parent)
    : QDialog(parent), m_project(project)
{
    setWindowTitle(tr("Dev Server"));
    setMinimumWidth(480);
    auto *layout = new QVBoxLayout(this);
    QString intro = tr("Detected: <b>%1</b>").arg(project.packageManager.toHtmlEscaped());
    if (!project.framework.isEmpty())
        intro += tr(" with <b>%1</b>").arg(project.framework.toHtmlEscaped());
    intro += tr(". Leave a field on Automatic to follow the project.");
    auto *info = new QLabel(intro, this);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto *form = new QFormLayout;
    m_manager = new QComboBox(this);
    m_manager->addItem(tr("Automatic (%1)").arg(project.packageManager), QString());
    for (const QString &m : WebProject::managers())
        m_manager->addItem(m, m);
    const int mi = m_manager->findData(config.value(QStringLiteral("manager")).toString());
    m_manager->setCurrentIndex(qMax(0, mi));
    form->addRow(tr("Package manager"), m_manager);

    m_script = new QComboBox(this);
    m_script->setEditable(true);
    m_script->addItems(project.scripts);
    m_script->setCurrentText(config.value(QStringLiteral("script")).toString(project.scripts.value(0)));
    form->addRow(tr("Script"), m_script);

    m_port = new QSpinBox(this);
    m_port->setRange(0, 65535);
    m_port->setSpecialValueText(project.defaultPort ? tr("Automatic (usually %1)").arg(project.defaultPort) : tr("Automatic"));
    m_port->setValue(config.value(QStringLiteral("port")).toInt());
    form->addRow(tr("Port"), m_port);

    m_command = new QLineEdit(config.value(QStringLiteral("command")).toString(), this);
    m_command->setPlaceholderText(tr("Optional: replaces the command below"));
    form->addRow(tr("Custom command"), m_command);
    layout->addLayout(form);

    m_preview = new QLabel(this);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setWordWrap(true);
    layout->addWidget(m_preview);
    auto *note = new QLabel(tr("A fixed port is passed to the server as <b>$PORT</b>%1.")
                                .arg(project.portFlag.isEmpty() ? QString()
                                                                : tr(" and as <b>%1</b>").arg(project.portFlag.toHtmlEscaped())),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    connect(m_manager, &QComboBox::currentIndexChanged, this, &ServerConfigDialog::updatePreview);
    connect(m_script, &QComboBox::currentTextChanged, this, &ServerConfigDialog::updatePreview);
    connect(m_port, &QSpinBox::valueChanged, this, &ServerConfigDialog::updatePreview);
    connect(m_command, &QLineEdit::textChanged, this, &ServerConfigDialog::updatePreview);
    updatePreview();
}

QJsonObject ServerConfigDialog::config() const
{
    QJsonObject c;
    if (!m_manager->currentData().toString().isEmpty())
        c.insert(QStringLiteral("manager"), m_manager->currentData().toString());
    const QString script = m_script->currentText().trimmed();
    if (!script.isEmpty() && script != m_project.scripts.value(0))
        c.insert(QStringLiteral("script"), script);
    if (m_port->value() > 0)
        c.insert(QStringLiteral("port"), m_port->value());
    if (!m_command->text().trimmed().isEmpty())
        c.insert(QStringLiteral("command"), m_command->text().trimmed());
    return c;
}

void ServerConfigDialog::updatePreview()
{
    QString cmd = m_command->text().trimmed();
    if (cmd.isEmpty()) {
        const QString manager = m_manager->currentData().toString().isEmpty() ? m_project.packageManager : m_manager->currentData().toString();
        cmd = m_project.command(manager, m_script->currentText().trimmed(), m_port->value());
    }
    m_preview->setText(tr("Runs: <b>%1</b>").arg(cmd.toHtmlEscaped()));
}

ServerLogDialog::ServerLogDialog(DevServer *server, QWidget *parent) : QDialog(parent), m_view(new QPlainTextEdit(this))
{
    setWindowTitle(tr("Dev Server Log"));
    setWindowModality(Qt::NonModal);
    resize(760, 420);
    auto *layout = new QVBoxLayout(this);
    m_view->setReadOnly(true);
    m_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_view->setMaximumBlockCount(5000);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setPlainText(server->log());
    layout->addWidget(m_view);
    auto *row = new QHBoxLayout;
    auto *clear = new QPushButton(tr("Clear"), this);
    auto *close = new QPushButton(tr("Close"), this);
    row->addStretch(1);
    row->addWidget(clear);
    row->addWidget(close);
    layout->addLayout(row);
    connect(clear, &QPushButton::clicked, m_view, &QPlainTextEdit::clear);
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(server, &DevServer::output, this, [this](const QString &text) {
        QScrollBar *bar = m_view->verticalScrollBar();
        const bool atEnd = bar->value() >= bar->maximum() - 4;
        m_view->moveCursor(QTextCursor::End);
        m_view->insertPlainText(text);
        if (atEnd)
            bar->setValue(bar->maximum());
    });
    m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
}
