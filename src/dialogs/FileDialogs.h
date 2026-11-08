#pragma once

#include <QFileDialog>

// QFileDialog's static functions, for every file / folder picker in QODE (never call QFileDialog:: directly). Same
// signatures and behaviour; each call also leaves Qt's shared dialog state where it was: on Windows Qt writes it to
// HKCU\Software\QtProject whenever a dialog closes (see Platform::saveQtDialogState), so QODE never changes the
// registry; on Unix it already goes to QODE's config folder (AppPaths::initQtSettings).
namespace FileDialogs {

QString getOpenFileName(QWidget *parent = nullptr, const QString &caption = {}, const QString &dir = {},
                        const QString &filter = {}, QString *selectedFilter = nullptr, QFileDialog::Options options = {});
QStringList getOpenFileNames(QWidget *parent = nullptr, const QString &caption = {}, const QString &dir = {},
                             const QString &filter = {}, QString *selectedFilter = nullptr, QFileDialog::Options options = {});
QString getSaveFileName(QWidget *parent = nullptr, const QString &caption = {}, const QString &dir = {},
                        const QString &filter = {}, QString *selectedFilter = nullptr, QFileDialog::Options options = {});
QString getExistingDirectory(QWidget *parent = nullptr, const QString &caption = {}, const QString &dir = {},
                             QFileDialog::Options options = QFileDialog::ShowDirsOnly);

} // namespace FileDialogs
