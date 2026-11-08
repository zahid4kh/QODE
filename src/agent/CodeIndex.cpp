#include "CodeIndex.h"

#include "ai/AiEnv.h"
#include "ai/GeminiClient.h"
#include "settings/SettingsManager.h"
#include "editor/TextEncoding.h"

#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <memory>

using namespace agent::codeindex;

namespace {

constexpr qint64 kMaxFileBytes = 512 * 1024;
constexpr int kMaxFiles = 40000;
constexpr int kMaxVectors = 60000;   // a cap on what one project can send for embedding
constexpr int kBatch = 100;          // batchEmbedContents takes up to 100 requests
constexpr int kBatchCharsMax = 160000; // ~40k tokens
constexpr int kBatchCharsMin = 6000;
constexpr int kBatchCharsStart = 60000; // the free tier allows about 30k tokens a minute: start low, grow while it works
constexpr int kDims = 768;
constexpr int kRanked = 40;          // candidates each ranking contributes to the fusion
constexpr int kFusionK = 60;         // reciprocal rank fusion constant
constexpr quint32 kMagic = 0x51434958; // "QCIX"
constexpr quint32 kVersion = 1;
const char kModel[] = "gemini-embedding-2";

QString endpoint(const char *method)
{
    return QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models/%1:%2").arg(QLatin1String(kModel), QLatin1String(method));
}

QJsonObject embedRequest(const QString &text)
{
    return QJsonObject{{"model", QStringLiteral("models/%1").arg(QLatin1String(kModel))},
                       {"content", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", text}}}}}},
                       {"outputDimensionality", kDims}};
}

bool inScope(const QString &rel, const QString &scope)
{
    return scope.isEmpty() || rel == scope || rel.startsWith(scope + QLatin1Char('/'));
}

QStringList linesOf(const QString &text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines;
}

struct JobTask {
    QString rel;
    qint64 mtime = -1, size = -1;
    bool force = false;
};
struct JobResult {
    QString rel;
    bool exists = true;
    CodeIndex::FileData data;
};

} // namespace

CodeIndex::CodeIndex(QObject *parent)
    : QObject(parent)
    , m_net(new QNetworkAccessManager(this))
    , m_pumpTimer(new QTimer(this))
    , m_saveTimer(new QTimer(this))
{
    m_pumpTimer->setSingleShot(true);
    connect(m_pumpTimer, &QTimer::timeout, this, &CodeIndex::pump);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(8000);
    connect(m_saveTimer, &QTimer::timeout, this, &CodeIndex::saveVectors);
}

CodeIndex::~CodeIndex()
{
    ++m_generation;
    if (m_reply)
        m_reply->abort();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    saveVectors();
}

// --- files ----------------------------------------------------------------------------------------------------------

void CodeIndex::indexText(const QString &rel, const QString &text, FileData *out)
{
    out->sections.clear();
    const QStringList lines = linesOf(text);
    if (lines.isEmpty() || text.size() / lines.size() > 400) // minified or generated
        return;
    const QStringList pathTerms = terms(rel);
    const QVector<Section> sections = split(lines, isProse(rel));
    out->sections.reserve(sections.size());
    for (const Section &s : sections) {
        QHash<quint32, int> counts;
        QString body;
        for (int i = s.start - 1; i < s.end; ++i) {
            body += lines.at(i).left(400);
            body += QLatin1Char('\n');
        }
        for (const QString &t : terms(body))
            counts[termHash(t)] += 1;
        for (const QString &t : terms(s.symbols.join(QLatin1Char(' '))))
            counts[termHash(t)] += 3; // what the section declares says most about it
        for (const QString &t : pathTerms)
            counts[termHash(t)] += 2;
        Entry e;
        e.section = s;
        e.terms.reserve(counts.size());
        for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
            e.terms.append((quint64(it.key()) << 32) | quint32(qMin(it.value(), 0xffff)));
            e.length += it.value();
        }
        std::sort(e.terms.begin(), e.terms.end());
        e.key = keyOf(embedText(rel, s, lines));
        out->sections.append(e);
    }
}

