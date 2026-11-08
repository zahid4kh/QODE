#pragma once

#include <QElapsedTimer>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QTransform>
#include <QVector>
#include <QWidget>

class QPainter;

// The game panel's first page: one animated card per game with its high score and play totals (read from GameStore
// each time it is shown). Arrows / WASD / Tab choose, Enter / Space / click plays, 1 / 2 pick directly, Escape closes
// the panel. Drawn with QPainter in an 820x760 logical space scaled to fit, in the games' neon look.
class GamePicker : public QWidget
{
    Q_OBJECT
public:
    explicit GamePicker(QWidget *parent = nullptr);

    QSize sizeHint() const override;

    // Ids of the games, in card order: "teqris", "snake".
    static QStringList gameIds();
    static QString gameName(const QString &id);

    // Selects a game's card (the last one played when the panel opens).
    void select(const QString &id);

signals:
    void gameChosen(const QString &id);
    void closeRequested();

protected:
    bool event(QEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Card
    {
        QString id;
        QString name;
        QString tagline;
        QColor accent;
        int best = 0;
        int played = 0;
        qint64 playMs = 0;
        double lift = 0; // 0..1, eased towards 1 while selected
    };

    void tick();
    void reloadStats();
    void choose(int index);
    void paintCard(QPainter &p, const Card &card, const QRectF &rect, int index, bool selected);
    void paintTeqrisPreview(QPainter &p, const QRectF &area, QColor accent);
    void paintSnakePreview(QPainter &p, const QRectF &area, QColor accent);

    QVector<Card> m_cards;
    int m_selected = 0;
    bool m_hoverCard = false;

    QTimer m_timer; // runs only while shown
    QElapsedTimer m_clock;
    double m_time = 0;
    QTransform m_transform;
    QVector<QRectF> m_hitRects; // card rects of the last frame, in logical coordinates
};
