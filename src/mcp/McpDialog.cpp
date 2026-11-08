#include "McpDialog.h"

#include "dialogs/FileDialogs.h"
#include "mcp/McpClient.h"
#include "mcp/McpManager.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextCursor>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace mcp {

namespace {
Theme theme()
{
    return Theme::byName(SettingsManager::instance().theme());
}

QString envText(const QMap<QString, QString> &env)
{
    QStringList lines;
    for (auto it = env.cbegin(); it != env.cend(); ++it)
        lines << it.key() + QLatin1Char('=') + it.value();
    return lines.join(QLatin1Char('\n'));
}

QMap<QString, QString> parseEnv(const QString &text)
{
    QMap<QString, QString> env;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.startsWith(QLatin1String("export ")))
            line = line.mid(7).trimmed();
        const int eq = line.indexOf(QLatin1Char('='));
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || eq <= 0)
            continue;
        QString value = line.mid(eq + 1).trimmed();
        if (value.size() >= 2 && (value.startsWith(QLatin1Char('"')) || value.startsWith(QLatin1Char('\''))) && value.endsWith(value.at(0)))
            value = value.mid(1, value.size() - 2);
        env.insert(line.left(eq).trimmed(), value);
    }
    return env;
}

QString dot(const QColor &c)
{
    return QStringLiteral("<span style='color:%1'>●</span>").arg(c.name());
}

QLabel *hint(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("mcpHint"));
    l->setWordWrap(true);
    return l;
}
} // namespace

