#include "PackagesDialog.h"

#include "project/PythonEnv.h"
#include "python/PackageManager.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {
enum Column { NameColumn, InstalledColumn, LatestColumn };

// pip treats "My_Package", "my-package" and "my.package" as one name.
QString normalized(const QString &name)
{
    QString n = name.toLower();
    n.replace(QLatin1Char('_'), QLatin1Char('-'));
    n.replace(QLatin1Char('.'), QLatin1Char('-'));
    return n;
}
} // namespace

PackagesDialog::PackagesDialog(const QString &projectRoot, QWidget *parent)
    : QDialog(parent), m_root(projectRoot), m_pm(new PackageManager(this))
{
    setWindowTitle(tr("Python Packages"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(760, 600);
    setMinimumSize(560, 440);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 16, 18, 14);
    outer->setSpacing(10);

    // Header: what environment this is, and a venv picker when the project has several.
    auto *head = new QHBoxLayout;
    head->setSpacing(10);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(1);
    m_title = new QLabel(tr("Python Packages"), this);
    m_title->setObjectName(QStringLiteral("pkgTitle"));
    m_subtitle = new QLabel(this);
    m_subtitle->setObjectName(QStringLiteral("pkgSubtitle"));
    m_subtitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);
    head->addLayout(titles, 1);
    m_venvCombo = new QComboBox(this);
    m_venvCombo->setToolTip(tr("Virtual environment of this project"));
    m_venvCombo->hide();
    head->addWidget(m_venvCombo, 0, Qt::AlignVCenter);
    m_refresh = new QToolButton(this);
    m_refresh->setToolTip(tr("Reload the package list"));
    head->addWidget(m_refresh, 0, Qt::AlignVCenter);
    outer->addLayout(head);

    m_pages = new QStackedWidget(this);
    outer->addWidget(m_pages, 1);

    // --- Page 0: the packages -------------------------------------------------------------
    auto *main = new QWidget(m_pages);
    auto *col = new QVBoxLayout(main);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(10);

    auto *installRow = new QHBoxLayout;
    installRow->setSpacing(8);
    m_installEdit = new QLineEdit(main);
    m_installEdit->setPlaceholderText(tr("Install packages: requests  flask  django>=5  httpx[http2]"));
    m_installEdit->setClearButtonEnabled(true);
    m_installBtn = new QPushButton(tr("Install"), main);
    m_installBtn->setObjectName(QStringLiteral("primaryBtn"));
    m_fileBtn = new QPushButton(tr("From File…"), main);
    m_fileBtn->setToolTip(tr("Install from a requirements.txt, or the project of a pyproject.toml / setup.py"));
    installRow->addWidget(m_installEdit, 1);
    installRow->addWidget(m_installBtn);
    installRow->addWidget(m_fileBtn);
    col->addLayout(installRow);

    auto *filterRow = new QHBoxLayout;
    filterRow->setSpacing(10);
    m_filter = new QLineEdit(main);
    m_filter->setPlaceholderText(tr("Filter installed packages"));
    m_filter->setClearButtonEnabled(true);
    m_outdatedOnly = new QCheckBox(tr("Updates available"), main);
    m_outdatedOnly->setEnabled(false);
    m_count = new QLabel(main);
    m_count->setObjectName(QStringLiteral("countPill"));
    filterRow->addWidget(m_filter, 1);
    filterRow->addWidget(m_outdatedOnly);
    filterRow->addWidget(m_count);
    col->addLayout(filterRow);

    m_tree = new QTreeWidget(main);
    m_tree->setObjectName(QStringLiteral("pkgTree"));
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Package"), tr("Installed"), tr("Latest")});
    m_tree->setRootIsDecorated(false);
    m_tree->setUniformRowHeights(true);
    m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(NameColumn, Qt::AscendingOrder);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(InstalledColumn, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(LatestColumn, QHeaderView::ResizeToContents);
    m_tree->header()->setMinimumSectionSize(110);
    col->addWidget(m_tree, 1);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    m_upgradeBtn = new QPushButton(tr("Upgrade"), main);
    m_upgradeAllBtn = new QPushButton(tr("Upgrade All"), main);
    m_upgradeAllBtn->setToolTip(tr("Upgrade every package that has a newer version"));
    m_uninstallBtn = new QPushButton(tr("Uninstall"), main);
    m_uninstallBtn->setObjectName(QStringLiteral("dangerBtn"));
    m_logBtn = new QPushButton(tr("Show Log"), main);
    m_logBtn->setCheckable(true);
    actions->addWidget(m_upgradeBtn);
    actions->addWidget(m_upgradeAllBtn);
    actions->addWidget(m_uninstallBtn);
    actions->addStretch(1);
    actions->addWidget(m_logBtn);
    col->addLayout(actions);
    m_pages->addWidget(main);

    // --- Page 1: no environment -----------------------------------------------------------
    auto *empty = new QWidget(m_pages);
    auto *ec = new QVBoxLayout(empty);
    ec->setAlignment(Qt::AlignCenter);
    auto *emptyTitle = new QLabel(tr("No virtual environment"), empty);
    emptyTitle->setObjectName(QStringLiteral("emptyTitle"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    m_emptyText = new QLabel(empty);
    m_emptyText->setObjectName(QStringLiteral("emptyText"));
    m_emptyText->setAlignment(Qt::AlignCenter);
    m_emptyText->setWordWrap(true);
    m_createBtn = new QPushButton(tr("Create .venv"), empty);
    m_createBtn->setObjectName(QStringLiteral("primaryBtn"));
    ec->addStretch(1);
    ec->addWidget(emptyTitle);
    ec->addWidget(m_emptyText);
    ec->addSpacing(8);
    ec->addWidget(m_createBtn, 0, Qt::AlignHCenter);
    ec->addStretch(2);
    m_pages->addWidget(empty);

    // --- Footer: progress and log ---------------------------------------------------------
    auto *statusRow = new QHBoxLayout;
    statusRow->setSpacing(10);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 0);
    m_bar->setTextVisible(false);
    m_bar->setFixedWidth(120);
    m_bar->hide();
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->hide();
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_bar);
    statusRow->addWidget(m_cancelBtn);
    outer->addLayout(statusRow);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setMaximumBlockCount(1000);
    m_log->setFixedHeight(160);
    m_log->hide();
    outer->addWidget(m_log);

    // --- Wiring -----------------------------------------------------------------------------
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &PackagesDialog::applyTheme);
    connect(m_refresh, &QToolButton::clicked, this, &PackagesDialog::loadEnvironment);
    connect(m_venvCombo, &QComboBox::activated, this, [this](int) {
        SettingsManager::instance().setPythonVenv(m_venvCombo->currentData().toString());
        loadEnvironment();
        emit environmentChanged();
    });
    connect(m_installBtn, &QPushButton::clicked, this, &PackagesDialog::installTyped);
    connect(m_installEdit, &QLineEdit::returnPressed, this, &PackagesDialog::installTyped);
    connect(m_installEdit, &QLineEdit::textChanged, this, &PackagesDialog::updateButtons);
    connect(m_fileBtn, &QPushButton::clicked, this, &PackagesDialog::installFromFile);
    connect(m_filter, &QLineEdit::textChanged, this, &PackagesDialog::applyFilter);
    connect(m_outdatedOnly, &QCheckBox::toggled, this, &PackagesDialog::applyFilter);
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, &PackagesDialog::updateButtons);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &PackagesDialog::showContextMenu);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://pypi.org/project/") + item->text(NameColumn)));
    });
    connect(m_upgradeBtn, &QPushButton::clicked, this, &PackagesDialog::upgradeSelected);
    connect(m_upgradeAllBtn, &QPushButton::clicked, this, &PackagesDialog::upgradeOutdated);
    connect(m_uninstallBtn, &QPushButton::clicked, this, &PackagesDialog::uninstallSelected);
    connect(m_logBtn, &QPushButton::toggled, this, [this](bool on) {
        m_log->setVisible(on);
        m_logBtn->setText(on ? tr("Hide Log") : tr("Show Log"));
    });
    connect(m_cancelBtn, &QPushButton::clicked, m_pm, &PackageManager::cancel);
    connect(m_createBtn, &QPushButton::clicked, this, [this] {
        m_pm->setEnvironment(m_root, QStringLiteral(".venv"));
        m_pm->createVenv(QStringLiteral(".venv"));
    });

    connect(m_pm, &PackageManager::installedListed, this, [this](const QList<PythonTools::Package> &packages) {
        m_installed = packages;
        m_latest.clear();
        m_checkedUpdates = false;
        m_status->setText(tr("Checking for updates…"));
        rebuildList();
    });
    connect(m_pm, &PackageManager::outdatedListed, this, [this](const QList<PythonTools::Package> &packages) {
        m_latest.clear();
        for (const PythonTools::Package &p : packages)
            m_latest.insert(normalized(p.name), p.latest);
        m_checkedUpdates = true;
        m_status->setText(m_latest.isEmpty() ? tr("Everything is up to date.") : tr("%n update(s) available.", nullptr, m_latest.size()));
        rebuildList();
    });
    connect(m_pm, &PackageManager::listFailed, this, [this](const QString &error) {
        m_installed.clear();
        rebuildList();
        m_status->setText(error);
    });
    connect(m_pm, &PackageManager::output, this, &PackagesDialog::appendLog);
    connect(m_pm, &PackageManager::operationStarted, this, [this](const QString &title) { setBusy(true, title + QStringLiteral("…")); });
    connect(m_pm, &PackageManager::operationFinished, this, [this](bool ok, const QString &title, const QString &error) {
        setBusy(false);
        if (m_pages->currentIndex() == 1 && ok) { // the environment was just created
            const QString name = QStringLiteral(".venv");
            SettingsManager::instance().setPythonVenv(QString());
            loadEnvironment();
            emit venvCreated(name);
            return;
        }
        if (ok) {
            m_installEdit->clear();
            m_status->setText(tr("%1: done.").arg(title));
            emit packagesChanged();
            m_pm->refresh();
        } else {
            m_status->setText(error);
            m_logBtn->setChecked(true); // show what went wrong
        }
    });

    applyTheme();
    loadEnvironment();
    m_installEdit->setFocus();
}

