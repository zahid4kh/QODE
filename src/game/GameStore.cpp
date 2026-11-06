#include "game/GameStore.h"

#include "settings/SettingsManager.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>

namespace {
const QString KeyBest = QStringLiteral("bestScore");
const QString KeyGames = QStringLiteral("gamesPlayed");
const QString KeyPlayMs = QStringLiteral("totalPlayMs");
const QString KeyTop = QStringLiteral("topScores");
const QString KeyOptions = QStringLiteral("options");
} // namespace

GameStore::GameStore(const QString &gameId)
    : m_id(gameId)
{
    const QString configDir = QFileInfo(SettingsManager::instance().configFilePath()).absolutePath();
    m_path = configDir + QStringLiteral("/games/") + gameId + QStringLiteral(".json");
    load();
}

void GameStore::load()
{
    QFile f(m_path);
    if (f.open(QIODevice::ReadOnly))
        m_data = QJsonDocument::fromJson(f.readAll()).object();
}

void GameStore::save() const
{
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QJsonObject data = m_data;
    data.insert(QStringLiteral("version"), 1);
    data.insert(QStringLiteral("game"), m_id);
    QSaveFile f(m_path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(data).toJson(QJsonDocument::Indented));
        f.commit();
    }
}

int GameStore::bestScore() const
{
    return m_data.value(KeyBest).toInt();
}

QList<GameStore::Entry> GameStore::topScores() const
{
    QList<Entry> list;
    for (const QJsonValue &v : m_data.value(KeyTop).toArray()) {
        QJsonObject o = v.toObject();
        Entry e;
        e.score = o.take(QStringLiteral("score")).toInt();
        e.playMs = qint64(o.take(QStringLiteral("playMs")).toDouble());
        e.date = o.take(QStringLiteral("date")).toString();
        e.stats = o;
        list.append(e);
    }
    return list;
}

int GameStore::gamesPlayed() const
{
    return m_data.value(KeyGames).toInt();
}

qint64 GameStore::totalPlayMs() const
{
    return qint64(m_data.value(KeyPlayMs).toDouble());
}

QVariant GameStore::option(const QString &key, const QVariant &fallback) const
{
    const QJsonValue v = m_data.value(KeyOptions).toObject().value(key);
    return v.isUndefined() ? fallback : v.toVariant();
}

void GameStore::setOption(const QString &key, const QVariant &value)
{
    QJsonObject options = m_data.value(KeyOptions).toObject();
    const QJsonValue v = QJsonValue::fromVariant(value);
    if (options.value(key) == v)
        return;
    options.insert(key, v);
    m_data.insert(KeyOptions, options);
    save();
}

bool GameStore::recordGame(int score, const QJsonObject &stats, qint64 playMs)
{
    m_data.insert(KeyGames, gamesPlayed() + 1);
    m_data.insert(KeyPlayMs, double(totalPlayMs() + playMs));

    const bool best = score > bestScore();
    if (best)
        m_data.insert(KeyBest, score);

    if (score > 0) {
        QJsonObject entry = stats;
        entry.insert(QStringLiteral("score"), score);
        entry.insert(QStringLiteral("playMs"), double(playMs));
        entry.insert(QStringLiteral("date"), QDateTime::currentDateTime().toString(Qt::ISODate));
        QList<QJsonObject> top;
        for (const QJsonValue &v : m_data.value(KeyTop).toArray())
            top.append(v.toObject());
        top.append(entry);
        std::stable_sort(top.begin(), top.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value(QStringLiteral("score")).toInt() > b.value(QStringLiteral("score")).toInt();
        });
        QJsonArray array;
        for (int i = 0; i < top.size() && i < TopCount; ++i)
            array.append(top[i]);
        m_data.insert(KeyTop, array);
    }
    save();
    return best;
}
