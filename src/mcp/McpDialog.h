#pragma once

#include "mcp/McpConfig.h"

#include <QDialog>
#include <QPointer>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QToolButton;
class QTreeWidget;

namespace mcp {

class Client;
class Manager;

// MCP Servers (View menu, the chat header's MCP button, the /mcp card): the servers the QODE chat can use. A list on the
// left; on the right the selected server's settings, its live status and its tools. Add one by hand, import one from
// Claude Code, or paste the JSON snippet a server's README shows. "Test" starts the server as typed without saving.
class McpDialog : public QDialog
{
    Q_OBJECT
public:
    McpDialog(Manager *manager, QWidget *parent = nullptr);
    ~McpDialog() override;

    void selectServer(const QString &name);

protected:
    void reject() override; // asks before dropping unsaved changes

private:
    void buildUi();
    void applyTheme();
    void rebuildList();
    void showCurrent();      // the form for the selected row
    void readForm();         // the form into m_list
    void updateStatus();
    void updateButtons();
    void addServer(const ServerConfig &config, bool select = true);
    void addBlank();
    void removeCurrent();
    void testCurrent();
    void showLog();
    void pasteJson();
    void fillImportMenu();
    bool save();
    QString uniqueName(const QString &base) const;
    bool dirty() const;
    const Client *liveClient(const ServerConfig &config) const; // the running server the form describes, if any

    Manager *m_manager;
    QVector<ServerConfig> m_list;
    QVector<ServerConfig> m_saved;
    int m_row = -1;
    bool m_loading = false;
    Client *m_test = nullptr;  // "Test": the server as typed
    int m_testRow = -1;

    QLabel *m_title, *m_subtitle, *m_error;
    QToolButton *m_importBtn;
    QListWidget *m_servers;
    QPushButton *m_addBtn, *m_removeBtn;
    QStackedWidget *m_pages;
    QWidget *m_emptyPage, *m_formPage;
    QWidget *m_found;          // empty page: Claude Code's servers, one click each
    QLineEdit *m_name, *m_command, *m_args, *m_cwd;
    QPlainTextEdit *m_env;
    QRadioButton *m_project, *m_global;
    QCheckBox *m_enabled, *m_trusted;
    QLabel *m_status, *m_statusDetail;
    QPushButton *m_testBtn, *m_logBtn;
    QTreeWidget *m_tools;
    QPushButton *m_okBtn, *m_applyBtn;
};

} // namespace mcp