void PackagesDialog::reject()
{
    if (m_busy)
        m_pm->cancel();
    QDialog::reject();
}

// Local styling from the active theme, so the dialog follows theme changes and custom themes.
void PackagesDialog::applyTheme()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    setStyleSheet(QStringLiteral(R"(
QLabel#pkgTitle { font-size: 14pt; font-weight: 700; }
QLabel#pkgSubtitle { color: %1; }
QLabel#emptyTitle { font-size: 16pt; }
QTreeWidget#pkgTree { background: %2; border: 1px solid %3; border-radius: 8px; padding: 2px; }
QTreeWidget#pkgTree::item { padding: 5px 6px; border-radius: 5px; }
QTreeWidget#pkgTree QHeaderView::section { background: transparent; color: %1; border: none; border-bottom: 1px solid %3; padding: 6px 8px; font-size: 8pt; font-weight: 600; letter-spacing: 1px; }
QPushButton#dangerBtn { color: %4; }
QPushButton#dangerBtn:hover { border-color: %4; }
QPushButton#dangerBtn:disabled { color: %1; }
QProgressBar { background: %2; border: 1px solid %3; border-radius: 4px; max-height: 6px; }
QProgressBar::chunk { background: %5; border-radius: 3px; }
QCheckBox { color: %1; }
)")
                      .arg(t.textMuted.name(), t.panel.name(), t.border.name(), t.danger.name(), t.accent.name()));
    Icons::bind(m_refresh, QStringLiteral(":/new-icons/refresh-cw.svg"));
    for (QAction *a : m_filter->actions())
        m_filter->removeAction(a);
    m_filter->addAction(Icons::tinted(QStringLiteral(":/new-icons/search.svg"), t.textMuted), QLineEdit::LeadingPosition);
    m_emptyText->setStyleSheet(QStringLiteral("color: %1;").arg(t.textMuted.name()));
    rebuildList();
}

