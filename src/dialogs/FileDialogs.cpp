#include "dialogs/FileDialogs.h"

#include "platform/Platform.h"

namespace {

// Snapshot before the dialog, restore after it: the dialog writes its state in its destructor, inside the static call.
class StateGuard
{
public:
    StateGuard() : m_state(Platform::saveQtDialogState()) {}
    ~StateGuard() { Platform::restoreQtDialogState(m_state); }
    StateGuard(const StateGuard &) = delete;
    StateGuard &operator=(const StateGuard &) = delete;

private:
    QVariant m_state;
};

} // namespace

namespace FileDialogs {

QString getOpenFileName(QWidget *parent, const QString &caption, const QString &dir, const QString &filter,
                        QString *selectedFilter, QFileDialog::Options options)
{
    const StateGuard guard;
    return QFileDialog::getOpenFileName(parent, caption, dir, filter, selectedFilter, options);
}

QStringList getOpenFileNames(QWidget *parent, const QString &caption, const QString &dir, const QString &filter,
                             QString *selectedFilter, QFileDialog::Options options)
{
    const StateGuard guard;
    return QFileDialog::getOpenFileNames(parent, caption, dir, filter, selectedFilter, options);
}

QString getSaveFileName(QWidget *parent, const QString &caption, const QString &dir, const QString &filter,
                        QString *selectedFilter, QFileDialog::Options options)
{
    const StateGuard guard;
    return QFileDialog::getSaveFileName(parent, caption, dir, filter, selectedFilter, options);
}

QString getExistingDirectory(QWidget *parent, const QString &caption, const QString &dir, QFileDialog::Options options)
{
    const StateGuard guard;
    return QFileDialog::getExistingDirectory(parent, caption, dir, options);
}

} // namespace FileDialogs
