#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVariant>

// The saved data of one game in the game panel: ~/.config/QODE-AGENTIC/games/<id>.json, next to QODE.conf.
// Holds the best score, the game's own options (start level, …), a top-10 list and totals (games, play time).
// Global, not per project: the same high score follows the user everywhere. Written on every change.
class GameStore
{
public:
    struct Entry
    {
        int score = 0;
        QJsonObject stats; // what the game adds: lines, level, …
        qint64 playMs = 0;
        QString date;      // ISO 8601, local time
    };

    explicit GameStore(const QString &gameId);

    QString filePath() const { return m_path; }

    int bestScore() const;
    QList<Entry> topScores() const; // best first, at most TopCount
    int gamesPlayed() const;
    qint64 totalPlayMs() const;     // finished games only

    QVariant option(const QString &key, const QVariant &fallback = {}) const;
    void setOption(const QString &key, const QVariant &value);

    // Records a finished game; returns true when it beat the best score.
    bool recordGame(int score, const QJsonObject &stats, qint64 playMs);

    static constexpr int TopCount = 10;

private:
    void load();
    void save() const;

    QString m_id;
    QString m_path;
    QJsonObject m_data;
};