void PackagesDialog::loadEnvironment()
{
    const QStringList venvs = PythonEnv::findVenvs(m_root);
    if (venvs.isEmpty()) {
        m_pm->setEnvironment(m_root, QString());
        showEmptyState(true);
        return;
    }
    const QString active = PythonEnv::activeVenv(m_root);
    m_venvCombo->blockSignals(true);
    m_venvCombo->clear();
    for (const QString &v : venvs)
        m_venvCombo->addItem(v, v);
    m_venvCombo->setCurrentIndex(qMax(0, venvs.indexOf(active)));
    m_venvCombo->blockSignals(false);
    m_venvCombo->setVisible(venvs.size() > 1);

    m_pm->setEnvironment(m_root, active);
    showEmptyState(false);
    const QString version = PythonEnv::venvVersion(m_root, active);
    m_subtitle->setText(tr("%1  ·  %2  ·  managed with %3")
                            .arg(active, version.isEmpty() ? tr("Python") : tr("Python %1").arg(version), m_pm->tool()));
    m_installed.clear();
    m_latest.clear();
    rebuildList();
    m_status->setText(tr("Loading packages…"));
    m_pm->refresh();
}

void PackagesDialog::showEmptyState(bool noVenv)
{
    m_pages->setCurrentIndex(noVenv ? 1 : 0);
    if (noVenv) {
        m_subtitle->setText(QFileInfo(m_root).fileName());
        m_emptyText->setText(tr("Packages are installed per project into a virtual environment, never into the system Python.\n"
                                "QODE creates one in this project's .venv folder with %1.")
                                 .arg(m_pm->usesUv() ? tr("uv") : tr("python3 -m venv")));
        m_status->clear();
    }
    updateButtons();
}

