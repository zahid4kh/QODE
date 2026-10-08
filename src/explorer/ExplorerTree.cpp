#include "ExplorerTree.h"

#include "explorer/FileIcons.h"
#include "project/ProjectModel.h"
#include "settings/Icons.h"
#include "settings/SettingsManager.h"

#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>

namespace {

const QString kMime = QStringLiteral("application/x-qode-explorer-paths");

QString dirOf(const QString &path)
{
    return QFileInfo(path).absolutePath();
}

bool isSource(const QString &path)
{
    static const QSet<QString> exts = {
        QStringLiteral("js"),   QStringLiteral("jsx"), QStringLiteral("ts"),   QStringLiteral("tsx"),   QStringLiteral("mjs"),  QStringLiteral("cjs"),
        QStringLiteral("vue"),  QStringLiteral("css"), QStringLiteral("scss"), QStringLiteral("html"),  QStringLiteral("md"),   QStringLiteral("py"),
        QStringLiteral("java"), QStringLiteral("kt"),  QStringLiteral("go"),   QStringLiteral("rs"),    QStringLiteral("c"),    QStringLiteral("cpp"),
        QStringLiteral("h"),    QStringLiteral("hpp"), QStringLiteral("cc"),   QStringLiteral("qml"),   QStringLiteral("php"),  QStringLiteral("sh")};
    return QFileInfo(path).isDir() || exts.contains(QFileInfo(path).suffix().toLower());
}

} // namespace

ExplorerTree::ExplorerTree(QWidget *parent)
    : QTreeView(parent)
    , m_spring(new QTimer(this))
    , m_scroll(new QTimer(this))
    , m_theme(Theme::byName(SettingsManager::instance().theme()))
{
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::DragDrop);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDefaultDropAction(Qt::MoveAction);
    setAutoScroll(false); // done by hand below: the stock one only runs for models that accept drops

    m_spring->setSingleShot(true);
    m_spring->setInterval(650);
    connect(m_spring, &QTimer::timeout, this, [this] {
        if (m_dragging && m_hoverDir.isValid() && !isExpanded(m_hoverDir))
            expand(m_hoverDir);
    });
    m_scroll->setInterval(30);
    connect(m_scroll, &QTimer::timeout, this, &ExplorerTree::autoScrollStep);
    connect(&SettingsManager::instance(), &SettingsManager::themeChanged, this, [this](const QString &name) { m_theme = Theme::byName(name); });
}

QString ExplorerTree::pathOf(const QModelIndex &idx) const
{
    return idx.isValid() ? idx.data(ProjectModel::PathRole).toString() : QString();
}

QStringList ExplorerTree::topLevelSelection() const
{
    QStringList all;
    for (const QModelIndex &i : selectedIndexes())
        if (i.column() == 0) {
            const QString p = pathOf(i);
            if (!p.isEmpty() && p != m_root && !all.contains(p))
                all << p;
        }
    QStringList out;
    for (const QString &p : std::as_const(all)) {
        bool nested = false;
        for (const QString &q : std::as_const(all))
            nested = nested || (q != p && p.startsWith(q + QLatin1Char('/')));
        if (!nested)
            out << p;
    }
    out.sort();
    return out;
}

QString ExplorerTree::folderLabel(const QString &dir) const
{
    if (dir == m_root)
        return QFileInfo(m_root).fileName();
    if (dir.startsWith(m_root + QLatin1Char('/')))
        return dir.mid(m_root.size() + 1);
    return dir;
}

QModelIndex ExplorerTree::lastVisibleDescendant(const QModelIndex &idx) const
{
    QModelIndex cur = idx;
    while (isExpanded(cur) && model()->rowCount(cur) > 0)
        cur = model()->index(model()->rowCount(cur) - 1, 0, cur);
    return cur;
}