bool CodeIndex::indexFile(const QString &root, const QString &rel, FileData *out)
{
    const QFileInfo fi(root + QLatin1Char('/') + rel);
    if (!fi.isFile())
        return false;
    out->mtime = fi.lastModified().toMSecsSinceEpoch();
    out->size = fi.size();
    out->sections.clear();
    if (fi.size() > kMaxFileBytes || fi.size() == 0)
        return true;
    QFile f(fi.filePath());
    if (!f.open(QIODevice::ReadOnly))
        return true;
    const QByteArray data = f.readAll();
    if (data.left(8000).contains('\0'))
        return true;
    QString text = QString::fromUtf8(data);
    text.remove(QLatin1Char('\r'));
    indexText(rel, text, out);
    return true;
}

void CodeIndex::setProject(const QString &root, const QString &cacheFile)
{
    if (root == m_root && cacheFile == m_cacheFile)
        return;
    saveVectors();
    if (m_reply)
        m_reply->abort();
    ++m_generation; // a running job's result is dropped
    m_root = root;
    m_cacheFile = cacheFile;
    m_wanted.clear();
    m_wantedSet.clear();
    m_files.clear();
    m_df.clear();
    m_totalLength = 0;
    m_sectionCount = 0;
    m_fullPass = false;
    m_forced.clear();
    m_vectors.clear();
    m_skip.clear();
    m_queryCache.clear();
    m_error.clear();
    m_blocked = false;
    m_vectorsDirty = false;
    m_batchChars = kBatchCharsStart;
    m_gapMs = 700;
    loadVectors();
    emit statusChanged();
}

void CodeIndex::setFiles(const QStringList &absolute)
{
    if (m_root.isEmpty())
        return;
    const QDir dir(m_root);
    QStringList wanted;
    for (const QString &abs : absolute) {
        const QString rel = dir.relativeFilePath(abs);
        if (!rel.startsWith(QLatin1String("..")) && indexableName(rel))
            wanted << rel;
        if (wanted.size() >= kMaxFiles)
            break;
    }
    if (wanted.isEmpty() && absolute.isEmpty())
        return; // the file index is still scanning
    m_wanted = wanted;
    m_wantedSet = QSet<QString>(wanted.cbegin(), wanted.cend());
    const QStringList known = m_files.keys();
    for (const QString &rel : known)
        if (!m_wantedSet.contains(rel))
            dropFile(rel);
    startJob();
}

void CodeIndex::fileChanged(const QString &absolute)
{
    if (m_root.isEmpty())
        return;
    const QString rel = QDir(m_root).relativeFilePath(absolute);
    if (rel.startsWith(QLatin1String("..")) || !indexableName(rel))
        return;
    if (!m_wantedSet.contains(rel)) { // a new file the file index has not picked up yet
        m_wanted << rel;
        m_wantedSet.insert(rel);
    }
    m_forced.insert(rel);
    startJob();
}

void CodeIndex::rebuild()
{
    const QStringList known = m_files.keys();
    for (const QString &rel : known)
        dropFile(rel);
    m_fullPass = false;
    m_skip.clear();
    m_error.clear();
    m_blocked = false;
    startJob();
    emit statusChanged();
}

