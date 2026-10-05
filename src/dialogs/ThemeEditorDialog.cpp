#include "ThemeEditorDialog.h"

#include "settings/SettingsManager.h"
#include "settings/ThemeManager.h"

#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>

namespace {
QString swatchStyle(const QColor &c)
{
    return QStringLiteral("QPushButton { background: %1; border: 1px solid rgba(128,128,128,160); border-radius: 4px; }")
        .arg(c.name(QColor::HexArgb));
}
QString colorText(const QColor &c)
{
    return c.name(c.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb);
}
} // namespace

ThemeEditorDialog::ThemeEditorDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Theme Editor"));
    setModal(false);
    resize(460, 720);

    m_themes = new QComboBox(this);
    m_themes->setMinimumWidth(150);
    m_base = new QComboBox(this);
    m_base->addItem(tr("Dark"), true);
    m_base->addItem(tr("Light"), false);
    m_base->setToolTip(tr("Dark or light base: picks icon variants and a few contrast tweaks"));
    m_delete = new QPushButton(tr("Delete"), this);
    auto *folder = new QPushButton(tr("Open Folder"), this);
    folder->setToolTip(tr("Open the folder holding your theme JSON files"));

    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Theme:"), this));
    top->addWidget(m_themes, 1);
    top->addWidget(new QLabel(tr("Base:"), this));
    top->addWidget(m_base);

    auto *top2 = new QHBoxLayout;
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(tr("Filter colours…"));
    m_filter->setClearButtonEnabled(true);
    top2->addWidget(m_filter, 1);
    top2->addWidget(m_delete);
    top2->addWidget(folder);

    m_rowHost = new QWidget;
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(m_rowHost);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("emptyText"));
    m_status->setWordWrap(true);

    m_revert = new QPushButton(tr("Revert"), this);
    m_saveAs = new QPushButton(tr("Save As…"), this);
    m_save = new QPushButton(tr("Save"), this);
    m_save->setDefault(true);
    auto *close = new QPushButton(tr("Close"), this);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_revert);
    bottom->addStretch(1);
    bottom->addWidget(m_saveAs);
    bottom->addWidget(m_save);
    bottom->addWidget(close);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addLayout(top2);
    layout->addWidget(scroll, 1);
    layout->addWidget(m_status);
    layout->addLayout(bottom);

    connect(m_themes, &QComboBox::activated, this, [this] {
        const QString id = m_themes->currentData().toString();
        if (id == m_id)
            return;
        if (!confirmDiscard()) {
            m_themes->setCurrentIndex(m_themes->findData(m_id));
            return;
        }
        load(id);
    });
    connect(m_base, &QComboBox::activated, this, [this] {
        if (m_loading)
            return;
        m_working.dark = m_base->currentData().toBool();
        touched();
    });
    connect(m_filter, &QLineEdit::textChanged, this, &ThemeEditorDialog::applyFilter);
    connect(m_save, &QPushButton::clicked, this, [this] { save(false); });
    connect(m_saveAs, &QPushButton::clicked, this, [this] { save(true); });
    connect(m_delete, &QPushButton::clicked, this, &ThemeEditorDialog::removeCurrent);
    connect(m_revert, &QPushButton::clicked, this, [this] { load(m_id); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(folder, &QPushButton::clicked, this, [] {
        QDir().mkpath(ThemeManager::instance().themesDir());
        QDesktopServices::openUrl(QUrl::fromLocalFile(ThemeManager::instance().themesDir()));
    });
    connect(&ThemeManager::instance(), &ThemeManager::listChanged, this, [this] {
        if (!m_dirty)
            rebuildThemeList(m_id);
    });

    buildRows();
    rebuildThemeList(SettingsManager::instance().theme());
    load(m_themes->currentData().toString());
}

void ThemeEditorDialog::rebuildThemeList(const QString &select)
{
    const QSignalBlocker block(m_themes);
    m_themes->clear();
    for (const ThemeManager::Info &i : ThemeManager::instance().themes())
        m_themes->addItem(i.builtin ? i.name + tr(" (built-in)") : i.name, i.id);
    int idx = m_themes->findData(select);
    m_themes->setCurrentIndex(idx < 0 ? 0 : idx);
}

void ThemeEditorDialog::buildRows()
{
    auto *grid = new QGridLayout(m_rowHost);
    grid->setColumnStretch(0, 1);
    int r = 0;
    QString group;
    const auto &fields = Theme::fields();
    for (int i = 0; i < fields.size(); ++i) {
        const Theme::Field &f = fields[i];
        const QString g = tr(f.group);
        if (g != group) {
            group = g;
            auto *h = new QLabel(QStringLiteral("<b>%1</b>").arg(g.toHtmlEscaped()), m_rowHost);
            h->setContentsMargins(0, r ? 12 : 0, 0, 2);
            grid->addWidget(h, r++, 0, 1, 3);
            m_headers.append(h);
            m_headerGroup.insert(h, g);
        }
        Row row;
        row.field = i;
        row.label = new QLabel(tr(f.label), m_rowHost);
        row.swatch = new QPushButton(m_rowHost);
        row.swatch->setFixedSize(34, 22);
        row.hex = new QLineEdit(m_rowHost);
        row.hex->setFixedWidth(92);
        row.hex->setMaxLength(9);
        grid->addWidget(row.label, r, 0);
        grid->addWidget(row.swatch, r, 1);
        grid->addWidget(row.hex, r, 2);
        ++r;
        const int idx = m_rows.size();
        connect(row.swatch, &QPushButton::clicked, this, [this, idx] {
            Theme copy = m_working;
            const QColor start = Theme::fields()[m_rows[idx].field].ref(copy);
            // Live while the picker is open; Cancel puts the old colour back.
            QColorDialog dlg(start, this);
            dlg.setOption(QColorDialog::ShowAlphaChannel);
            dlg.setWindowTitle(tr("Choose Colour"));
            connect(&dlg, &QColorDialog::currentColorChanged, this, [this, idx](const QColor &c) { setColor(idx, c); });
            if (dlg.exec() != QDialog::Accepted)
                setColor(idx, start);
        });
        connect(row.hex, &QLineEdit::textEdited, this, [this, idx](const QString &text) {
            static const QRegularExpression re(QStringLiteral("^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$"));
            if (re.match(text.trimmed()).hasMatch())
                setColor(idx, QColor(text.trimmed()));
        });
        connect(row.hex, &QLineEdit::editingFinished, this, [this, idx] { syncRow(idx); });
        m_rows.append(row);
    }
    grid->setRowStretch(r, 1);
}

void ThemeEditorDialog::syncRow(int idx)
{
    Row &row = m_rows[idx];
    const QColor c = Theme::fields()[row.field].ref(m_working);
    row.swatch->setStyleSheet(swatchStyle(c));
    row.swatch->setToolTip(colorText(c));
    if (!row.hex->hasFocus())
        row.hex->setText(colorText(c));
}

void ThemeEditorDialog::setColor(int idx, const QColor &c)
{
    if (!c.isValid())
        return;
    QColor &ref = Theme::fields()[m_rows[idx].field].ref(m_working);
    if (ref == c)
        return;
    ref = c;
    syncRow(idx);
    touched();
}

void ThemeEditorDialog::touched()
{
    if (m_loading)
        return;
    m_dirty = true;
    ThemeManager::instance().setPreview(m_working);
    updateButtons();
}

void ThemeEditorDialog::load(const QString &id)
{
    m_loading = true;
    m_id = id;
    // theme() answers with the preview while one runs, so end it first.
    ThemeManager::instance().clearPreview();
    m_working = ThemeManager::instance().theme(id);
    m_builtin = (id == QLatin1String("dark") || id == QLatin1String("light"));
    m_dirty = false;
    m_themes->setCurrentIndex(m_themes->findData(id));
    m_base->setCurrentIndex(m_working.dark ? 0 : 1);
    for (int i = 0; i < m_rows.size(); ++i) {
        m_rows[i].hex->clearFocus();
        syncRow(i);
    }
    m_loading = false;
    // Looking at a theme other than the active one previews it until the dialog closes.
    if (id != SettingsManager::instance().theme())
        ThemeManager::instance().setPreview(m_working);
    updateButtons();
}

void ThemeEditorDialog::updateButtons()
{
    m_save->setText(m_builtin ? tr("Save as New…") : tr("Save"));
    m_save->setEnabled(m_dirty || m_builtin);
    m_save->setVisible(true);
    m_saveAs->setVisible(!m_builtin);
    m_delete->setEnabled(!m_builtin);
    m_revert->setEnabled(m_dirty);
    m_status->setText(m_builtin ? tr("Built-in themes are read-only: changes are saved as a new theme.")
                                : tr("Saved in %1/%2.json").arg(ThemeManager::instance().themesDir(), m_id));
    if (m_dirty)
        m_status->setText(m_status->text() + tr("  Unsaved changes."));
}

void ThemeEditorDialog::applyFilter(const QString &text)
{
    const QString needle = text.trimmed();
    QHash<QString, bool> groupHasMatch;
    for (Row &row : m_rows) {
        const Theme::Field &f = Theme::fields()[row.field];
        const QString group = tr(f.group);
        const bool match = needle.isEmpty() || row.label->text().contains(needle, Qt::CaseInsensitive)
                           || group.contains(needle, Qt::CaseInsensitive) || QLatin1String(f.key).contains(needle, Qt::CaseInsensitive);
        row.label->setVisible(match);
        row.swatch->setVisible(match);
        row.hex->setVisible(match);
        if (match)
            groupHasMatch[group] = true;
    }
    for (QWidget *h : m_headers)
        h->setVisible(groupHasMatch.value(m_headerGroup.value(h)));
}

void ThemeEditorDialog::save(bool asNew)
{
    QString id = m_id;
    Theme t = m_working;
    if (m_builtin || asNew) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save Theme"), tr("Name for the new theme:"), QLineEdit::Normal,
                                                   t.name + tr(" Copy"), &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        t.name = name;
        id = ThemeManager::instance().uniqueId(name);
    }
    ThemeManager::instance().clearPreview();
    const QString saved = ThemeManager::instance().save(t, id);
    if (saved.isEmpty()) {
        QMessageBox::warning(this, tr("Save Theme"), tr("Could not write %1.").arg(ThemeManager::instance().themesDir()));
        if (m_dirty)
            ThemeManager::instance().setPreview(m_working);
        return;
    }
    m_dirty = false;
    SettingsManager::instance().setTheme(saved);
    rebuildThemeList(saved);
    load(saved);
}