McpDialog::McpDialog(Manager *manager, QWidget *parent)
    : QDialog(parent)
    , m_manager(manager)
{
    setWindowTitle(tr("MCP Servers"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(SettingsManager::instance().scaled(900), SettingsManager::instance().scaled(700));
    setMinimumSize(720, 600); // below that the form's rows are squeezed into each other
    m_list = m_saved = manager->configs();
    buildUi();
    applyTheme();
    rebuildList();
    if (!m_list.isEmpty())
        m_servers->setCurrentRow(0);
    showCurrent();
    connect(m_manager, &Manager::changed, this, [this] {
        rebuildList();
        updateStatus();
    });
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, &McpDialog::applyTheme);
}

McpDialog::~McpDialog()
{
    delete m_test;
}

void McpDialog::selectServer(const QString &name)
{
    for (int i = 0; i < m_list.size(); ++i)
        if (m_list.at(i).name == name)
            m_servers->setCurrentRow(i);
}

void McpDialog::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(18, 16, 18, 14);
    outer->setSpacing(12);

    // --- header ---------------------------------------------------------------------------------------------------
    auto *head = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    m_title = new QLabel(tr("MCP Servers"), this);
    m_title->setObjectName(QStringLiteral("mcpTitle"));
    m_subtitle = hint(tr("Programs that give QODE more tools: Blender, databases, browsers, issue trackers… They run on this computer "
                         "and the QODE chat calls their tools when a request needs them."),
                      this);
    titles->addWidget(m_title);
    titles->addWidget(m_subtitle);
    head->addLayout(titles, 1);
    m_importBtn = new QToolButton(this);
    m_importBtn->setText(tr("Import"));
    m_importBtn->setToolTip(tr("Import a server from Claude Code, or paste a JSON snippet"));
    m_importBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_importBtn->setPopupMode(QToolButton::InstantPopup);
    auto *importMenu = new QMenu(m_importBtn);
    importMenu->setToolTipsVisible(true);
    m_importBtn->setMenu(importMenu);
    connect(importMenu, &QMenu::aboutToShow, this, &McpDialog::fillImportMenu);
    head->addWidget(m_importBtn, 0, Qt::AlignTop);
    outer->addLayout(head);

    // --- list | form ------------------------------------------------------------------------------------------------
    auto *split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    auto *left = new QWidget(split);
    auto *ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->setSpacing(8);
    m_servers = new QListWidget(left);
    m_servers->setObjectName(QStringLiteral("mcpList"));
    m_servers->setMinimumWidth(200);
    connect(m_servers, &QListWidget::currentRowChanged, this, [this](int) {
        if (!m_loading)
            showCurrent();
    });
    ll->addWidget(m_servers, 1);
    auto *lb = new QHBoxLayout;
    lb->setSpacing(6);
    m_addBtn = new QPushButton(tr("Add Server"), left);
    connect(m_addBtn, &QPushButton::clicked, this, &McpDialog::addBlank);
    m_removeBtn = new QPushButton(tr("Remove"), left);
    m_removeBtn->setObjectName(QStringLiteral("dangerBtn"));
    connect(m_removeBtn, &QPushButton::clicked, this, &McpDialog::removeCurrent);
    lb->addWidget(m_addBtn);
    lb->addWidget(m_removeBtn);
    lb->addStretch(1);
    ll->addLayout(lb);

    m_pages = new QStackedWidget(split);

    // Empty page: what MCP is, and the quickest ways to get a server.
    m_emptyPage = new QWidget(m_pages);
    auto *el = new QVBoxLayout(m_emptyPage);
    el->setContentsMargins(24, 12, 12, 12);
    el->setSpacing(10);
    auto *emptyTitle = new QLabel(tr("No MCP servers yet"), m_emptyPage);
    emptyTitle->setObjectName(QStringLiteral("mcpEmptyTitle"));
    el->addWidget(emptyTitle);
    el->addWidget(hint(tr("A server is a command that QODE starts, for example <b>uvx blender-mcp</b> or "
                          "<b>npx -y @modelcontextprotocol/server-github</b>. Its README shows the command, often as a JSON "
                          "snippet you can paste here."),
                       m_emptyPage));
    auto *eb = new QHBoxLayout;
    eb->setSpacing(8);
    auto *emptyAdd = new QPushButton(tr("Add Server"), m_emptyPage);
    emptyAdd->setObjectName(QStringLiteral("primaryBtn"));
    connect(emptyAdd, &QPushButton::clicked, this, &McpDialog::addBlank);
    auto *emptyPaste = new QPushButton(tr("Paste JSON…"), m_emptyPage);
    connect(emptyPaste, &QPushButton::clicked, this, &McpDialog::pasteJson);
    eb->addWidget(emptyAdd);
    eb->addWidget(emptyPaste);
    eb->addStretch(1);
    el->addLayout(eb);
    m_found = new QWidget(m_emptyPage);
    el->addWidget(m_found);
    el->addStretch(1);
    m_pages->addWidget(m_emptyPage);

    // Form page.
    m_formPage = new QWidget(m_pages);
    auto *fl = new QVBoxLayout(m_formPage);
    fl->setContentsMargins(16, 0, 0, 0);
    fl->setSpacing(10);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(8);
    m_name = new QLineEdit(m_formPage);
    m_name->setPlaceholderText(tr("blender"));
    m_name->setToolTip(tr("Letters, digits, - and _. The chat names the tools after it: mcp__<name>__<tool>."));
    form->addRow(tr("Name"), m_name);

    auto *scopeRow = new QHBoxLayout;
    scopeRow->setSpacing(16);
    m_project = new QRadioButton(tr("This project"), m_formPage);
    m_global = new QRadioButton(tr("All projects"), m_formPage);
    auto *scopes = new QButtonGroup(this);
    scopes->addButton(m_project);
    scopes->addButton(m_global);
    scopeRow->addWidget(m_project);
    scopeRow->addWidget(m_global);
    scopeRow->addStretch(1);
    form->addRow(tr("Use in"), scopeRow);

    auto *cmdRow = new QHBoxLayout;
    cmdRow->setSpacing(6);
    m_command = new QLineEdit(m_formPage);
    m_command->setPlaceholderText(tr("uvx, npx, python, or a full path"));
    auto *browse = new QPushButton(tr("Browse…"), m_formPage);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = FileDialogs::getOpenFileName(this, tr("Server Program"), QDir::homePath());
        if (!f.isEmpty())
            m_command->setText(f);
    });
    cmdRow->addWidget(m_command, 1);
    cmdRow->addWidget(browse);
    form->addRow(tr("Command"), cmdRow);
    // A whole command line typed or pasted into Command is split into the program and its arguments.
    connect(m_command, &QLineEdit::editingFinished, this, [this] {
        const QStringList parts = QProcess::splitCommand(m_command->text().trimmed());
        if (parts.size() > 1 && m_args->text().trimmed().isEmpty() && !QFileInfo::exists(m_command->text().trimmed())) {
            m_command->setText(parts.first());
            m_args->setText(joinArgs(parts.mid(1)));
        }
    });

    m_args = new QLineEdit(m_formPage);
    m_args->setPlaceholderText(tr("blender-mcp   (quote arguments with spaces: \"my folder\")"));
    form->addRow(tr("Arguments"), m_args);

    m_env = new QPlainTextEdit(m_formPage);
    m_env->setPlaceholderText(tr("API_TOKEN=…   one variable per line; ${HOME} and ${VAR:-default} are expanded.\n"
                                 "Stored only on this computer (mode 0600), never shown to the model."));
    m_env->setFixedHeight(SettingsManager::instance().scaled(64));
    m_env->setTabChangesFocus(true);
    form->addRow(tr("Environment"), m_env);

    auto *cwdRow = new QHBoxLayout;
    cwdRow->setSpacing(6);
    m_cwd = new QLineEdit(m_formPage);
    m_cwd->setPlaceholderText(tr("the project folder"));
    auto *cwdBrowse = new QPushButton(tr("Browse…"), m_formPage);
    connect(cwdBrowse, &QPushButton::clicked, this, [this] {
        const QString d = FileDialogs::getExistingDirectory(this, tr("Working Folder"), m_cwd->text().isEmpty() ? QDir::homePath() : m_cwd->text());
        if (!d.isEmpty())
            m_cwd->setText(d);
    });
    cwdRow->addWidget(m_cwd, 1);
    cwdRow->addWidget(cwdBrowse);
    form->addRow(tr("Run in"), cwdRow);

    m_enabled = new QCheckBox(tr("Enabled"), m_formPage);
    m_enabled->setToolTip(tr("Start this server and give its tools to the QODE chat"));
    m_trusted = new QCheckBox(tr("Run its tools without asking"), m_formPage);
    m_trusted->setToolTip(tr("Otherwise QODE asks before each tool call (Allow once / Allow for this chat). "
                             "Tools the server marks as read-only never ask."));
    auto *checks = new QHBoxLayout;
    checks->setSpacing(18);
    checks->addWidget(m_enabled);
    checks->addWidget(m_trusted);
    checks->addStretch(1);
    form->addRow(QString(), checks);
    fl->addLayout(form);

    // Status and tools.
    auto *statusBox = new QFrame(m_formPage);
    statusBox->setObjectName(QStringLiteral("mcpStatusBox"));
    auto *sl = new QVBoxLayout(statusBox);
    sl->setContentsMargins(12, 10, 12, 10);
    sl->setSpacing(6);
    auto *sh = new QHBoxLayout;
    sh->setSpacing(8);
    m_status = new QLabel(statusBox);
    m_status->setObjectName(QStringLiteral("mcpStatus"));
    m_status->setTextFormat(Qt::RichText);
    sh->addWidget(m_status, 1);
    m_logBtn = new QPushButton(tr("Log"), statusBox);
    m_logBtn->setToolTip(tr("What the server wrote to its error output"));
    connect(m_logBtn, &QPushButton::clicked, this, &McpDialog::showLog);
    m_testBtn = new QPushButton(tr("Test Connection"), statusBox);
    m_testBtn->setToolTip(tr("Start the server as typed above and list its tools (nothing is saved)"));
    connect(m_testBtn, &QPushButton::clicked, this, &McpDialog::testCurrent);
    sh->addWidget(m_logBtn);
    sh->addWidget(m_testBtn);
    sl->addLayout(sh);
    m_statusDetail = hint(QString(), statusBox);
    m_statusDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_statusDetail->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    sl->addWidget(m_statusDetail);
    m_tools = new QTreeWidget(statusBox);
    m_tools->setObjectName(QStringLiteral("mcpTools"));
    m_tools->setColumnCount(2);
    m_tools->setHeaderLabels({tr("Tool"), tr("What it does")});
    m_tools->setRootIsDecorated(false);
    m_tools->setUniformRowHeights(true);
    m_tools->setSelectionMode(QAbstractItemView::NoSelection);
    m_tools->header()->setStretchLastSection(true);
    m_tools->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_tools->setMinimumHeight(80);
    sl->addWidget(m_tools, 1);
    fl->addWidget(statusBox, 1);
    m_pages->addWidget(m_formPage);

    split->addWidget(left);
    split->addWidget(m_pages);
    split->setStretchFactor(1, 1);
    split->setSizes({220, 640});
    outer->addWidget(split, 1);

    // --- bottom -----------------------------------------------------------------------------------------------------
    auto *bottom = new QHBoxLayout;
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("mcpError"));
    m_error->setWordWrap(true);
    bottom->addWidget(m_error, 1);
    auto *buttons = new QDialogButtonBox(this);
    m_okBtn = buttons->addButton(tr("Save"), QDialogButtonBox::AcceptRole);
    m_okBtn->setObjectName(QStringLiteral("primaryBtn"));
    m_okBtn->setToolTip(tr("Save and close; changed servers restart"));
    m_applyBtn = buttons->addButton(tr("Apply"), QDialogButtonBox::ApplyRole);
    m_applyBtn->setToolTip(tr("Save and keep the dialog open to watch the servers connect"));
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(m_okBtn, &QPushButton::clicked, this, [this] {
        if (save())
            accept();
    });
    connect(m_applyBtn, &QPushButton::clicked, this, &McpDialog::save);
    connect(buttons, &QDialogButtonBox::rejected, this, &McpDialog::reject);
    bottom->addWidget(buttons);
    outer->addLayout(bottom);

    // Every edit goes into the list right away (the list row and the buttons follow).
    auto edited = [this] {
        if (m_loading)
            return;
        readForm();
        rebuildList();
        updateStatus();
        updateButtons();
    };
    for (QLineEdit *e : {m_name, m_command, m_args, m_cwd})
        connect(e, &QLineEdit::textChanged, this, edited);
    connect(m_env, &QPlainTextEdit::textChanged, this, edited);
    for (QAbstractButton *b : std::initializer_list<QAbstractButton *>{m_project, m_global, m_enabled, m_trusted})
        connect(b, &QAbstractButton::toggled, this, edited);
}