void CodeIndex::startJob()
{
    if (m_root.isEmpty())
        return;
    if (m_thread) {
        m_again = true;
        return;
    }
    QVector<JobTask> tasks;
    tasks.reserve(m_wanted.size());
    for (const QString &rel : std::as_const(m_wanted)) {
        JobTask t;
        t.rel = rel;
        const auto it = m_files.constFind(rel);
        if (it != m_files.cend()) {
            t.mtime = it->mtime;
            t.size = it->size;
        }
        t.force = m_forced.contains(rel);
        tasks.append(t);
    }
    m_forced.clear();
    const QString root = m_root;
    const int gen = m_generation;
    auto results = std::make_shared<QVector<JobResult>>();
    m_thread = QThread::create([root, tasks, results] {
        for (const JobTask &t : tasks) {
            const QFileInfo fi(root + QLatin1Char('/') + t.rel);
            if (!t.force && fi.isFile() && fi.lastModified().toMSecsSinceEpoch() == t.mtime && fi.size() == t.size)
                continue;
            JobResult r;
            r.rel = t.rel;
            r.exists = indexFile(root, t.rel, &r.data);
            if (r.exists || t.mtime >= 0)
                results->append(std::move(r));
        }
    });
    connect(m_thread, &QThread::finished, this, [this, gen, results] {
        m_thread->deleteLater();
        m_thread = nullptr;
        if (gen == m_generation) {
            for (JobResult &r : *results) {
                if (!r.exists)
                    dropFile(r.rel);
                else if (m_wantedSet.contains(r.rel))
                    putFile(r.rel, std::move(r.data));
            }
            m_fullPass = true;
        }
        if (m_again) {
            m_again = false;
            startJob();
        }
        emit statusChanged();
        schedulePump();
    });
    emit statusChanged();
    m_thread->start(QThread::LowPriority);
}

void CodeIndex::putFile(const QString &rel, FileData data)
{
    dropFile(rel);
    for (const Entry &e : std::as_const(data.sections)) {
        for (quint64 t : e.terms)
            ++m_df[quint32(t >> 32)];
        m_totalLength += e.length;
        ++m_sectionCount;
    }
    m_files.insert(rel, std::move(data));
}

void CodeIndex::dropFile(const QString &rel)
{
    const auto it = m_files.constFind(rel);
    if (it == m_files.cend())
        return;
    for (const Entry &e : it->sections) {
        for (quint64 t : e.terms) {
            auto d = m_df.find(quint32(t >> 32));
            if (d != m_df.end() && --d.value() <= 0)
                m_df.erase(d);
        }
        m_totalLength -= e.length;
        --m_sectionCount;
    }
    m_files.remove(rel);
}

// Brings the sections in `scope` up to date with the disk before a search (the user or a command may have changed files).
void CodeIndex::refreshNow(const QString &scope)
{
    for (const QString &rel : std::as_const(m_wanted)) {
        if (!inScope(rel, scope))
            continue;
        const QFileInfo fi(m_root + QLatin1Char('/') + rel);
        const auto it = m_files.constFind(rel);
        if (it != m_files.cend() && fi.isFile() && fi.lastModified().toMSecsSinceEpoch() == it->mtime && fi.size() == it->size
            && !m_forced.contains(rel))
            continue;
        FileData data;
        if (indexFile(m_root, rel, &data))
            putFile(rel, std::move(data));
        else
            dropFile(rel);
        m_forced.remove(rel);
    }
}

bool CodeIndex::waitForJob(int ms)
{
    if (!m_thread)
        return true;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &CodeIndex::statusChanged, &loop, [this, &loop] {
        if (!m_thread)
            loop.quit();
    });
    timer.start(ms);
    loop.exec();
    return !m_thread;
}

// --- search ---------------------------------------------------------------------------------------------------------

double CodeIndex::idf(quint32 term) const
{
    const double n = qMax(1, m_sectionCount);
    const double df = m_df.value(term, 0);
    return std::log(1.0 + (n - df + 0.5) / (df + 0.5));
}

double CodeIndex::bm25(const Entry &e, const QVector<quint32> &query) const
{
    constexpr double k1 = 1.2, b = 0.75;
    const double avg = m_sectionCount > 0 ? double(m_totalLength) / m_sectionCount : 1.0;
    double score = 0;
    for (quint32 q : query) {
        const quint64 low = quint64(q) << 32;
        const auto it = std::lower_bound(e.terms.cbegin(), e.terms.cend(), low);
        if (it == e.terms.cend() || quint32(*it >> 32) != q)
            continue;
        const double tf = double(*it & 0xffffffffu);
        score += idf(q) * tf * (k1 + 1) / (tf + k1 * (1 - b + b * e.length / avg));
    }
    return score;
}

