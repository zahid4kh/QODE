#include "NewProjectDialog.h"

#include "filesystem/FileManager.h"
#include "lsp/LspServers.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {
const int kIdRole = Qt::UserRole;

QString mutedStyle()
{
    const Theme t = Theme::byName(SettingsManager::instance().theme());
    return QStringLiteral("color: %1;").arg(t.textMuted.name());
}
} // namespace

NewProjectDialog::NewProjectDialog(const QString &defaultLocation, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("New Project"));
    setMinimumSize(780, 520);

    m_list = new QListWidget(this);
    m_list->setFixedWidth(210);
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setIconSize(QSize(16, 16));

    auto addHeader = [&](const QString &text) {
        auto *h = new QListWidgetItem(text.toUpper(), m_list);
        h->setFlags(Qt::NoItemFlags);
        QFont f = h->font();
        f.setBold(true);
        f.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.5));
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
        h->setFont(f);
    };
    auto *empty = new QListWidgetItem(tr("Empty Project"), m_list);
    empty->setData(kIdRole, QString());
    QString category;
    for (const ProjectTemplates::Template &t : ProjectTemplates::all()) {
        if (t.category != category) {
            category = t.category;
            addHeader(category);
        }
        auto *item = new QListWidgetItem(t.name, m_list);
        item->setData(kIdRole, t.id);
        item->setToolTip(t.description);
    }

    m_title = new QLabel(this);
    QFont tf = m_title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() + 3);
    m_title->setFont(tf);
    m_description = new QLabel(this);
    m_description->setWordWrap(true);
    m_description->setStyleSheet(mutedStyle());

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("MyProject"));
    m_location = new QLineEdit(defaultLocation.isEmpty() ? QDir::homePath() : defaultLocation, this);
    auto *browseBtn = new QPushButton(tr("Browse…"), this);
    browseBtn->setAutoDefault(false);
    auto *locRow = new QHBoxLayout;
    locRow->setContentsMargins(0, 0, 0, 0);
    locRow->addWidget(m_location, 1);
    locRow->addWidget(browseBtn);

    m_form = new QFormLayout;
    m_form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_form->addRow(tr("Project Name:"), m_name);
    m_form->addRow(tr("Location:"), locRow);
    m_staticRows = m_form->rowCount();

    m_requirement = new QLabel(this);
    m_requirement->setWordWrap(true);
    m_requirement->setVisible(false);
    m_git = new QCheckBox(tr("Initialize a Git repository"), this);
    m_hint = new QLabel(this);
    m_hint->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_create = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    m_create->setDefault(true);

    auto *right = new QVBoxLayout;
    right->addWidget(m_title);
    right->addWidget(m_description);
    right->addSpacing(8);
    right->addLayout(m_form);
    right->addWidget(m_requirement);
    right->addWidget(m_git);
    right->addStretch(1);
    right->addWidget(m_hint);
    right->addWidget(buttons);

    auto *root = new QHBoxLayout(this);
    root->addWidget(m_list);
    root->addLayout(right, 1);

    connect(browseBtn, &QPushButton::clicked, this, &NewProjectDialog::browse);
    connect(m_name, &QLineEdit::textChanged, this, [this] {
        refreshDefaults();
        validate();
    });
    connect(m_location, &QLineEdit::textChanged, this, &NewProjectDialog::validate);
    connect(m_list, &QListWidget::currentItemChanged, this, &NewProjectDialog::templateChosen);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_list->setCurrentRow(0);
    m_name->setFocus();
}

QString NewProjectDialog::projectName() const { return m_name->text().trimmed(); }
QString NewProjectDialog::location() const { return m_location->text().trimmed(); }
bool NewProjectDialog::initGit() const { return m_git->isChecked(); }

