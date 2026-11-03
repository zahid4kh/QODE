#include "jvm/JdkDialog.h"

#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kHomeRole = Qt::UserRole + 1;
}

JdkDialog::JdkDialog(const QString &chosen, int toolchain, bool autoProvision, QWidget *parent)
    : QDialog(parent)
    , m_chosen(chosen)
    , m_toolchain(toolchain)
    , m_autoProvision(autoProvision)
{
    setWindowTitle(tr("Project JDK"));
    resize(720, 420);
    auto *intro = new QLabel(tr("The JDK this project uses: QODE sets JAVA_HOME and puts its bin/ first on PATH for new terminal "
                                "sessions and for what it runs (Run, Debug, Gradle tasks, scripts). The build's own toolchains still "
                                "decide what compiles the code."),
                             this);
    intro->setWordWrap(true);
    m_list = new QTreeWidget(this);
    m_list->setHeaderLabels({tr("Version"), tr("Vendor"), tr("Found in"), tr("Folder")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_list->header()->setStretchLastSection(true);
    connect(m_list, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *it) {
        if (it)
            m_chosen = it->data(0, kHomeRole).toString();
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, &QDialog::accept);
    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QPushButton *addBtn = buttons->addButton(tr("Add JDK Folder…"), QDialogButtonBox::ActionRole);
    QPushButton *rescan = buttons->addButton(tr("Rescan"), QDialogButtonBox::ActionRole);
    connect(addBtn, &QPushButton::clicked, this, &JdkDialog::addFolder);
    connect(rescan, &QPushButton::clicked, this, &JdkDialog::populate);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_warning);
    layout->addWidget(buttons);
    populate();
}

void JdkDialog::populate()
{
    m_jdks = JdkScanner::scan();
    // A chosen folder outside the usual places stays listed.
    if (!m_chosen.isEmpty()) {
        bool listed = false;
        for (const JdkScanner::Jdk &j : std::as_const(m_jdks))
            listed = listed || j.home == m_chosen;
        if (!listed) {
            JdkScanner::Jdk j = JdkScanner::fromHome(m_chosen);
            if (j.isValid()) {
                j.source = tr("Chosen");
                m_jdks.prepend(j);
            }
        }
    }
    const JdkScanner::Jdk def = JdkScanner::defaultJdk(m_jdks);
    const QString keep = m_chosen;
    m_list->clear();
    QFont bold = m_list->font();
    bold.setBold(true);
    auto *system = new QTreeWidgetItem(m_list, {tr("System default"), def.isValid() ? def.label() : tr("none found"), QStringLiteral("JAVA_HOME / PATH"),
                                                 def.isValid() ? def.home : QString()});
    system->setData(0, kHomeRole, QString());
    QTreeWidgetItem *current = system;
    for (const JdkScanner::Jdk &j : std::as_const(m_jdks)) {
        auto *it = new QTreeWidgetItem(m_list, {j.version.isEmpty() ? QString::number(j.major) : j.version, j.vendor + (j.jre ? tr(" (JRE)") : QString()),
                                                j.source, QDir::home().relativeFilePath(j.home).startsWith(QLatin1String("..")) ? j.home
                                                                                                                                 : QStringLiteral("~/") + QDir::home().relativeFilePath(j.home)});
        it->setData(0, kHomeRole, j.home);
        it->setToolTip(3, j.home);
        if (j.home == keep)
            current = it;
    }
    for (int c = 0; c < 4; ++c)
        current->setFont(c, bold);
    m_list->setCurrentItem(current);
    m_chosen = keep;
    for (int c = 0; c < 3; ++c)
        m_list->resizeColumnToContents(c);
    updateWarning();
}

void JdkDialog::addFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("JDK Folder"), QDir::homePath());
    if (dir.isEmpty())
        return;
    const JdkScanner::Jdk j = JdkScanner::fromHome(dir);
    if (!j.isValid()) {
        QMessageBox::warning(this, tr("Project JDK"), tr("%1 holds no JDK (bin/java is missing).").arg(dir));
        return;
    }
    m_chosen = j.home;
    populate();
}

void JdkDialog::updateWarning()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    if (m_toolchain <= 0) {
        m_warning->clear();
        m_warning->hide();
        return;
    }
    bool have = false;
    for (const JdkScanner::Jdk &j : std::as_const(m_jdks))
        have = have || (j.major == m_toolchain && !j.jre);
    m_warning->show();
    if (have) {
        m_warning->setStyleSheet(QStringLiteral("color: %1;").arg(t.success.name()));
        m_warning->setText(tr("✓ The build asks for a Java %1 toolchain, and one is installed.").arg(m_toolchain));
    } else if (m_autoProvision) {
        m_warning->setStyleSheet(QStringLiteral("color: %1;").arg(t.textMuted.name()));
        m_warning->setText(tr("The build asks for a Java %1 toolchain, which is not installed. Its foojay toolchain resolver will download "
                              "one on the first build.")
                               .arg(m_toolchain));
    } else {
        m_warning->setStyleSheet(QStringLiteral("color: %1;").arg(t.warning.name()));
        m_warning->setText(tr("⚠ The build asks for a Java %1 toolchain (jvmToolchain(%1)), which is not installed, so Gradle will fail with "
                              "\"Cannot find a Java installation\". Install one (sudo apt install openjdk-%1-jdk, or sdk install java %1-tem…), "
                              "or add the plugin id(\"org.gradle.toolchains.foojay-resolver-convention\") to settings.gradle.kts so Gradle "
                              "downloads it.")
                               .arg(m_toolchain));
    }
}