QVector<CodeIndex::Hit> CodeIndex::search(const QString &query, const QString &scopeIn, int max, const QHash<QString, QString> &overlay, QString *note)
{
    QStringList notes;
    if (m_root.isEmpty()) {
        *note = QStringLiteral("no project is open");
        return {};
    }
    QString scope = QDir::cleanPath(scopeIn.trimmed());
    if (scope == QLatin1String(".") || scope == QLatin1String("/"))
        scope.clear();

    if (m_thread && !waitForJob(30000))
        notes << QStringLiteral("the index is still being built; results cover the files indexed so far");

    // Meaning first: embedding the query runs an event loop, and nothing may change the sections while they are ranked.
    QVector<float> qvec;
    const bool meaningWanted = embeddingsEnabled() && !m_blocked && !m_vectors.isEmpty();
    if (meaningWanted) {
        QString err;
        if (!embedQuery(query, &qvec, &err))
            notes << QStringLiteral("meaning search unavailable (%1); keyword ranking only").arg(err);
    }
    if (!m_thread)
        refreshNow(scope);

    struct Ref {
        const QString *path;
        const Entry *entry;
    };
    QVector<Ref> refs;
    QHash<QString, FileData> buffers; // unsaved editor buffers replace their file
    for (auto it = overlay.cbegin(); it != overlay.cend(); ++it) {
        if (!inScope(it.key(), scope) || !indexableName(it.key()))
            continue;
        FileData data;
        indexText(it.key(), it.value(), &data);
        buffers.insert(it.key(), std::move(data));
    }
    for (auto it = m_files.cbegin(); it != m_files.cend(); ++it)
        if (inScope(it.key(), scope) && !buffers.contains(it.key()))
            for (const Entry &e : it->sections)
                refs.append({&it.key(), &e});
    for (auto it = buffers.cbegin(); it != buffers.cend(); ++it)
        for (const Entry &e : it->sections)
            refs.append({&it.key(), &e});
    if (refs.isEmpty()) {
        *note = scope.isEmpty() ? QStringLiteral("nothing indexed yet") : QStringLiteral("nothing indexed below '%1'").arg(scope);
        return {};
    }

    // keywords
    QVector<quint32> qterms;
    for (const QString &t : terms(query)) {
        const quint32 h = termHash(t);
        if (!qterms.contains(h))
            qterms.append(h);
    }
    QVector<QPair<double, int>> byKeyword;
    if (!qterms.isEmpty()) {
        for (int i = 0; i < refs.size(); ++i) {
            const double s = bm25(*refs.at(i).entry, qterms);
            if (s > 0)
                byKeyword.append({s, i});
        }
        std::sort(byKeyword.begin(), byKeyword.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        byKeyword.resize(qMin(byKeyword.size(), qsizetype(kRanked)));
    }

    // meaning
    QVector<QPair<double, int>> byMeaning;
    int withVector = 0;
    if (qvec.size() == kDims) {
        for (int i = 0; i < refs.size(); ++i) {
            const auto v = m_vectors.constFind(refs.at(i).entry->key);
            if (v == m_vectors.cend())
                continue;
            ++withVector;
            const qint8 *q = reinterpret_cast<const qint8 *>(v->q.constData());
            double dot = 0;
            for (int d = 0; d < kDims; ++d)
                dot += double(q[d]) * qvec.at(d);
            byMeaning.append({dot * v->scale, i});
        }
        std::sort(byMeaning.begin(), byMeaning.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        byMeaning.resize(qMin(byMeaning.size(), qsizetype(kRanked)));
        if (withVector < refs.size() * 9 / 10)
            notes << QStringLiteral("meaning search covers %1 of %2 sections so far; the rest ranked by keywords").arg(withVector).arg(refs.size());
    } else if (!embeddingsEnabled()) {
        // keyword ranking only, by the user's choice: nothing to say
    } else if (apiKey().isEmpty()) {
        notes << QStringLiteral("keyword ranking only (no Gemini key for meaning search)");
    } else if (m_vectors.isEmpty() && notes.isEmpty()) {
        notes << QStringLiteral("keyword ranking only; meaning search is still embedding the project");
    }

    // fusion
    QHash<int, double> fused;
    QSet<int> kw, mn;
    for (int r = 0; r < byKeyword.size(); ++r) {
        fused[byKeyword.at(r).second] += 1.0 / (kFusionK + r + 1);
        kw.insert(byKeyword.at(r).second);
    }
    for (int r = 0; r < byMeaning.size(); ++r) {
        fused[byMeaning.at(r).second] += 1.0 / (kFusionK + r + 1);
        mn.insert(byMeaning.at(r).second);
    }
    QVector<QPair<double, int>> order;
    for (auto it = fused.cbegin(); it != fused.cend(); ++it)
        order.append({it.value(), it.key()});
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });

    QVector<Hit> hits;
    for (const auto &o : std::as_const(order)) {
        if (hits.size() >= max)
            break;
        const Ref &ref = refs.at(o.second);
        Hit h;
        h.path = *ref.path;
        h.start = ref.entry->section.start;
        h.end = ref.entry->section.end;
        h.symbol = ref.entry->section.symbol;
        h.keyword = kw.contains(o.second);
        h.meaning = mn.contains(o.second);
        hits.append(h);
    }
    *note = notes.join(QStringLiteral("; "));
    return hits;
}