ProjectTemplates::Values NewProjectDialog::values() const
{
    ProjectTemplates::Values v;
    v.insert(QStringLiteral("name"), projectName());
    const ProjectTemplates::Template *t = ProjectTemplates::find(m_templateId);
    if (!t)
        return v;
    for (const ProjectTemplates::Option &o : t->options) {
        QWidget *w = m_fields.value(o.key);
        if (!w)
            continue;
        if (auto *c = qobject_cast<QCheckBox *>(w))
            v.insert(o.key, c->isChecked() ? QStringLiteral("true") : QStringLiteral("false"));
        else if (auto *b = qobject_cast<QComboBox *>(w))
            v.insert(o.key, b->currentText());
        else if (auto *e = qobject_cast<QLineEdit *>(w))
            v.insert(o.key, e->text().trimmed());
        else if (auto *e2 = w->findChild<QLineEdit *>()) // image: path field next to its buttons
            v.insert(o.key, e2->text().trimmed());
    }
    return v;
}

void NewProjectDialog::browse()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Project Location"), location());
    if (!dir.isEmpty())
        m_location->setText(dir);
}

void NewProjectDialog::templateChosen()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    m_templateId = item->data(kIdRole).toString();
    rebuildOptions();
    validate();
}

QWidget *NewProjectDialog::makeField(const ProjectTemplates::Option &o)
{
    QWidget *field = nullptr;
    if (o.type == QLatin1String("bool")) {
        auto *c = new QCheckBox(o.label, this);
        c->setChecked(o.def == QLatin1String("true"));
        connect(c, &QCheckBox::toggled, this, &NewProjectDialog::validate);
        field = c;
    } else if (o.type == QLatin1String("choice")) {
        auto *b = new QComboBox(this);
        b->addItems(o.choices);
        b->setCurrentText(o.def);
        connect(b, &QComboBox::currentTextChanged, this, &NewProjectDialog::validate);
        field = b;
    } else if (o.type == QLatin1String("image")) {
        auto *host = new QWidget(this);
        auto *row = new QHBoxLayout(host);
        row->setContentsMargins(0, 0, 0, 0);
        auto *preview = new QLabel(host);
        preview->setFixedSize(40, 40);
        preview->setAlignment(Qt::AlignCenter);
        auto *edit = new QLineEdit(host);
        edit->setPlaceholderText(tr("Default icon (first letter of the app name)"));
        auto *browse = new QPushButton(tr("Browse…"), host);
        browse->setAutoDefault(false);
        auto *clear = new QPushButton(tr("Clear"), host);
        clear->setAutoDefault(false);
        row->addWidget(preview);
        row->addWidget(edit, 1);
        row->addWidget(browse);
        row->addWidget(clear);
        m_previews.insert(o.key, preview);
        connect(browse, &QPushButton::clicked, this, [this, edit] { browseImage(edit); });
        connect(clear, &QPushButton::clicked, edit, &QLineEdit::clear);
        connect(edit, &QLineEdit::textChanged, this, [this](const QString &p) {
            updatePreview(p.trimmed());
            validate();
        });
        field = host;
    } else {
        auto *e = new QLineEdit(this);
        e->setText(o.def);
        connect(e, &QLineEdit::textEdited, this, [this, key = o.key] { m_edited.insert(key); });
        connect(e, &QLineEdit::textChanged, this, [this] {
            if (!m_updating) {
                refreshDefaults();
                validate();
            }
        });
        field = e;
    }
    if (!o.hint.isEmpty()) {
        auto *wrap = new QWidget(this);
        auto *col = new QVBoxLayout(wrap);
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(2);
        col->addWidget(field);
        auto *hint = new QLabel(o.hint, wrap);
        hint->setWordWrap(true);
        hint->setStyleSheet(mutedStyle());
        col->addWidget(hint);
        m_fields.insert(o.key, field);
        return wrap;
    }
    m_fields.insert(o.key, field);
    return field;
}

void NewProjectDialog::browseImage(QLineEdit *edit)
{
    const QString file = QFileDialog::getOpenFileName(this, tr("App Icon"), QDir::homePath(),
                                                      tr("Images (*.png *.svg *.jpg *.jpeg *.webp *.bmp);;All files (*)"));
    if (!file.isEmpty())
        edit->setText(file);
}