void McpDialog::applyTheme()
{
    const Theme t = theme();
    setStyleSheet(QStringLiteral(R"(
QLabel#mcpTitle { font-size: 14pt; font-weight: 700; }
QLabel#mcpEmptyTitle { font-size: 13pt; font-weight: 600; }
QLabel#mcpHint { color: %1; }
QLabel#mcpError { color: %4; }
QListWidget#mcpList { background: %2; border: 1px solid %3; border-radius: 8px; padding: 4px; }
QListWidget#mcpList::item { padding: 6px 6px; border-radius: 6px; }
QListWidget#mcpList::item:selected { background: %5; color: palette(text); }
QFrame#mcpStatusBox { background: %2; border: 1px solid %3; border-radius: 8px; }
QFrame#mcpStatusBox QLabel { background: transparent; }
QTreeWidget#mcpTools { background: transparent; border: none; }
QTreeWidget#mcpTools::item { padding: 3px 4px; }
QTreeWidget#mcpTools QHeaderView::section { background: transparent; color: %1; border: none; border-bottom: 1px solid %3; padding: 4px 6px; font-size: 8pt; font-weight: 600; }
QPushButton#dangerBtn { color: %4; }
QPushButton#dangerBtn:disabled { color: %1; }
)")
                      .arg(t.textMuted.name(), t.panel.name(), t.border.name(), t.danger.name(), t.selection.name()));
    m_importBtn->setIcon(Icons::tinted(QStringLiteral(":/new-icons/download.svg"), t.textMuted));
    rebuildList();
    updateStatus();
}