void ThemeEditorDialog::removeCurrent()
{
    if (m_builtin)
        return;
    if (QMessageBox::question(this, tr("Delete Theme"), tr("Delete the theme \"%1\"?").arg(m_working.name)) != QMessageBox::Yes)
        return;
    const QString gone = m_id;
    m_dirty = false;
    ThemeManager::instance().clearPreview();
    const bool wasActive = SettingsManager::instance().theme() == gone;
    if (wasActive)
        SettingsManager::instance().setTheme(m_working.dark ? QStringLiteral("dark") : QStringLiteral("light"));
    ThemeManager::instance().remove(gone);
    rebuildThemeList(SettingsManager::instance().theme());
    load(m_themes->currentData().toString());
}

bool ThemeEditorDialog::confirmDiscard()
{
    if (!m_dirty)
        return true;
    return QMessageBox::question(this, tr("Unsaved Changes"), tr("Discard the unsaved changes to \"%1\"?").arg(m_working.name)) == QMessageBox::Yes;
}

void ThemeEditorDialog::finish()
{
    ThemeManager::instance().clearPreview();
}

void ThemeEditorDialog::closeEvent(QCloseEvent *e)
{
    if (!confirmDiscard()) {
        e->ignore();
        return;
    }
    m_dirty = false;
    finish();
    QDialog::closeEvent(e);
}

void ThemeEditorDialog::reject()
{
    close();
}
