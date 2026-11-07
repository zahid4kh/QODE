#include "jvm/JdkDialog.h"

#include "dialogs/FileDialogs.h"
#include "jvm/JvmTools.h"
#include "platform/Platform.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kHomeRole = Qt::UserRole + 1;
}

JdkDialog::JdkDialog(const QString &chosen, const QString &root, QWidget *parent)
    : QDialog(parent)
    , m_chosen(chosen)
    , m_root(root)
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
        if (!it)
            return;
        m_chosen = it->data(0, kHomeRole).toString();
        updateWarning();
    });
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, &QDialog::accept);
    m_warning = new QLabel(this);
    m_warning->setWordWrap(true);
    m_warning->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    QPushButton *download = buttons->addButton(tr("Download JDK…"), QDialogButtonBox::ActionRole);
    download->setToolTip(tr("Download Eclipse Temurin from adoptium.net into %1").arg(QDir::toNativeSeparators(QDir::homePath() + QStringLiteral("/.jdks"))));
    QPushButton *addBtn = buttons->addButton(tr("Add JDK Folder…"), QDialogButtonBox::ActionRole);
    QPushButton *rescan = buttons->addButton(tr("Rescan"), QDialogButtonBox::ActionRole);
    connect(download, &QPushButton::clicked, this, &JdkDialog::askDownload);
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
    const QString dir = FileDialogs::getExistingDirectory(this, tr("JDK Folder"), QDir::homePath());
    if (dir.isEmpty())
        return;
    const JdkScanner::Jdk j = JdkScanner::fromHome(dir);
    if (!j.isValid()) {
        QMessageBox::warning(this, tr("Project JDK"), tr("%1 holds no JDK (%2 is missing).").arg(QDir::toNativeSeparators(dir), QDir::toNativeSeparators(QStringLiteral("bin/java") + Platform::exeSuffix())));
        return;
    }
    m_chosen = j.home;
    populate();
}

void JdkDialog::askDownload()
{
    // The versions that fix this project first, then the long-term-support releases.
    const JdkScanner::Needs n = JdkScanner::needsOf(m_root, m_jdks, m_chosen);
    QList<int> majors = n.downloads;
    for (int lts : {25, 21, 17, 11, 8})
        if (!majors.contains(lts))
            majors << lts;
    QStringList items;
    for (int m : std::as_const(majors))
        items << (n.downloads.contains(m) ? tr("Java %1 (fixes this project)").arg(m) : tr("Java %1").arg(m));
    bool ok = false;
    const QString item = QInputDialog::getItem(this, tr("Download JDK"),
                                               tr("Eclipse Temurin (adoptium.net), the newest release of the version, into %1. It becomes "
                                                  "this project's JDK.")
                                                   .arg(QDir::toNativeSeparators(QDir::homePath() + QStringLiteral("/.jdks"))),
                                               items, 0, false, &ok);
    if (!ok)
        return;
    m_download = majors.value(int(items.indexOf(item)));
    if (m_download > 0)
        accept();
}

void JdkDialog::updateWarning()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const JdkScanner::Needs n = JdkScanner::needsOf(m_root, m_jdks, m_chosen);
    if (!n.isGradle) {
        m_warning->clear();
        m_warning->hide();
        return;
    }
    QStringList bad, good;
    const JdkScanner::Jdk sel = m_chosen.isEmpty() ? JdkScanner::defaultJdk(m_jdks) : JdkScanner::fromHome(m_chosen);
    if (!n.gradle.isEmpty()) {
        const int min = JvmTools::gradleMinJava(n.gradle), max = JvmTools::gradleMaxJava(n.gradle);
        const QString range = max > 0 ? tr("Java %1 to %2").arg(min).arg(max) : tr("Java %1 and newer").arg(min);
        if (sel.isValid() && !JvmTools::gradleRunsOn(n.gradle, sel.major)) {
            if (n.runner.isValid())
                bad << tr("The project's Gradle %1 runs on %2, not on JDK %3: builds in the terminal fail with this JDK. Choose "
                          "JDK %4, which it runs on (the language servers already use it for their Gradle import).")
                           .arg(n.gradle, range).arg(sel.major).arg(n.runner.major);
            else
                bad << tr("The project's Gradle %1 runs on %2, and none of the installed JDKs is one of those: builds fail, and the "
                          "Kotlin / Java language servers cannot read the project. Download a JDK it runs on, or upgrade the Gradle "
                          "wrapper (Gradle panel > Upgrade…).")
                           .arg(n.gradle, range);
        } else if (sel.isValid()) {
            good << tr("✓ The project's Gradle %1 runs on JDK %2.").arg(n.gradle).arg(sel.major);
        }
    }
    if (n.toolchainMissing)
        bad << tr("The build asks for a Java %1 toolchain (jvmToolchain(%1)), which is not installed, so Gradle fails with \"Cannot "
                  "find a Java installation\". Download JDK %1, or add the plugin id(\"org.gradle.toolchains.foojay-resolver-convention\") "
                  "to settings.gradle.kts so Gradle downloads it.")
                   .arg(n.toolchain);
    else if (n.toolchain > 0)
        good << tr("✓ The build's Java %1 toolchain is installed (or downloaded by its toolchain resolver).").arg(n.toolchain);
    if (bad.isEmpty() && good.isEmpty()) {
        m_warning->hide();
        return;
    }
    m_warning->show();
    if (!bad.isEmpty()) {
        m_warning->setStyleSheet(QStringLiteral("color: %1;").arg(t.warning.name()));
        m_warning->setText(QStringLiteral("⚠ ") + bad.join(QStringLiteral("\n\n⚠ ")));
    } else {
        m_warning->setStyleSheet(QStringLiteral("color: %1;").arg(t.success.name()));
        m_warning->setText(good.join(QLatin1Char('\n')));
    }
}