ExplorerTree::Verdict ExplorerTree::verdictAt(const QPoint &pos) const
{
    Verdict v;
    if (m_dragPaths.isEmpty() || m_root.isEmpty())
        return v;
    const QModelIndex idx = indexAt(pos);
    const QString hit = idx.isValid() ? pathOf(idx) : m_root;
    v.targetDir = QFileInfo(hit).isDir() ? hit : dirOf(hit);
    v.name = folderLabel(v.targetDir);

    if (m_external) {
        v.copy = true;
        for (const QString &src : m_dragPaths) {
            if (QFileInfo(src).isDir() && (v.targetDir == src || v.targetDir.startsWith(src + QLatin1Char('/')))) {
                v.kind = Verdict::IntoItself;
                v.text = tr("Can't copy “%1” into itself").arg(QFileInfo(src).fileName());
                return v;
            }
        }
        v.kind = Verdict::Ok;
        v.items = m_dragPaths;
        return v;
    }
    for (const QString &src : m_dragPaths) {
        if (v.targetDir == src || v.targetDir.startsWith(src + QLatin1Char('/'))) {
            v.kind = Verdict::IntoItself;
            v.text = tr("Can't move “%1” into itself").arg(QFileInfo(src).fileName());
            return v;
        }
    }
    QStringList moving, names;
    for (const QString &src : m_dragPaths) {
        if (dirOf(src) == v.targetDir)
            continue;
        const QString name = QFileInfo(src).fileName();
        if (QFileInfo::exists(v.targetDir + QLatin1Char('/') + name) || names.contains(name)) {
            v.kind = Verdict::NameTaken;
            v.text = tr("“%1” already exists in %2").arg(name, v.name);
            return v;
        }
        names << name;
        moving << src;
    }
    if (moving.isEmpty()) {
        v.kind = Verdict::SameFolder;
        v.text = m_dragPaths.size() == 1 ? tr("Already in %1").arg(v.name) : tr("Already in %1").arg(v.name);
        return v;
    }
    v.kind = Verdict::Ok;
    v.items = moving;
    return v;
}

// --- drag source ---------------------------------------------------------------

QPixmap ExplorerTree::dragPixmap(const QStringList &paths) const
{
    const qreal dpr = devicePixelRatioF();
    QFont f = font();
    f.setPointSizeF(qMax(8.0, f.pointSizeF() - 0.5));
    const QFontMetrics fm(f);
    const QString first = QFileInfo(paths.first()).fileName();
    const int pad = 18; // transparent margin so the chip sits beside the pointer, not under it
    const int iconSz = 15, h = 28;
    const QString label = fm.elidedText(first, Qt::ElideMiddle, 220);
    const int textW = fm.horizontalAdvance(label);
    const int badgeW = paths.size() > 1 ? fm.horizontalAdvance(QStringLiteral("+%1").arg(paths.size() - 1)) + 12 : 0;
    const int w = 10 + iconSz + 7 + textW + 12 + (badgeW ? badgeW + 6 : 0);
    QPixmap pm(QSize(w + pad, h + pad) * dpr);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(0.94);
    const QRectF chip(pad, pad, w, h);
    p.setPen(QPen(m_theme.accent, 1.2));
    p.setBrush(m_theme.panel);
    p.drawRoundedRect(chip, 8, 8);
    const QIcon icon = QFileInfo(paths.first()).isDir() ? FileIcons::folder() : FileIcons::forFile(first);
    icon.paint(&p, QRect(int(chip.left()) + 10, int(chip.top()) + (h - iconSz) / 2, iconSz, iconSz));
    p.setFont(f);
    p.setPen(m_theme.editorFg);
    p.drawText(QRectF(chip.left() + 10 + iconSz + 7, chip.top(), textW + 4, h), Qt::AlignVCenter | Qt::AlignLeft, label);
    if (badgeW) {
        const QRectF badge(chip.right() - badgeW - 6, chip.top() + 5, badgeW, h - 10);
        p.setPen(Qt::NoPen);
        p.setBrush(m_theme.accent);
        p.drawRoundedRect(badge, badge.height() / 2, badge.height() / 2);
        p.setPen(m_theme.onAccent());
        QFont b = f;
        b.setBold(true);
        p.setFont(b);
        p.drawText(badge, Qt::AlignCenter, QStringLiteral("+%1").arg(paths.size() - 1));
    }
    return pm;
}

