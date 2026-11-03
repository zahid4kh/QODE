#include "jvm/DependencyDialog.h"

#include "jvm/MavenCentral.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kGroupRole = Qt::UserRole + 1;
constexpr int kArtifactRole = Qt::UserRole + 2;
constexpr int kIndexRole = Qt::UserRole + 3;
constexpr int kNewRole = Qt::UserRole + 4;
constexpr int kMaxParallel = 6;
} // namespace

DependencyDialog::DependencyDialog(const QString &root, const QString &currentFile, QWidget *parent)
    : QDialog(parent)
    , m_root(root)
    , m_central(new MavenCentral(this))
{
    setWindowTitle(tr("Dependencies"));
    resize(720, 560);
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(buildAddTab(), tr("Add"));
    m_tabs->addTab(buildUpdatesTab(), tr("Updates"));
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int i) {
        if (i == 1 && m_declared.isEmpty() && m_lookupBusy == 0)
            checkUpdates();
    });
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_tabs, 1);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_status, 1);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    // Targets: the build files of the project, the module of the open file first.
    const QStringList files = DependencyEdit::buildFiles(root);
    QString preferred;
    for (const QString &f : files)
        if (!currentFile.isEmpty() && currentFile.startsWith(QFileInfo(f).absolutePath() + QLatin1Char('/'))
            && QFileInfo(f).absolutePath().size() > QFileInfo(preferred).absolutePath().size())
            preferred = f;
    for (const QString &f : files) {
        m_target->addItem(QDir(root).relativeFilePath(f), f);
        if (f == preferred)
            m_target->setCurrentIndex(m_target->count() - 1);
    }
    updateTarget();
    m_query->setFocus();
    setStatus(files.isEmpty() ? tr("No Gradle or Maven build file was found in the project.") : QString(), files.isEmpty());
}

QWidget *DependencyDialog::buildAddTab()
{
    auto *w = new QWidget(this);
    m_query = new QLineEdit(w);
    m_query->setPlaceholderText(tr("Search Maven Central: gson, ktor client, org.jetbrains.kotlinx:kotlinx-coroutines"));
    m_query->setClearButtonEnabled(true);
    auto *searchBtn = new QPushButton(tr("Search"), w);
    connect(m_query, &QLineEdit::returnPressed, this, &DependencyDialog::search);
    connect(searchBtn, &QPushButton::clicked, this, &DependencyDialog::search);
    m_results = new QTreeWidget(w);
    m_results->setHeaderLabels({tr("Library"), tr("Latest"), tr("Versions")});
    m_results->setRootIsDecorated(false);
    m_results->setUniformRowHeights(true);
    m_results->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_results->header()->setStretchLastSection(false);
    connect(m_results, &QTreeWidget::currentItemChanged, this, &DependencyDialog::selectResult);
    connect(m_results, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (m_addBtn->isEnabled())
            add();
    });

    m_version = new QComboBox(w);
    m_version->setMinimumWidth(160);
    m_preRelease = new QCheckBox(tr("Show pre-releases"), w);
    connect(m_preRelease, &QCheckBox::toggled, this, &DependencyDialog::fillVersions);
    m_target = new QComboBox(w);
    connect(m_target, &QComboBox::currentIndexChanged, this, &DependencyDialog::updateTarget);
    m_scope = new QComboBox(w);
    m_catalog = new QCheckBox(tr("Add to the version catalog (gradle/libs.versions.toml)"), w);
    m_addBtn = new QPushButton(tr("Add Dependency"), w);
    m_addBtn->setEnabled(false);
    connect(m_addBtn, &QPushButton::clicked, this, &DependencyDialog::add);

    auto *row = new QHBoxLayout;
    row->addWidget(m_query, 1);
    row->addWidget(searchBtn);
    auto *versionRow = new QHBoxLayout;
    versionRow->addWidget(m_version);
    versionRow->addWidget(m_preRelease);
    versionRow->addStretch(1);
    auto *form = new QFormLayout;
    form->addRow(tr("Version:"), versionRow);
    form->addRow(tr("Module:"), m_target);
    form->addRow(tr("Scope:"), m_scope);
    form->addRow(QString(), m_catalog);
    auto *layout = new QVBoxLayout(w);
    layout->addLayout(row);
    layout->addWidget(m_results, 1);
    layout->addLayout(form);
    layout->addWidget(m_addBtn, 0, Qt::AlignRight);
    return w;
}

