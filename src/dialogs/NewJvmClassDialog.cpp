#include "NewJvmClassDialog.h"

#include "explorer/FileIcons.h"
#include "filesystem/FileManager.h"
#include "settings/SettingsManager.h"
#include "settings/Theme.h"

#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QVBoxLayout>

namespace {

constexpr int kKindRole = Qt::UserRole + 1;

// A round badge with the kind's letter, the way IDEs mark classes, interfaces and enums.
QIcon badge(QChar letter, const QColor &color, const QColor &fg, qreal dpr)
{
    const int size = 16;
    QPixmap pm(QSize(size, size) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(0.5, 0.5, size - 1, size - 1));
    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);
    p.setPen(fg);
    p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, QString(letter));
    return QIcon(pm);
}

} // namespace

NewJvmClassDialog::NewJvmClassDialog(bool kotlin, const QString &dir, QWidget *parent)
    : QDialog(parent)
    , m_kotlin(kotlin)
    , m_dir(dir)
{
    setWindowTitle(kotlin ? tr("New Kotlin Class/File") : tr("New Java Class"));
    setMinimumWidth(420);
    const Theme t = Theme::byName(SettingsManager::instance().theme());

    auto *title = new QLabel(kotlin ? tr("New Kotlin Class/File") : tr("New Java Class"), this);
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    auto *logo = new QLabel(this);
    logo->setPixmap(FileIcons::forFile(kotlin ? QStringLiteral("x.kt") : QStringLiteral("x.java")).pixmap(16, 16));

    m_name = new QLineEdit(this);
    m_name->setPlaceholderText(tr("Name, or sub.package.Name"));
    m_name->installEventFilter(this);

    bool known = false;
    m_package = JvmNewFile::packageFor(dir, &known);
    m_where = new QLabel(this);
    m_where->setObjectName(QStringLiteral("emptyText"));
    m_where->setWordWrap(true);
    m_error = new QLabel(this);
    m_error->setWordWrap(true);
    QPalette ep = m_error->palette();
    ep.setColor(QPalette::WindowText, t.danger);
    m_error->setPalette(ep);

    m_kinds = new QListWidget(this);
    m_kinds->setIconSize(QSize(16, 16));
    m_kinds->setUniformItemSizes(true);
    m_kinds->setFocusPolicy(Qt::NoFocus);
    const QColor tones[] = {t.accent, t.gitAdded, t.number, t.keyword, t.textMuted};
    const qreal dpr = devicePixelRatioF();
    const QList<JvmNewFile::KindInfo> kinds = JvmNewFile::kinds(kotlin);
    for (const JvmNewFile::KindInfo &k : kinds) {
        auto *it = new QListWidgetItem(k.title, m_kinds);
        it->setData(kKindRole, static_cast<int>(k.kind));
        if (k.kind == JvmNewFile::Kind::File)
            it->setIcon(FileIcons::forFile(QStringLiteral("x.kt")));
        else
            it->setIcon(badge(k.badge, tones[qBound(0, k.tone, 4)], t.onAccent(), dpr));
    }
    m_kinds->setCurrentRow(0);
    const int rowH = m_kinds->sizeHintForRow(0);
    m_kinds->setFixedHeight(rowH * int(kinds.size()) + 2 * m_kinds->frameWidth() + 4);
    connect(m_kinds, &QListWidget::currentRowChanged, this, &NewJvmClassDialog::update);
    connect(m_kinds, &QListWidget::itemDoubleClicked, this, &NewJvmClassDialog::tryAccept);
    connect(m_name, &QLineEdit::textChanged, this, &NewJvmClassDialog::update);
    connect(m_name, &QLineEdit::returnPressed, this, &NewJvmClassDialog::tryAccept);

    auto *head = new QHBoxLayout;
    head->setSpacing(6);
    head->addWidget(logo);
    head->addWidget(title, 1);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);
    layout->addLayout(head);
    layout->addWidget(m_name);
    layout->addWidget(m_kinds);
    layout->addWidget(m_where);
    layout->addWidget(m_error);
    update();
}

JvmNewFile::Kind NewJvmClassDialog::currentKind() const
{
    QListWidgetItem *it = m_kinds->currentItem();
    return it ? static_cast<JvmNewFile::Kind>(it->data(kKindRole).toInt()) : (m_kotlin ? JvmNewFile::Kind::Class : JvmNewFile::Kind::JavaClass);
}

void NewJvmClassDialog::update()
{
    const QString text = m_name->text().trimmed();
    QString pkg = m_package;
    const int dot = text.lastIndexOf(QLatin1Char('.'));
    if (dot > 0) {
        const QString sub = text.left(dot);
        pkg = pkg.isEmpty() ? sub : pkg + QLatin1Char('.') + sub;
    }
    m_where->setText(tr("Package: %1\nIn: %2").arg(pkg.isEmpty() ? tr("(default package)") : pkg, FileManager::displayPath(m_dir)));
    if (text.isEmpty()) {
        m_error->clear();
        m_error->hide();
        return;
    }
    const JvmNewFile::Result r = JvmNewFile::generate(currentKind(), m_dir, text);
    m_error->setText(r.error);
    m_error->setVisible(!r.ok());
}

void NewJvmClassDialog::tryAccept()
{
    m_result = JvmNewFile::generate(currentKind(), m_dir, m_name->text());
    if (!m_result.ok()) {
        m_error->setText(m_result.error);
        m_error->show();
        m_name->setFocus();
        return;
    }
    accept();
}

bool NewJvmClassDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_name && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            const int n = m_kinds->count();
            const int row = (m_kinds->currentRow() + (key == Qt::Key_Down ? 1 : n - 1)) % n;
            m_kinds->setCurrentRow(row);
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}
