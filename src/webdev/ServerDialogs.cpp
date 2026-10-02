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

ServerConfigDialog::ServerConfigDialog(const QString &root, const QJsonObject &config, QWidget *parent) : QDialog(parent), m_root(root)
{
    setWindowTitle(tr("Dev Server"));
    setMinimumWidth(500);
    auto *layout = new QVBoxLayout(this);
    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    layout->addWidget(m_info);

    auto *form = new QFormLayout;
    m_folder = new QComboBox(this);
    QStringList dirs;
    if (WebProject::detect(root).valid)
        dirs << QString();
    dirs << WebProject::findNested(root);
    const QString chosen = config.value(QStringLiteral("dir")).toString();
    if (!dirs.contains(chosen))
        dirs << chosen;
    for (const QString &d : std::as_const(dirs))
        m_folder->addItem(d.isEmpty() ? tr("Project root") : d, d);
    m_folder->setCurrentIndex(qMax(0, m_folder->findData(chosen)));
    form->addRow(tr("Web project folder"), m_folder);

    m_manager = new QComboBox(this);
    form->addRow(tr("Package manager"), m_manager);
    m_script = new QComboBox(this);
    m_script->setEditable(true);
    form->addRow(tr("Script"), m_script);
    m_port = new QSpinBox(this);
    m_port->setRange(0, 65535);
    form->addRow(tr("Port"), m_port);
    m_command = new QLineEdit(config.value(QStringLiteral("command")).toString(), this);
    m_command->setPlaceholderText(tr("Optional: replaces the command below"));
    form->addRow(tr("Custom command"), m_command);
    layout->addLayout(form);

    m_preview = new QLabel(this);
    m_preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview->setWordWrap(true);
    layout->addWidget(m_preview);
    m_portNote = new QLabel(this);
    m_portNote->setWordWrap(true);
    layout->addWidget(m_portNote);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    loadFolder(chosen, config);
    connect(m_folder, &QComboBox::currentIndexChanged, this, [this] { loadFolder(m_folder->currentData().toString(), {}); });
    connect(m_manager, &QComboBox::currentIndexChanged, this, &ServerConfigDialog::updatePreview);
    connect(m_script, &QComboBox::currentTextChanged, this, &ServerConfigDialog::updatePreview);
    connect(m_port, &QSpinBox::valueChanged, this, &ServerConfigDialog::updatePreview);
    connect(m_command, &QLineEdit::textChanged, this, &ServerConfigDialog::updatePreview);
}

// Fills the fields that depend on which folder's package.json is used.
void ServerConfigDialog::loadFolder(const QString &rel, const QJsonObject &config)
{
    const QSignalBlocker b1(m_manager), b2(m_script), b3(m_port);
    m_project = WebProject::detect(rel.isEmpty() ? m_root : m_root + QLatin1Char('/') + rel);
    QString intro = m_project.valid ? tr("Detected: <b>%1</b>").arg(m_project.packageManager.toHtmlEscaped())
                                    : tr("No dev server script found in this folder.");
    if (!m_project.framework.isEmpty())
        intro += tr(" with <b>%1</b>").arg(m_project.framework.toHtmlEscaped());
    m_info->setText(intro + tr(" Leave a field on Automatic to follow the project."));

    m_manager->clear();
    m_manager->addItem(tr("Automatic (%1)").arg(m_project.packageManager), QString());
    for (const QString &m : WebProject::managers())
        m_manager->addItem(m, m);
    m_manager->setCurrentIndex(qMax(0, m_manager->findData(config.value(QStringLiteral("manager")).toString())));
    m_script->clear();
    m_script->addItems(m_project.scripts);
    m_script->setCurrentText(config.value(QStringLiteral("script")).toString(m_project.scripts.value(0)));
    m_port->setSpecialValueText(m_project.configPort ? tr("Automatic (%1)").arg(m_project.configPort)
                                : m_project.defaultPort ? tr("Automatic (usually %1)").arg(m_project.defaultPort)
                                                        : tr("Automatic"));
    m_port->setValue(config.value(QStringLiteral("port")).toInt());
    updatePreview();
}

QJsonObject ServerConfigDialog::config() const
{
    QJsonObject c;
    if (!m_folder->currentData().toString().isEmpty())
        c.insert(QStringLiteral("dir"), m_folder->currentData().toString());
    if (!m_manager->currentData().toString().isEmpty())
        c.insert(QStringLiteral("manager"), m_manager->currentData().toString());
    const QString script = m_script->currentText().trimmed();
    if (!script.isEmpty() && script != m_project.scripts.value(0))
        c.insert(QStringLiteral("script"), script);
    if (m_port->value() > 0 && !m_project.fixedPort(script.isEmpty() ? m_project.scripts.value(0) : script))
        c.insert(QStringLiteral("port"), m_port->value());
    if (!m_command->text().trimmed().isEmpty())
        c.insert(QStringLiteral("command"), m_command->text().trimmed());
    return c;
}

void ServerConfigDialog::updatePreview()
{
    const QString script = m_script->currentText().trimmed();
    const int pinned = m_project.fixedPort(script);
    m_port->setEnabled(!pinned);
    QString cmd = m_command->text().trimmed();
    if (cmd.isEmpty()) {
        const QString manager = m_manager->currentData().toString().isEmpty() ? m_project.packageManager : m_manager->currentData().toString();
        cmd = m_project.command(manager, script, m_port->value());
    }
    m_preview->setText(tr("Runs: <b>%1</b>").arg(cmd.toHtmlEscaped()));
    if (pinned)
        m_portNote->setText(tr("The <b>%1</b> script sets its own port (<b>%2</b>); QODE leaves it alone.").arg(script.toHtmlEscaped()).arg(pinned));
    else if (m_project.configPort)
        m_portNote->setText(tr("<b>%1</b> sets port <b>%2</b>; leave Port on Automatic to keep it. A port set here overrides it.")
                                .arg(m_project.configPortSource.toHtmlEscaped()).arg(m_project.configPort));
    else
        m_portNote->setText(tr("A fixed port is passed to the server as <b>$PORT</b>%1.")
                                .arg(m_project.portFlag.isEmpty() ? QString() : tr(" and as <b>%1</b>").arg(m_project.portFlag.toHtmlEscaped())));
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