QWidget *DependencyDialog::buildUpdatesTab()
{
    auto *w = new QWidget(this);
    m_updatesState = new QLabel(tr("Checks every library the build declares with a version against Maven Central."), w);
    m_updatesState->setWordWrap(true);
    m_updates = new QTreeWidget(w);
    m_updates->setHeaderLabels({tr("Library"), tr("Current"), tr("Newest"), tr("Declared in")});
    m_updates->setRootIsDecorated(false);
    m_updates->setUniformRowHeights(true);
    m_updates->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_updates->header()->setStretchLastSection(false);
    m_checkBtn = new QPushButton(tr("Check Again"), w);
    connect(m_checkBtn, &QPushButton::clicked, this, &DependencyDialog::checkUpdates);
    m_applyBtn = new QPushButton(tr("Update Selected"), w);
    m_applyBtn->setEnabled(false);
    connect(m_applyBtn, &QPushButton::clicked, this, [this] {
        QList<QPair<DependencyEdit::Declared, QString>> list;
        for (int i = 0; i < m_updates->topLevelItemCount(); ++i) {
            QTreeWidgetItem *it = m_updates->topLevelItem(i);
            if (it->checkState(0) == Qt::Checked)
                list << qMakePair(m_declared.value(it->data(0, kIndexRole).toInt()), it->data(0, kNewRole).toString());
        }
        if (!list.isEmpty())
            emit updatesRequested(list);
    });
    connect(m_updates, &QTreeWidget::itemChanged, this, [this] {
        bool any = false;
        for (int i = 0; i < m_updates->topLevelItemCount() && !any; ++i)
            any = m_updates->topLevelItem(i)->checkState(0) == Qt::Checked;
        m_applyBtn->setEnabled(any);
    });
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_checkBtn);
    buttons->addWidget(m_applyBtn);
    auto *layout = new QVBoxLayout(w);
    layout->addWidget(m_updatesState);
    layout->addWidget(m_updates, 1);
    layout->addLayout(buttons);
    return w;
}

void DependencyDialog::setScript(const QString &path, const QString &text)
{
    m_script = path;
    m_scriptText = text;
    setWindowTitle(tr("Dependencies of %1").arg(QFileInfo(path).fileName()));
    m_target->clear();
    m_target->setEnabled(false);
    m_target->addItem(QFileInfo(path).fileName(), path);
    m_scope->clear();
    m_scope->setEnabled(false);
    m_catalog->hide();
    m_addBtn->setText(tr("Add @file:DependsOn"));
    setStatus(QString());
}

void DependencyDialog::showUpdates()
{
    m_tabs->setCurrentIndex(1);
}

void DependencyDialog::setStatus(const QString &text, bool error)
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    m_status->setStyleSheet(QStringLiteral("color: %1;").arg((error ? t.danger : t.textMuted).name()));
    m_status->setText(text);
}

void DependencyDialog::updateTarget()
{
    if (!m_script.isEmpty())
        return;
    const QString file = m_target->currentData().toString();
    const bool maven = file.endsWith(QLatin1String("pom.xml"));
    const QString keep = m_scope->currentText();
    m_scope->clear();
    m_scope->addItems(maven ? DependencyEdit::mavenScopes() : DependencyEdit::gradleScopes());
    if (const int i = m_scope->findText(keep); i >= 0)
        m_scope->setCurrentIndex(i);
    const bool catalog = !maven && !DependencyEdit::catalogPath(m_root).isEmpty();
    m_catalog->setVisible(catalog);
    m_catalog->setChecked(catalog && DependencyEdit::buildUsesCatalog(file));
}

void DependencyDialog::search()
{
    const QString q = m_query->text().trimmed();
    if (q.isEmpty())
        return;
    const int token = ++m_searchToken;
    m_results->clear();
    m_version->clear();
    m_addBtn->setEnabled(false);
    setStatus(tr("Searching Maven Central…"));
    m_central->search(q, [this, token](const QList<MavenCentral::Artifact> &hits, const QString &error) {
        if (token != m_searchToken)
            return;
        if (!error.isEmpty()) {
            setStatus(tr("Search failed: %1").arg(error), true);
            return;
        }
        for (const MavenCentral::Artifact &a : hits) {
            auto *it = new QTreeWidgetItem(m_results, {a.coordinate(), a.latest, QString::number(a.versionCount)});
            it->setData(0, kGroupRole, a.group);
            it->setData(0, kArtifactRole, a.artifact);
        }
        setStatus(hits.isEmpty() ? tr("Nothing found.") : tr("%n result(s). Pick one to see its versions.", nullptr, int(hits.size())));
        if (m_results->topLevelItemCount() > 0)
            m_results->setCurrentItem(m_results->topLevelItem(0));
    });
}