CodeIndex::Status CodeIndex::status() const
{
    Status s;
    s.building = m_thread != nullptr || (!m_root.isEmpty() && !m_fullPass);
    s.files = 0;
    s.sections = m_sectionCount;
    for (auto it = m_files.cbegin(); it != m_files.cend(); ++it) {
        if (!it->sections.isEmpty())
            ++s.files;
        for (const Entry &e : it->sections)
            if (m_vectors.contains(e.key))
                ++s.embedded;
    }
    s.embeddingsOn = embeddingsEnabled();
    s.hasKey = !apiKey().isEmpty();
    s.embedding = m_reply != nullptr || (s.embeddingsOn && s.hasKey && !m_blocked && s.embedded < s.sections && m_error.isEmpty());
    s.error = m_error;
    return s;
}

// --- embeddings -----------------------------------------------------------------------------------------------------

bool CodeIndex::embeddingsEnabled()
{
    return SettingsManager::instance().codeEmbeddings();
}

void CodeIndex::setEmbeddingsEnabled(bool on)
{
    SettingsManager::instance().setCodeEmbeddings(on);
    if (!on && m_reply)
        m_reply->abort();
    m_error.clear();
    m_blocked = false;
    schedulePump(300);
    emit statusChanged();
}

void CodeIndex::keyChanged()
{
    m_blocked = false;
    m_error.clear();
    m_skip.clear();
    m_queryCache.clear();
    schedulePump(300);
    emit statusChanged();
}

QString CodeIndex::apiKey() const
{
    return m_root.isEmpty() ? QString() : AiEnv::apiKey(AiEnv::Provider::Gemini, m_root);
}

void CodeIndex::schedulePump(int ms)
{
    if (!m_pumpTimer->isActive() || m_pumpTimer->remainingTime() > ms)
        m_pumpTimer->start(ms);
}

CodeIndex::Vec CodeIndex::quantize(const QJsonArray &values)
{
    Vec v;
    double norm = 0, top = 0;
    for (const QJsonValue &x : values) {
        norm += x.toDouble() * x.toDouble();
        top = qMax(top, std::abs(x.toDouble()));
    }
    norm = std::sqrt(norm);
    if (norm <= 0 || top <= 0)
        return v;
    const double scale = top / norm / 127.0;
    v.scale = float(scale);
    v.q.resize(values.size());
    for (int i = 0; i < values.size(); ++i)
        v.q[i] = char(qBound(-127, int(std::lround(values.at(i).toDouble() / norm / scale)), 127));
    return v;
}

