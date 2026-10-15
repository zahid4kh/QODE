#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QStackedWidget;
class QTimer;
class HttpClientView;
class QToolButton;
class SpringBar;
class SpringLogView;

// Right-hand panel of a Spring Boot project: the application's state (starting, running on which port, uptime,
// startup time), its live log with level / text filters, and the Requests tab (HTTP client with the endpoints found in
// the controllers). Start, stop and restart are the bar's; the panel only
// observes the server the bar owns.
class SpringPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SpringPanel(QWidget *parent = nullptr);

    void attach(SpringBar *bar);
    SpringLogView *log() const { return m_log; }

signals:
    void closeRequested();
    void locationRequested(const QString &path, int line, int column);

private:
    void refresh();
    void onStateChanged();
    void updateCounts();
    void repaintIcons();
    void loadRequests();
    void showPage(int page);
    void scanEndpoints();

    SpringBar *m_bar = nullptr;
    int m_lastState = -1;
    QLabel *m_logo, *m_title, *m_dot, *m_status, *m_detail, *m_count;
    QToolButton *m_toggle, *m_restart, *m_config, *m_closeBtn;
    QComboBox *m_level;
    QLineEdit *m_filter;
    QToolButton *m_build, *m_pause, *m_clear, *m_copy, *m_end;
    SpringLogView *m_log;
    HttpClientView *m_http;
    QStackedWidget *m_stack;
    QToolButton *m_tabLog, *m_tabRequests;
    bool m_endpointsStale = true;
    QTimer *m_uptime, *m_filterTimer;
};