const Client *McpDialog::liveClient(const ServerConfig &config) const
{
    const Client *c = m_manager->client(config.name);
    return c && c->config().sameLaunch(config) ? c : nullptr;
}

void McpDialog::rebuildList()
{
    const Theme t = theme();
    m_loading = true;
    const int row = m_servers->currentRow();
    m_servers->clear();
    for (int i = 0; i < m_list.size(); ++i) {
        const ServerConfig &s = m_list.at(i);
        QColor c = t.textMuted;
        QString state = tr("not saved yet");
        if (const Client *live = liveClient(s); live && s.enabled) {
            switch (live->state()) {
            case Client::State::Connected:
                c = t.success;
                state = live->tools().size() == 1 ? tr("1 tool") : tr("%1 tools").arg(live->tools().size());
                break;
            case Client::State::Starting:
                c = t.warning;
                state = tr("connecting…");
                break;
            case Client::State::Failed:
                c = t.danger;
                state = tr("failed");
                break;
            case Client::State::Stopped:
                state = tr("stopped");
                break;
            }
        } else if (!s.enabled) {
            state = tr("disabled");
        }
        QPixmap pm(10, 10);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawEllipse(QRectF(1, 1, 8, 8));
        }
        auto *item = new QListWidgetItem(QIcon(pm), QStringLiteral("%1\n%2 · %3").arg(s.name.isEmpty() ? tr("(unnamed)") : s.name,
                                                                                     s.global ? tr("all projects") : tr("this project"), state),
                                         m_servers);
        item->setToolTip(s.commandLine());
    }
    m_servers->setCurrentRow(qMin(row, int(m_list.size()) - 1));
    m_loading = false;
    m_removeBtn->setEnabled(m_servers->currentRow() >= 0);
}