void ExplorerTree::startDrag(Qt::DropActions)
{
    const QStringList paths = topLevelSelection();
    if (paths.isEmpty())
        return;
    m_dragPaths = paths;
    m_dragging = true;
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    for (const QString &p : paths)
        urls << QUrl::fromLocalFile(p);
    mime->setUrls(urls); // other applications can take the files too
    mime->setData(kMime, paths.join(QLatin1Char('\n')).toUtf8());
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(dragPixmap(paths));
    drag->setHotSpot(QPoint(0, 0));
    drag->exec(Qt::MoveAction, Qt::MoveAction);
    endDrag();
}

void ExplorerTree::endDrag()
{
    m_dragging = false;
    m_external = false;
    m_dragPaths.clear();
    m_verdict = {};
    m_hoverDir = QPersistentModelIndex();
    m_spring->stop();
    m_scroll->stop();
    viewport()->update();
}

void ExplorerTree::previewDrag(const QStringList &paths, const QPoint &pos)
{
    if (paths.isEmpty()) {
        endDrag();
        return;
    }
    m_dragPaths = paths;
    m_dragging = true;
    m_cursor = pos;
    m_verdict = verdictAt(pos);
    viewport()->update();
}

// --- drop target ---------------------------------------------------------------

static QStringList localPaths(const QMimeData *m)
{
    QStringList out;
    for (const QUrl &u : m->urls())
        if (u.isLocalFile())
            out << u.toLocalFile();
    return out;
}

void ExplorerTree::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->source() != this && !localPaths(e->mimeData()).isEmpty()) {
        m_dragging = true;
        m_external = true;
        e->setDropAction(Qt::CopyAction);
        e->accept();
    } else if (e->source() == this && e->mimeData()->hasFormat(kMime)) {
        m_dragging = true;
        e->acceptProposedAction();
    } else {
        e->ignore();
    }
}

void ExplorerTree::dragMoveEvent(QDragMoveEvent *e)
{
    if (e->source() != this) {
        const QStringList ext = localPaths(e->mimeData());
        if (ext.isEmpty() || m_root.isEmpty()) {
            e->ignore();
            return;
        }
        m_external = true;
        m_dragging = true;
        m_dragPaths = ext;
    } else if (!e->mimeData()->hasFormat(kMime)) {
        e->ignore();
        return;
    }
    if (m_dragPaths.isEmpty()) // a drag that started before this widget knew about it
        m_dragPaths = QString::fromUtf8(e->mimeData()->data(kMime)).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    m_cursor = e->position().toPoint();
    m_verdict = verdictAt(m_cursor);

    // spring-loaded folders
    const QModelIndex under = indexAt(m_cursor);
    QModelIndex dirIdx;
    if (under.isValid() && QFileInfo(pathOf(under)).isDir())
        dirIdx = under;
    if (QPersistentModelIndex(dirIdx) != m_hoverDir) {
        m_hoverDir = QPersistentModelIndex(dirIdx);
        m_spring->stop();
        if (dirIdx.isValid() && !isExpanded(dirIdx))
            m_spring->start();
    }
    // auto scroll near the edges
    const int edge = 28;
    if (m_cursor.y() < edge || m_cursor.y() > viewport()->height() - edge)
        m_scroll->start();
    else
        m_scroll->stop();

    if (m_verdict.kind == Verdict::Ok) {
        e->setDropAction(m_external ? Qt::CopyAction : Qt::MoveAction);
        e->accept();
    } else {
        e->ignore();
    }
    viewport()->update();
}

void ExplorerTree::dragLeaveEvent(QDragLeaveEvent *e)
{
    if (m_external) {
        m_external = false;
        m_dragging = false;
        m_dragPaths.clear();
    }
    m_verdict = {};
    m_hoverDir = QPersistentModelIndex();
    m_spring->stop();
    m_scroll->stop();
    viewport()->update();
    QTreeView::dragLeaveEvent(e);
}