void NewProjectDialog::updatePreview(const QString &path)
{
    QLabel *preview = m_previews.value(QStringLiteral("icon"));
    if (!preview)
        return;
    QPixmap pm;
    if (!path.isEmpty()) {
        QImageReader r(path);
        r.setAutoTransform(true);
        if (r.format() == "svg" || r.format() == "svgz")
            r.setScaledSize(QSize(40, 40));
        const QImage img = r.read();
        if (!img.isNull())
            pm = QPixmap::fromImage(img.scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }
    preview->setPixmap(pm);
}

void NewProjectDialog::rebuildOptions()
{
    while (m_form->rowCount() > m_staticRows)
        m_form->removeRow(m_staticRows);
    m_fields.clear();
    m_previews.clear();
    m_edited.clear();

    const ProjectTemplates::Template *t = ProjectTemplates::find(m_templateId);
    if (!t) {
        m_title->setText(tr("Empty Project"));
        m_description->setText(tr("A new, empty folder. Add files and folders from the explorer."));
        m_requirement->setVisible(false);
        return;
    }
    m_title->setText(t->name);
    m_description->setText(t->description);

    m_updating = true;
    for (const ProjectTemplates::Option &o : t->options) {
        QWidget *w = makeField(o);
        if (o.type == QLatin1String("bool"))
            m_form->addRow(QString(), w);
        else
            m_form->addRow(o.label + QLatin1Char(':'), w);
    }
    m_updating = false;
    refreshDefaults();

    // Prerequisites only warn: the project is still created and the tool can be installed afterwards.
    QStringList warnings;
    for (const QString &req : t->requires) {
        static const QRegularExpression jdk(QStringLiteral(R"(^jdk(\d+)$)"));
        const auto m = jdk.match(req);
        if (m.hasMatch()) {
            const int need = m.captured(1).toInt();
            if (LspServers::findJava(need).executable.isEmpty())
                warnings << tr("A JDK %1 or newer was not found. Gradle needs one to build and run the project: install it or set JAVA_HOME. The project is still created.").arg(need);
        }
    }
    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    m_requirement->setStyleSheet(QStringLiteral("color: %1;").arg(theme.warning.name()));
    m_requirement->setText(warnings.join(QLatin1Char('\n')));
    m_requirement->setVisible(!warnings.isEmpty());
}

// Options the user has not touched follow the project name (package com.example.<name>, app name = project name).
void NewProjectDialog::refreshDefaults()
{
    const ProjectTemplates::Template *t = ProjectTemplates::find(m_templateId);
    if (!t || m_updating)
        return;
    m_updating = true;
    for (const ProjectTemplates::Option &o : t->options) {
        auto *e = qobject_cast<QLineEdit *>(m_fields.value(o.key));
        if (!e || m_edited.contains(o.key) || o.type == QLatin1String("image"))
            continue;
        const QString v = ProjectTemplates::defaultValue(*t, o, values());
        if (e->text() != v)
            e->setText(v);
    }
    m_updating = false;
}

void NewProjectDialog::validate()
{
    if (m_updating)
        return;
    QString msg;
    if (!projectName().isEmpty())
        msg = FileManager::validateName(projectName());
    if (msg.isEmpty() && projectName().contains(QRegularExpression(QStringLiteral(R"(\s)"))))
        msg = tr("The project name can't contain a space.");
    bool ok = !projectName().isEmpty() && msg.isEmpty() && !location().isEmpty();

    const QString full = QDir(location()).filePath(projectName());
    if (ok && !m_templateId.isEmpty()) {
        const QFileInfo target(full);
        if (target.exists() && (!target.isDir() || !QDir(full).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)))
            msg = tr("\"%1\" already exists and is not empty.").arg(FileManager::displayPath(full));
        else if (const ProjectTemplates::Template *t = ProjectTemplates::find(m_templateId))
            msg = ProjectTemplates::validate(*t, values());
        ok = msg.isEmpty();
    }

    const Theme theme = Theme::byName(SettingsManager::instance().theme());
    if (ok) {
        m_hint->setStyleSheet(mutedStyle());
        m_hint->setText(tr("Will be created at: %1").arg(FileManager::displayPath(full)));
    } else {
        m_hint->setStyleSheet(QStringLiteral("color: %1;").arg(msg.isEmpty() ? theme.textMuted.name() : theme.danger.name()));
        m_hint->setText(msg);
    }
    m_create->setEnabled(ok);
}