void McpDialog::showCurrent()
{
    m_row = m_servers->currentRow();
    m_removeBtn->setEnabled(m_row >= 0);
    if (m_row < 0 || m_row >= m_list.size()) {
        // The empty page lists what Claude Code has, one click to add each.
        delete m_found->layout();
        qDeleteAll(m_found->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly));
        auto *l = new QVBoxLayout(m_found);
        l->setContentsMargins(0, 12, 0, 0);
        l->setSpacing(6);
        int shown = 0;
        for (const Found &f : claudeCodeServers(m_manager->projectRoot())) {
            if (f.type != QLatin1String("stdio") || shown >= 8)
                continue;
            if (!shown++) {
                auto *h = new QLabel(tr("Found in Claude Code"), m_found);
                h->setObjectName(QStringLiteral("mcpEmptyTitle"));
                l->addWidget(h);
            }
            auto *row = new QHBoxLayout;
            auto *text = new QLabel(QStringLiteral("<b>%1</b> &nbsp;<span>%2</span><br><small>%3</small>")
                                        .arg(f.config.name.toHtmlEscaped(), f.config.commandLine().toHtmlEscaped(), f.where.toHtmlEscaped()),
                                    m_found);
            text->setTextFormat(Qt::RichText);
            auto *add = new QPushButton(tr("Add"), m_found);
            const Found copy = f;
            connect(add, &QPushButton::clicked, this, [this, copy] {
                ServerConfig c = copy.config;
                c.global = copy.where == tr("user settings") || !m_manager->hasProject();
                addServer(c);
            });
            row->addWidget(text, 1);
            row->addWidget(add);
            l->addLayout(row);
        }
        m_pages->setCurrentWidget(m_emptyPage);
        updateButtons();
        return;
    }
    m_pages->setCurrentWidget(m_formPage);
    const ServerConfig &s = m_list.at(m_row);
    m_loading = true;
    m_name->setText(s.name);
    m_project->setEnabled(m_manager->hasProject());
    (s.global || !m_manager->hasProject() ? m_global : m_project)->setChecked(true);
    m_command->setText(s.command);
    m_args->setText(joinArgs(s.args));
    m_env->setPlainText(envText(s.env));
    m_cwd->setText(s.cwd);
    m_enabled->setChecked(s.enabled);
    m_trusted->setChecked(s.trusted);
    m_loading = false;
    updateStatus();
    updateButtons();
}

void McpDialog::readForm()
{
    if (m_row < 0 || m_row >= m_list.size())
        return;
    ServerConfig &s = m_list[m_row];
    s.name = m_name->text().trimmed();
    s.global = m_global->isChecked();
    s.command = m_command->text().trimmed();
    s.args = QProcess::splitCommand(m_args->text());
    s.env = parseEnv(m_env->toPlainText());
    s.cwd = m_cwd->text().trimmed();
    s.enabled = m_enabled->isChecked();
    s.trusted = m_trusted->isChecked();
}

