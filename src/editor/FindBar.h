#pragma once

#include <QFrame>

class CodeEditor;
class QLabel;
class QLineEdit;
class QToolButton;
class QPushButton;

// Compact find/replace strip shown above the editor tabs.
class FindBar : public QFrame
{
    Q_OBJECT
public:
    explicit FindBar(QWidget *parent = nullptr);

    void setEditor(CodeEditor *editor);
    void showFind();
    void showReplace();

private:
    void show_(bool replace);
    void onTextChanged();
    void updateCount();
    void next();
    void previous();
    void closeBar();

    bool eventFilter(QObject *obj, QEvent *ev) override;

    CodeEditor *m_editor = nullptr;
    QLineEdit *m_find;
    QLineEdit *m_replace;
    QLabel *m_count;
    QToolButton *m_case;
    QPushButton *m_replaceOne;
    QPushButton *m_replaceAll;
    QWidget *m_replaceRow;
};