void ExplorerTree::dropEvent(QDropEvent *e)
{
    const Verdict v = verdictAt(e->position().toPoint());
    m_verdict = {};
    m_scroll->stop();
    m_spring->stop();
    viewport()->update();
    if (v.kind != Verdict::Ok) {
        m_external = false;
        e->ignore();
        return;
    }
    const QStringList items = v.items;
    const QString target = v.targetDir;
    if (m_external) {
        m_external = false;
        m_dragging = false;
        m_dragPaths.clear();
        e->setDropAction(Qt::CopyAction);
        e->accept();
        QTimer::singleShot(0, this, [this, items, target] { emit copyRequested(items, target); });
        return;
    }
    if (e->source() != this) {
        e->ignore();
        return;
    }
    e->setDropAction(Qt::MoveAction);
    e->accept();
    // Let the drag finish before anything is moved underneath it.
    QTimer::singleShot(0, this, [this, items, target] { emit moveRequested(items, target); });
}

void ExplorerTree::autoScrollStep()
{
    if (!m_dragging)
        return;
    const int edge = 28;
    QScrollBar *bar = verticalScrollBar();
    int delta = 0;
    if (m_cursor.y() < edge)
        delta = -qMax(2, (edge - m_cursor.y()) / 2);
    else if (m_cursor.y() > viewport()->height() - edge)
        delta = qMax(2, (m_cursor.y() - (viewport()->height() - edge)) / 2);
    if (delta == 0) {
        m_scroll->stop();
        return;
    }
    bar->setValue(bar->value() + delta);
    m_verdict = verdictAt(m_cursor);
    viewport()->update();
}

// --- painting ------------------------------------------------------------------

void ExplorerTree::paintEvent(QPaintEvent *e)
{
    QTreeView::paintEvent(e);
    if (!m_dragging || m_verdict.kind == Verdict::None)
        return;
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);
    paintHighlight(p);
    paintHint(p);
}

void ExplorerTree::paintHighlight(QPainter &p) const
{
    if (m_verdict.targetDir.isEmpty())
        return;
    // the folder's row and everything shown below it
    QModelIndex dirIdx;
    const QModelIndex under = indexAt(m_cursor);
    if (!under.isValid() || m_verdict.targetDir == m_root) {
        const QModelIndex rootParent = rootIndex();
        for (int r = 0; r < model()->rowCount(rootParent); ++r) {
            const QModelIndex i = model()->index(r, 0, rootParent);
            if (pathOf(i) == m_verdict.targetDir)
                dirIdx = i;
        }
    } else {
        dirIdx = QFileInfo(pathOf(under)).isDir() ? under : under.parent();
    }
    if (!dirIdx.isValid())
        return;
    const QRect top = visualRect(dirIdx);
    const QRect bottom = visualRect(lastVisibleDescendant(dirIdx));
    QRect r = top;
    if (bottom.isValid())
        r = r.united(bottom);
    r = QRect(2, r.top(), viewport()->width() - 4, r.height()).adjusted(0, 0, 0, 0);
    QColor stroke, fill;
    switch (m_verdict.kind) {
    case Verdict::Ok:
        stroke = m_theme.accent;
        fill = m_theme.accent;
        fill.setAlpha(34);
        break;
    case Verdict::NameTaken:
    case Verdict::IntoItself:
        stroke = m_theme.gitDeleted;
        fill = m_theme.gitDeleted;
        fill.setAlpha(26);
        break;
    default:
        stroke = m_theme.textMuted;
        fill = m_theme.textMuted;
        fill.setAlpha(20);
        break;
    }
    p.save();
    p.setClipRect(viewport()->rect());
    p.setPen(QPen(stroke, 1.4));
    p.setBrush(fill);
    p.drawRoundedRect(QRectF(r).adjusted(0.7, 0.7, -0.7, -0.7), 6, 6);
    p.restore();
}