void McpDialog::updateStatus()
{
    if (m_row < 0 || m_row >= m_list.size())
        return;
    const Theme t = theme();
    const ServerConfig &s = m_list.at(m_row);
    const Client *c = m_test && m_testRow == m_row ? m_test : liveClient(s);
    const bool testing = c && c == m_test;
    QString head, detail;
    if (!c) {
        const Client *running = m_manager->client(s.name);
        head = dot(t.textMuted) + QLatin1Char(' ') + (s.enabled ? tr("Not running") : tr("Disabled"));
        detail = running ? tr("The settings changed: Save to restart the server, or test them first.")
                 : s.enabled ? tr("Save to start it, or test it first.")
                             : tr("The chat does not use this server. Test still works.");
    } else {
        switch (c->state()) {
        case Client::State::Connected:
            head = dot(t.success) + QLatin1Char(' ') + tr("Connected");
            if (!c->serverTitle().isEmpty())
                head += QStringLiteral(" · ") + c->serverTitle().toHtmlEscaped();
            head += QStringLiteral(" · ") + (c->tools().size() == 1 ? tr("1 tool") : tr("%1 tools").arg(c->tools().size()));
            detail = testing ? tr("Test run: the server works. Save to use it in the chat.") : QString();
            break;
        case Client::State::Starting:
            head = dot(t.warning) + QLatin1Char(' ') + tr("Connecting…");
            detail = tr("The first start can take a while when uvx or npx downloads the server.");
            break;
        case Client::State::Failed:
            head = dot(t.danger) + QLatin1Char(' ') + tr("Failed");
            detail = c->error();
            break;
        case Client::State::Stopped:
            head = dot(t.textMuted) + QLatin1Char(' ') + tr("Stopped");
            break;
        }
    }
    m_status->setText(head);
    m_statusDetail->setText(detail);
    m_statusDetail->setVisible(!detail.isEmpty());
    m_logBtn->setEnabled(c && !c->log().trimmed().isEmpty());
    m_testBtn->setEnabled(!s.command.isEmpty() && !(testing && c->state() == Client::State::Starting));

    m_tools->clear();
    if (c && c->state() == Client::State::Connected) {
        for (const QJsonValue &v : c->tools()) {
            const QJsonObject tool = v.toObject();
            QString desc = tool.value(QStringLiteral("description")).toString().simplified();
            auto *item = new QTreeWidgetItem(m_tools, {tool.value(QStringLiteral("name")).toString(), desc.left(160)});
            item->setToolTip(1, desc.left(1500));
            if (tool.value(QStringLiteral("annotations")).toObject().value(QStringLiteral("readOnlyHint")).toBool())
                item->setToolTip(0, tr("Read-only: runs without asking"));
        }
    }
    m_tools->setVisible(m_tools->topLevelItemCount() > 0);
}

void McpDialog::updateButtons()
{
    QString problem;
    QStringList names;
    for (const ServerConfig &s : std::as_const(m_list)) {
        if (!validName(s.name))
            problem = s.name.isEmpty() ? tr("Every server needs a name.")
                                       : tr("“%1”: a name may only have letters, digits, - and _.").arg(s.name);
        else if (names.contains(s.name))
            problem = tr("Two servers are called “%1”.").arg(s.name);
        else if (s.command.isEmpty())
            problem = tr("“%1” has no command.").arg(s.name);
        names << s.name;
        if (!problem.isEmpty())
            break;
    }
    m_error->setText(problem);
    m_okBtn->setEnabled(problem.isEmpty());
    m_applyBtn->setEnabled(problem.isEmpty() && dirty());
}

bool McpDialog::dirty() const
{
    if (m_list.size() != m_saved.size())
        return true;
    for (int i = 0; i < m_list.size(); ++i) {
        const ServerConfig &a = m_list.at(i), &b = m_saved.at(i);
        if (a.name != b.name || a.global != b.global || !a.sameLaunch(b) || a.enabled != b.enabled || a.trusted != b.trusted)
            return true;
    }
    return false;
}

QString McpDialog::uniqueName(const QString &base) const
{
    QString clean = base;
    clean.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("-"));
    if (clean.isEmpty())
        clean = QStringLiteral("server");
    QString name = clean.left(36);
    for (int n = 2;; ++n) {
        bool taken = false;
        for (const ServerConfig &s : m_list)
            taken = taken || s.name == name;
        if (!taken)
            return name;
        name = QStringLiteral("%1-%2").arg(clean.left(33)).arg(n);
    }
}

void McpDialog::addServer(const ServerConfig &config, bool select)
{
    ServerConfig c = config;
    c.name = uniqueName(c.name.isEmpty() ? QFileInfo(c.command).baseName() : c.name);
    if (!m_manager->hasProject())
        c.global = true;
    m_list << c;
    rebuildList();
    if (select)
        m_servers->setCurrentRow(int(m_list.size()) - 1);
    showCurrent();
}