// Sends the next batch of sections that have no vector yet; runs again after every answer until all are embedded.
void CodeIndex::pump()
{
    if (m_reply || m_thread || m_root.isEmpty() || m_blocked || !embeddingsEnabled())
        return;
    const QString key = apiKey();
    if (key.isEmpty())
        return;
    if (m_vectors.size() >= kMaxVectors) {
        m_error = tr("The project is too large: meaning search stopped at %1 sections.").arg(kMaxVectors);
        emit statusChanged();
        return;
    }

    // Pick sections without a vector, a file at a time, and rebuild the text each was keyed from.
    QJsonArray requests;
    QVector<QByteArray> keys;
    int chars = 0;
    QSet<QByteArray> queued;
    for (auto it = m_files.cbegin(); it != m_files.cend() && requests.size() < kBatch && chars < m_batchChars; ++it) {
        bool needs = false;
        for (const Entry &e : it->sections)
            needs = needs || (!m_vectors.contains(e.key) && !m_skip.contains(e.key));
        if (!needs)
            continue;
        QFile f(m_root + QLatin1Char('/') + it.key());
        if (!f.open(QIODevice::ReadOnly)) {
            for (const Entry &e : it->sections)
                m_skip.insert(e.key);
            continue;
        }
        QString text = TextEncoding::decode(f.readAll()).text;
        text.remove(QLatin1Char('\r'));
        const QStringList lines = linesOf(text);
        for (const Entry &e : it->sections) {
            if (m_vectors.contains(e.key) || m_skip.contains(e.key) || queued.contains(e.key))
                continue;
            if (requests.size() >= kBatch || chars >= m_batchChars)
                break;
            const QString t = embedText(it.key(), e.section, lines);
            if (!requests.isEmpty() && chars + t.size() > m_batchChars)
                break;
            if (keyOf(t) != e.key) { // the file changed since it was indexed; the next refresh re-keys it
                m_skip.insert(e.key);
                continue;
            }
            requests.append(embedRequest(t));
            keys.append(e.key);
            queued.insert(e.key);
            chars += t.size();
        }
    }
    if (requests.isEmpty()) {
        if (m_vectorsDirty)
            m_saveTimer->start(500);
        emit statusChanged();
        return;
    }

    QNetworkRequest req{QUrl(endpoint("batchEmbedContents"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("x-goog-api-key", key.toUtf8());
    req.setTransferTimeout(120000);
    m_reply = m_net->post(req, QJsonDocument(QJsonObject{{"requests", requests}}).toJson(QJsonDocument::Compact));
    const int gen = m_generation;
    QNetworkReply *reply = m_reply;
    emit statusChanged();
    connect(reply, &QNetworkReply::finished, this, [this, reply, keys, gen] {
        reply->deleteLater();
        if (m_reply == reply)
            m_reply = nullptr;
        if (gen != m_generation)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() == QNetworkReply::OperationCanceledError) {
            emit statusChanged();
            return;
        }
        if (status == 200) {
            const QJsonArray embeddings = body.value("embeddings").toArray();
            for (int i = 0; i < keys.size() && i < embeddings.size(); ++i) {
                const Vec v = quantize(embeddings.at(i).toObject().value("values").toArray());
                if (v.q.size() == kDims)
                    m_vectors.insert(keys.at(i), v);
                else
                    m_skip.insert(keys.at(i));
            }
            m_vectorsDirty = true;
            m_error.clear();
            if (!m_saveTimer->isActive())
                m_saveTimer->start();
            m_batchChars = qMin(kBatchCharsMax, m_batchChars * 5 / 4);
            m_gapMs = qMax(700, m_gapMs * 3 / 4);
            schedulePump(m_gapMs);
        } else if (status == 429 || status == 500 || status == 503) {
            const int wait = qMax(GeminiClient::retryDelayOf(body), status == 429 ? 30 : 15);
            if (status == 429) { // smaller batches, further apart: leaves room for the queries of a running chat
                m_batchChars = qMax(kBatchCharsMin, m_batchChars / 2);
                m_gapMs = qMin(30000, m_gapMs * 2);
            }
            m_error = status == 429 ? tr("Rate limited by Gemini, waiting %1 s").arg(wait) : QString();
            schedulePump(wait * 1000);
        } else if (status == 400 || status == 401 || status == 403) {
            const QString msg = body.value("error").toObject().value("message").toString();
            if (status != 400 || msg.contains(QLatin1String("API key"), Qt::CaseInsensitive)) {
                m_blocked = true; // the key does not work: wait for a new one
            } else {
                for (const QByteArray &k : keys)
                    m_skip.insert(k);
                schedulePump(2000);
            }
            m_error = GeminiClient::errorText(body, status, reply->errorString(), QLatin1String(kModel));
        } else {
            m_error = tr("Embedding request failed: %1").arg(status ? QString::number(status) : reply->errorString());
            schedulePump(60000);
        }
        emit statusChanged();
    });
}

bool CodeIndex::embedQuery(const QString &query, QVector<float> *out, QString *error)
{
    const QString text = QStringLiteral("task: code retrieval | query: %1").arg(query.simplified());
    if (m_queryCache.contains(text)) {
        *out = m_queryCache.value(text);
        return true;
    }
    const QString key = apiKey();
    if (key.isEmpty()) {
        *error = QStringLiteral("no Gemini key");
        return false;
    }
    QNetworkRequest req{QUrl(endpoint("embedContent"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("x-goog-api-key", key.toUtf8());
    req.setTransferTimeout(12000);
    QJsonObject payload = embedRequest(text);
    payload.remove("model");
    QNetworkReply *reply = m_net->post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
    if (status != 200) {
        if (status == 429)
            *error = QStringLiteral("Gemini rate limit reached");
        else if (status)
            *error = QStringLiteral("HTTP %1: %2").arg(status).arg(body.value("error").toObject().value("message").toString().section(QLatin1Char('\n'), 0, 0).left(120));
        else
            *error = reply->errorString();
        return false;
    }
    const Vec v = quantize(body.value("embedding").toObject().value("values").toArray());
    if (v.q.size() != kDims) {
        *error = QStringLiteral("unexpected answer from the embedding model");
        return false;
    }
    out->resize(kDims);
    for (int d = 0; d < kDims; ++d)
        (*out)[d] = float(qint8(v.q.at(d)) * v.scale);
    if (m_queryCache.size() > 64)
        m_queryCache.clear();
    m_queryCache.insert(text, *out);
    return true;
}

void CodeIndex::loadVectors()
{
    if (m_cacheFile.isEmpty())
        return;
    QFile f(m_cacheFile);
    if (!f.open(QIODevice::ReadOnly))
        return;
    QDataStream in(&f);
    quint32 magic = 0, version = 0;
    QString model;
    qint32 dims = 0, count = 0;
    in >> magic >> version >> model >> dims >> count;
    if (magic != kMagic || version != kVersion || model != QLatin1String(kModel) || dims != kDims || count < 0)
        return; // another model's vectors cannot be compared with this one's
    m_vectors.reserve(count);
    for (int i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
        QByteArray key;
        Vec v;
        in >> key >> v.scale >> v.q;
        if (v.q.size() == kDims)
            m_vectors.insert(key, v);
    }
}

void CodeIndex::saveVectors()
{
    m_saveTimer->stop();
    if (!m_vectorsDirty || m_cacheFile.isEmpty())
        return;
    // Once every file has been looked at, vectors of sections that no longer exist are dropped.
    QSet<QByteArray> live;
    if (m_fullPass)
        for (auto it = m_files.cbegin(); it != m_files.cend(); ++it)
            for (const Entry &e : it->sections)
                live.insert(e.key);
    QDir().mkpath(QFileInfo(m_cacheFile).absolutePath());
    QSaveFile f(m_cacheFile);
    if (!f.open(QIODevice::WriteOnly))
        return;
    QDataStream out(&f);
    qint32 count = 0;
    for (auto it = m_vectors.cbegin(); it != m_vectors.cend(); ++it)
        count += !m_fullPass || live.contains(it.key());
    out << kMagic << kVersion << QString::fromLatin1(kModel) << qint32(kDims) << count;
    for (auto it = m_vectors.cbegin(); it != m_vectors.cend(); ++it)
        if (!m_fullPass || live.contains(it.key()))
            out << it.key() << it->scale << it->q;
    if (f.commit())
        m_vectorsDirty = false;
}