void ExplorerTree::paintHint(QPainter &p) const
{
    struct Seg {
        QString text;
        bool bold;
        QColor color;
    };
    QVector<Seg> line1;
    QString line2;
    QColor edge;
    QString iconRes;
    const QColor fg = m_theme.editorFg;
    switch (m_verdict.kind) {
    case Verdict::Ok: {
        const QString what = m_verdict.items.size() == 1 ? tr("“%1”").arg(QFileInfo(m_verdict.items.first()).fileName()) : tr("%1 items").arg(m_verdict.items.size());
        line1 = {{m_verdict.copy ? tr("Copy ") : tr("Move "), false, fg}, {what, true, fg}, {tr(" into "), false, fg}, {m_verdict.name, true, m_theme.accent}};
        bool any = false;
        for (const QString &s : m_verdict.items)
            any = any || (!m_verdict.copy && isSource(s));
        if (any)
            line2 = tr("Imports and links update automatically");
        edge = m_theme.accent;
        iconRes = QStringLiteral(":/new-icons/folder-open.svg");
        break;
    }
    case Verdict::SameFolder:
        line1 = {{m_verdict.text, false, m_theme.textMuted}};
        edge = m_theme.border;
        iconRes = QStringLiteral(":/new-icons/folder.svg");
        break;
    default:
        line1 = {{m_verdict.text, false, fg}};
        edge = m_theme.gitDeleted;
        iconRes = QStringLiteral(":/new-icons/x.svg");
        break;
    }
    QFont f = font();
    f.setPointSizeF(qMax(8.0, f.pointSizeF() - 0.5));
    QFont bf = f;
    bf.setBold(true);
    QFont sf = f;
    sf.setPointSizeF(f.pointSizeF() - 1);
    const QFontMetrics fm(f), bm(bf), sm(sf);
    int w1 = 0;
    for (const Seg &s : std::as_const(line1))
        w1 += (s.bold ? bm : fm).horizontalAdvance(s.text);
    const int iconSz = 14;
    int contentW = iconSz + 7 + w1;
    if (!line2.isEmpty())
        contentW = qMax(contentW, iconSz + 7 + sm.horizontalAdvance(line2));
    const int padX = 10, padY = 6;
    const int lineH = fm.height();
    const int h = padY * 2 + lineH + (line2.isEmpty() ? 0 : sm.height() + 1);
    const int w = padX * 2 + contentW;
    QRect box(m_cursor.x() + 16, m_cursor.y() + 26, w, h);
    const QRect vp = viewport()->rect().adjusted(4, 4, -4, -4);
    if (box.right() > vp.right())
        box.moveRight(m_cursor.x() - 10);
    if (box.left() < vp.left())
        box.moveLeft(vp.left());
    if (box.bottom() > vp.bottom())
        box.moveBottom(m_cursor.y() - 14);
    if (box.top() < vp.top())
        box.moveTop(vp.top());

    p.save();
    // soft shadow
    for (int i = 3; i >= 1; --i) {
        QColor sh(0, 0, 0, 14 * (4 - i));
        p.setPen(Qt::NoPen);
        p.setBrush(sh);
        p.drawRoundedRect(box.adjusted(-i, -i + 2, i, i + 2), 9 + i, 9 + i);
    }
    p.setPen(QPen(edge, 1.2));
    p.setBrush(m_theme.panel);
    p.drawRoundedRect(QRectF(box).adjusted(0.6, 0.6, -0.6, -0.6), 9, 9);
    const QPixmap icon = Icons::pixmap(iconRes, edge, iconSz, devicePixelRatioF());
    p.drawPixmap(box.left() + padX, box.top() + padY + (lineH - iconSz) / 2, icon);
    int x = box.left() + padX + iconSz + 7;
    const int base = box.top() + padY + fm.ascent();
    for (const Seg &s : std::as_const(line1)) {
        p.setFont(s.bold ? bf : f);
        p.setPen(s.color);
        p.drawText(x, base, s.text);
        x += (s.bold ? bm : fm).horizontalAdvance(s.text);
    }
    if (!line2.isEmpty()) {
        p.setFont(sf);
        p.setPen(m_theme.textMuted);
        p.drawText(box.left() + padX + iconSz + 7, box.top() + padY + lineH + 1 + sm.ascent(), line2);
    }
    p.restore();
}