void McpDialog::addBlank()
{
    ServerConfig c;
    c.name = uniqueName(QStringLiteral("server"));
    addServer(c);
    m_name->setFocus();
    m_name->selectAll();
}

void McpDialog::removeCurrent()
{
    if (m_row < 0 || m_row >= m_list.size())
        return;
    if (m_testRow == m_row) {
        delete m_test;
        m_test = nullptr;
        m_testRow = -1;
    } else if (m_testRow > m_row) {
        --m_testRow;
    }
    m_list.removeAt(m_row);
    rebuildList();
    showCurrent();
}

void McpDialog::testCurrent()
{
    if (m_row < 0 || m_row >= m_list.size())
        return;
    delete m_test;
    m_test = new Client(m_list.at(m_row), m_manager->projectRoot(), this);
    m_testRow = m_row;
    connect(m_test, &Client::changed, this, &McpDialog::updateStatus);
    m_test->start();
    updateStatus();
}

void McpDialog::showLog()
{
    if (m_row < 0 || m_row >= m_list.size())
        return;
    const Client *c = m_test && m_testRow == m_row ? m_test : liveClient(m_list.at(m_row));
    if (!c)
        return;
    QDialog d(this);
    d.setWindowTitle(tr("%1: server log").arg(m_list.at(m_row).name));
    d.resize(720, 420);
    auto *l = new QVBoxLayout(&d);
    auto *text = new QPlainTextEdit(&d);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setPlainText(c->log());
    text->moveCursor(QTextCursor::End);
    l->addWidget(text);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Close, &d);
    connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    l->addWidget(bb);
    d.exec();
}

void McpDialog::pasteJson()
{
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(this, tr("Paste JSON"),
                                                        tr("The configuration from the server's README, for example:\n"
                                                           "{ \"mcpServers\": { \"blender\": { \"command\": \"uvx\", \"args\": [\"blender-mcp\"] } } }"),
                                                        QString(), &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    QString error;
    const QVector<ServerConfig> servers = parseSnippet(text, &error);
    if (servers.isEmpty()) {
        QMessageBox::warning(this, tr("Paste JSON"), error);
        return;
    }
    for (const ServerConfig &s : servers)
        addServer(s);
    if (!error.isEmpty())
        m_error->setText(error);
}

void McpDialog::fillImportMenu()
{
    QMenu *menu = m_importBtn->menu();
    menu->clear();
    menu->addAction(tr("Paste JSON…"), this, &McpDialog::pasteJson);
    const QVector<Found> found = claudeCodeServers(m_manager->projectRoot());
    menu->addSeparator();
    menu->addAction(tr("From Claude Code:"))->setEnabled(false);
    if (found.isEmpty())
        menu->addAction(tr("No servers in Claude Code's settings"))->setEnabled(false);
    for (const Found &f : found) {
        QAction *a = menu->addAction(QStringLiteral("%1  —  %2").arg(f.config.name, f.where));
        if (f.type != QLatin1String("stdio")) {
            a->setEnabled(false);
            a->setText(a->text() + tr("  (%1: not supported)").arg(f.type));
            continue;
        }
        a->setToolTip(f.config.commandLine());
        const Found copy = f;
        connect(a, &QAction::triggered, this, [this, copy] {
            ServerConfig c = copy.config;
            c.global = copy.where == tr("user settings") || !m_manager->hasProject();
            addServer(c);
        });
    }
}

bool McpDialog::save()
{
    readForm();
    updateButtons();
    if (!m_okBtn->isEnabled())
        return false;
    QString error;
    if (!m_manager->apply(m_list, &error)) {
        m_error->setText(error);
        return false;
    }
    m_saved = m_list;
    if (m_test) { // the saved server takes over
        delete m_test;
        m_test = nullptr;
        m_testRow = -1;
    }
    rebuildList();
    updateStatus();
    updateButtons();
    return true;
}

void McpDialog::reject()
{
    readForm();
    if (dirty()) {
        const auto answer = QMessageBox::question(this, tr("MCP Servers"), tr("Save the changes to the MCP servers?"),
                                                  QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel)
            return;
        if (answer == QMessageBox::Save && !save())
            return;
    }
    QDialog::reject();
}

} // namespace mcp