void DependencyDialog::selectResult()
{
    QTreeWidgetItem *it = m_results->currentItem();
    m_version->clear();
    m_versions.clear();
    m_addBtn->setEnabled(false);
    if (!it)
        return;
    const QString group = it->data(0, kGroupRole).toString(), artifact = it->data(0, kArtifactRole).toString();
    const int token = ++m_searchToken;
    setStatus(tr("Reading the versions of %1…").arg(group + QLatin1Char(':') + artifact));
    m_central->versions(group, artifact, [this, token, it](const QStringList &versions, const QString &error) {
        if (token != m_searchToken)
            return;
        if (!error.isEmpty()) {
            // The search index still knows its latest version.
            m_versions = it->text(1).isEmpty() ? QStringList() : QStringList{it->text(1)};
            setStatus(tr("Could not read the version list (%1); offering the latest one the search knows.").arg(error), true);
        } else {
            m_versions = versions;
            setStatus(QString());
        }
        fillVersions();
    });
}

void DependencyDialog::fillVersions()
{
    m_version->clear();
    const bool all = m_preRelease->isChecked();
    for (const QString &v : std::as_const(m_versions))
        if (all || MavenCentral::isStable(v))
            m_version->addItem(v);
    if (m_version->count() == 0 && !m_versions.isEmpty())
        m_version->addItem(m_versions.first()); // only pre-releases published
    m_addBtn->setEnabled(m_version->count() > 0 && m_target->count() > 0);
}

void DependencyDialog::add()
{
    QTreeWidgetItem *it = m_results->currentItem();
    if (!it || m_version->currentText().isEmpty())
        return;
    const QString group = it->data(0, kGroupRole).toString(), artifact = it->data(0, kArtifactRole).toString();
    if (!m_script.isEmpty()) {
        emit dependsOnRequested(group + QLatin1Char(':') + artifact + QLatin1Char(':') + m_version->currentText());
        return;
    }
    emit addRequested(m_target->currentData().toString(), group, artifact, m_version->currentText(), m_scope->currentText(),
                      m_catalog->isVisible() && m_catalog->isChecked());
}

void DependencyDialog::checkUpdates()
{
    m_updates->clear();
    m_applyBtn->setEnabled(false);
    m_declared = m_script.isEmpty() ? DependencyEdit::declared(m_root) : DependencyEdit::declaredInScript(m_script, m_scriptText);
    m_lookupNext = 0;
    m_found = 0;
    if (m_declared.isEmpty()) {
        m_updatesState->setText(tr("No dependency with a literal version was found."));
        return;
    }
    m_checkBtn->setEnabled(false);
    m_updatesState->setText(tr("Checking %1 libraries on Maven Central…").arg(m_declared.size()));
    while (m_lookupBusy < kMaxParallel && m_lookupNext < m_declared.size())
        nextVersionLookup();
}

void DependencyDialog::nextVersionLookup()
{
    const int index = m_lookupNext++;
    const DependencyEdit::Declared d = m_declared.at(index);
    ++m_lookupBusy;
    m_central->versions(d.group, d.artifact, [this, index](const QStringList &versions, const QString &) {
        --m_lookupBusy;
        const DependencyEdit::Declared d = m_declared.value(index);
        const QString newest = MavenCentral::newestFor(versions, d.version);
        if (!newest.isEmpty() && MavenCentral::compareVersions(newest, d.version) > 0) {
            ++m_found;
            QString name = d.group + QLatin1Char(':') + d.artifact;
            if (!d.sharedBy.isEmpty())
                name += tr(" (+%n more)", nullptr, int(d.sharedBy.size()));
            auto *it = new QTreeWidgetItem(m_updates, {name, d.version, newest, d.where});
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(0, Qt::Unchecked);
            it->setData(0, kIndexRole, index);
            it->setData(0, kNewRole, newest);
            if (!d.sharedBy.isEmpty())
                it->setToolTip(0, tr("The same version is used by: %1").arg(d.sharedBy.join(QStringLiteral(", "))));
        }
        if (m_lookupNext < m_declared.size()) {
            nextVersionLookup();
            return;
        }
        if (m_lookupBusy == 0) {
            m_checkBtn->setEnabled(true);
            m_updatesState->setText(m_found == 0 ? tr("Everything is up to date (%1 libraries checked).").arg(m_declared.size())
                                                 : tr("%1 of %2 libraries have a newer version. Tick the ones to update.").arg(m_found).arg(m_declared.size()));
        }
    });
}