void PackagesDialog::rebuildList()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    const QStringList keepSelected = selectedNames();
    m_tree->setSortingEnabled(false);
    m_tree->clear();
    for (const PythonTools::Package &p : std::as_const(m_installed)) {
        auto *item = new QTreeWidgetItem(m_tree);
        item->setText(NameColumn, p.name);
        item->setText(InstalledColumn, p.version);
        const QString latest = m_latest.value(normalized(p.name));
        if (!latest.isEmpty()) {
            item->setText(LatestColumn, QStringLiteral("↑ ") + latest);
            item->setForeground(LatestColumn, t.accent);
            item->setData(LatestColumn, Qt::UserRole, true);
        }
        item->setForeground(InstalledColumn, t.textMuted);
        if (keepSelected.contains(p.name))
            item->setSelected(true);
    }
    m_tree->setSortingEnabled(true);
    m_outdatedOnly->setEnabled(m_checkedUpdates && !m_latest.isEmpty());
    if (m_latest.isEmpty())
        m_outdatedOnly->setChecked(false);
    applyFilter();
}

void PackagesDialog::applyFilter()
{
    const QString needle = m_filter->text().trimmed().toLower();
    const bool only = m_outdatedOnly->isChecked();
    int shown = 0;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_tree->topLevelItem(i);
        const bool match = (needle.isEmpty() || item->text(NameColumn).toLower().contains(needle)) &&
                           (!only || item->data(LatestColumn, Qt::UserRole).toBool());
        item->setHidden(!match);
        shown += match;
    }
    m_count->setText(QString::number(m_installed.isEmpty() ? 0 : shown));
    m_count->setToolTip(tr("%1 of %2 packages shown").arg(shown).arg(m_installed.size()));
    updateButtons();
}

QStringList PackagesDialog::selectedNames() const
{
    QStringList names;
    for (QTreeWidgetItem *item : m_tree->selectedItems())
        if (!item->isHidden())
            names << item->text(NameColumn);
    return names;
}

QStringList PackagesDialog::outdatedNames() const
{
    QStringList names;
    for (const PythonTools::Package &p : m_installed)
        if (m_latest.contains(normalized(p.name)))
            names << p.name;
    return names;
}

