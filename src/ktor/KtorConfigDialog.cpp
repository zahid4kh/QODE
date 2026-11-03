#include "ktor/KtorConfigDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

KtorConfigDialog::KtorConfigDialog(const KtorProject &project, const KtorProject::Options &o, QWidget *parent) : QDialog(parent), m_project(project)
{
    setWindowTitle(tr("Ktor"));
    setMinimumWidth(520);
    auto *layout = new QVBoxLayout(this);
    QStringList facts{project.maven ? tr("Maven") : tr("Gradle")};
    if (!project.engine.isEmpty())
        facts << project.engine;
    if (!project.version.isEmpty())
        facts << tr("Ktor %1").arg(project.version);
    auto *info = new QLabel(tr("<b>%1</b> — %2. Leave a field empty to use what the application configures itself.")
                                .arg(project.appName.toHtmlEscaped(), facts.join(QStringLiteral(" · ")).toHtmlEscaped()),
                            this);
    info->setWordWrap(true);
    layout->addWidget(info);

    auto *form = new QFormLayout;
    m_port = new QSpinBox(this);
    m_port->setRange(0, 65535);
    m_port->setSpecialValueText(tr("Application default"));
    m_port->setValue(o.port);
    m_port->setToolTip(tr("Passed as -port=N, which EngineMain (configuration in application.yaml / .conf) reads. "
                          "A server started with embeddedServer(port = …) in code keeps its own port."));
    form->addRow(tr("Server port"), m_port);
    m_development = new QCheckBox(tr("Development mode"), this);
    m_development->setChecked(o.development);
    m_development->setToolTip(tr("-Dio.ktor.development=true: detailed error pages and logging"));
    form->addRow(QString(), m_development);
    m_autoReload = new QCheckBox(tr("Auto-reload: compile every change and reload the running server"), this);
    m_autoReload->setChecked(o.autoReload);
    m_autoReload->setToolTip(project.maven ? tr("Runs in development mode. After each save, mvn compile builds the classes and Ktor reloads them "
                                                "in place (-watch=target/classes); where Ktor cannot, the server is restarted.")
                                           : tr("Runs in development mode. A continuous Gradle build (-t classes) compiles every change, from "
                                                "any editor, and Ktor reloads the classes in place (-watch=build/classes); where Ktor cannot "
                                                "(embeddedServer without watchPaths), the server is restarted."));
    form->addRow(QString(), m_autoReload);
    m_live = new QCheckBox(tr("Read the routes of the running server"), this);
    m_live->setChecked(o.liveRoutes);
    m_live->setToolTip(tr("Starts the server's JVM with a debug port on 127.0.0.1 (JDWP) so the Routes tab shows the routing tree "
                          "the server really has. QODE only reads it; nothing in the server is paused or changed."));
    form->addRow(QString(), m_live);
    // Auto-reload needs development mode: shown as on while auto-reload is.
    auto syncDevelopment = [this] {
        m_development->setEnabled(!m_autoReload->isChecked());
        m_development->setChecked(m_autoReload->isChecked() || m_developmentWanted);
    };
    m_developmentWanted = o.development;
    connect(m_development, &QCheckBox::toggled, this, [this](bool on) {
        if (m_development->isEnabled())
            m_developmentWanted = on;
    });
    connect(m_autoReload, &QCheckBox::toggled, this, syncDevelopment);
    syncDevelopment();
    m_arguments = new QLineEdit(o.arguments, this);
    m_arguments->setPlaceholderText(tr("e.g. -P:ktor.deployment.host=127.0.0.1 -config=application-dev.yaml"));
    form->addRow(tr("Arguments"), m_arguments);
    m_jvm = new QLineEdit(o.jvmArgs, this);
    m_jvm->setToolTip(project.maven ? tr("Given to Maven's JVM, which runs the server (JAVA_TOOL_OPTIONS)")
                                    : tr("Given only to the JVM that runs the server (through an init script QODE keeps in its cache "
                                         "folder), after the build's own applicationDefaultJvmArgs"));
    m_jvm->setPlaceholderText(tr("e.g. -Xmx512m -Dlogback.configurationFile=logback-dev.xml"));
    form->addRow(tr("JVM options"), m_jvm);
    m_env = new QPlainTextEdit(o.env, this);
    m_env->setPlaceholderText(tr("One per line: PORT=8081 or DATABASE_URL=jdbc:postgresql://localhost/app"));
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
    const QString config = project.configFile();
    if (!config.isEmpty()) {
        auto *open = new QPushButton(tr("Open %1").arg(QFileInfo(config).fileName()), this);
        connect(open, &QPushButton::clicked, this, [this, config] { emit openFileRequested(config); });
        buttons->addButton(open, QDialogButtonBox::ActionRole);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    for (QLineEdit *e : {m_arguments, m_command, m_jvm})
        connect(e, &QLineEdit::textChanged, this, &KtorConfigDialog::updatePreview);
    connect(m_env, &QPlainTextEdit::textChanged, this, &KtorConfigDialog::updatePreview);
    connect(m_port, &QSpinBox::valueChanged, this, &KtorConfigDialog::updatePreview);
    connect(m_development, &QCheckBox::toggled, this, &KtorConfigDialog::updatePreview);
    connect(m_autoReload, &QCheckBox::toggled, this, &KtorConfigDialog::updatePreview);
    connect(m_live, &QCheckBox::toggled, this, &KtorConfigDialog::updatePreview);
    updatePreview();
}

KtorProject::Options KtorConfigDialog::options() const
{
    KtorProject::Options o;
    o.port = m_port->value();
    o.development = m_developmentWanted;
    o.autoReload = m_autoReload->isChecked();
    o.liveRoutes = m_live->isChecked();
    o.arguments = m_arguments->text().trimmed();
    o.command = m_command->text().trimmed();
    o.jvmArgs = m_jvm->text().trimmed();
    o.env = m_env->toPlainText().trimmed();
    return o;
}

void KtorConfigDialog::updatePreview()
{
    const KtorProject::Options o = options();
    QString text = tr("Runs in <b>%1</b>:<br><tt>%2</tt>").arg(m_project.workDir.toHtmlEscaped(), m_project.command(o).toHtmlEscaped());
    const QStringList jvm = m_project.jvmOptions(o, o.liveRoutes ? 1 : 0);
    if (!m_project.maven && o.command.trimmed().isEmpty() && !jvm.isEmpty()) {
        QStringList shown = jvm;
        for (QString &j : shown)
            if (j.startsWith(QLatin1String("-agentlib:jdwp")))
                j = QStringLiteral("-agentlib:jdwp=…,address=127.0.0.1:&lt;free port&gt;");
            else
                j = j.toHtmlEscaped();
        text += tr("<br>The server's JVM also gets: <tt>%1</tt>").arg(shown.join(QLatin1Char(' ')));
    }
    if (o.autoReload && o.command.trimmed().isEmpty())
        text += m_project.maven ? tr("<br>With auto-reload, <tt>mvn compile</tt> runs after each saved source.")
                                : tr("<br>With auto-reload, <tt>%1 -t classes</tt> runs beside it.").arg(m_project.tool.toHtmlEscaped());
    m_preview->setText(text);
    // The wrapped preview grows with the options. A window's minimum size ignores height-for-width, so the label states
    // its height itself (for its current width, or the dialog's narrowest before it is shown).
    const int width = m_preview->isVisible() ? m_preview->width() : minimumWidth() - 24;
    m_preview->setMinimumHeight(m_preview->heightForWidth(width));
    if (isVisible())
        adjustSize();
}