void PackagesDialog::updateButtons()
{
    const int selected = selectedNames().size();
    const int outdated = outdatedNames().size();
    const bool idle = !m_busy;
    m_installBtn->setEnabled(idle && !m_installEdit->text().trimmed().isEmpty());
    m_installEdit->setEnabled(idle);
    m_fileBtn->setEnabled(idle);
    m_venvCombo->setEnabled(idle);
    m_refresh->setEnabled(idle);
    m_createBtn->setEnabled(idle);
    m_upgradeBtn->setText(selected > 1 ? tr("Upgrade %1").arg(selected) : tr("Upgrade"));
    m_upgradeBtn->setEnabled(idle && selected > 0);
    m_upgradeAllBtn->setText(outdated > 0 ? tr("Upgrade All (%1)").arg(outdated) : tr("Upgrade All"));
    m_upgradeAllBtn->setEnabled(idle && outdated > 0);
    m_uninstallBtn->setText(selected > 1 ? tr("Uninstall %1").arg(selected) : tr("Uninstall"));
    m_uninstallBtn->setEnabled(idle && selected > 0);
    m_installBtn->setDefault(idle && !m_installEdit->text().trimmed().isEmpty());
}

void PackagesDialog::setBusy(bool busy, const QString &text)
{
    m_busy = busy;
    m_bar->setVisible(busy);
    m_cancelBtn->setVisible(busy);
    if (!text.isEmpty())
        m_status->setText(text);
    updateButtons();
}

void PackagesDialog::appendLog(const QString &text)
{
    const QString t = text.trimmed();
    if (!t.isEmpty())
        m_log->appendPlainText(t);
}

void PackagesDialog::installTyped()
{
    if (m_busy || m_installEdit->text().trimmed().isEmpty())
        return;
    QString error;
    const QStringList specs = PythonTools::splitRequirements(m_installEdit->text(), &error);
    if (specs.isEmpty()) {
        m_status->setText(error);
        return;
    }
    m_pm->install(specs, false);
}

void PackagesDialog::installNow(const QStringList &specs)
{
    m_installEdit->setText(specs.join(QLatin1Char(' ')));
    if (m_pages->currentIndex() == 0 && !m_busy)
        installTyped();
}

void PackagesDialog::installFromFile()
{
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Install From File"), m_root,
        tr("Requirements and projects (requirements*.txt *.txt pyproject.toml setup.py);;All files (*)"));
    if (!file.isEmpty())
        m_pm->installFromFile(file);
}

void PackagesDialog::upgradeSelected()
{
    const QStringList names = selectedNames();
    if (!names.isEmpty() && !m_busy)
        m_pm->install(names, true);
}

void PackagesDialog::upgradeOutdated()
{
    const QStringList names = outdatedNames();
    if (!names.isEmpty() && !m_busy)
        m_pm->install(names, true);
}

void PackagesDialog::uninstallSelected()
{
    const QStringList names = selectedNames();
    if (names.isEmpty() || m_busy)
        return;
    QString list = names.mid(0, 8).join(QStringLiteral(", "));
    if (names.size() > 8)
        list += tr(" and %1 more").arg(names.size() - 8);
    const auto answer = QMessageBox::question(this, tr("Uninstall packages"),
                                              tr("Uninstall %1 from %2?\n\nOther packages that need them may stop working.").arg(list, m_venvCombo->currentData().toString()),
                                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer == QMessageBox::Yes)
        m_pm->uninstall(names);
}

void PackagesDialog::showContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item)
        return;
    if (!item->isSelected()) {
        m_tree->clearSelection();
        item->setSelected(true);
    }
    const QStringList names = selectedNames();
    QMenu menu(this);
    QAction *up = menu.addAction(names.size() > 1 ? tr("Upgrade %1 Packages").arg(names.size()) : tr("Upgrade"));
    QAction *rm = menu.addAction(names.size() > 1 ? tr("Uninstall %1 Packages…").arg(names.size()) : tr("Uninstall…"));
    up->setEnabled(!m_busy);
    rm->setEnabled(!m_busy);
    menu.addSeparator();
    QAction *copy = menu.addAction(tr("Copy Name"));
    QAction *open = menu.addAction(tr("Open on PyPI"));
    open->setEnabled(names.size() == 1);
    connect(up, &QAction::triggered, this, &PackagesDialog::upgradeSelected);
    connect(rm, &QAction::triggered, this, &PackagesDialog::uninstallSelected);
    connect(copy, &QAction::triggered, this, [names] { QApplication::clipboard()->setText(names.join(QLatin1Char('\n'))); });
    connect(open, &QAction::triggered, this,
            [names] { QDesktopServices::openUrl(QUrl(QStringLiteral("https://pypi.org/project/") + names.first())); });
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}
