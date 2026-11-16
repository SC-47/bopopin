#include "backend.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QVariantMap>
#include <algorithm>

namespace {

// A pause longer than this is a break, not slow typing, and doesn't count
// against the speed.
const qint64 longestGapMs = 5000;

// A missed word comes back after this many others.
const int retryAfter = 3;

// How a sound's weakness moves, and how hard it pulls its words forward.
const double weakestItGets = 5;
const double cleanAnswerKeeps = 0.7;
const double forgottenBelow = 0.2;
const double pullPerWeakness = 2;

} // namespace

Backend::Backend(const QString &wordsPath, const QString &settingsPath, quint32 seed, QObject *parent)
    : QObject(parent),
      m_words(loadWords(wordsPath)),
      m_settingsPath(settingsPath.isEmpty()
                         ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/settings.ini"
                         : settingsPath),
      m_random(seed ? QRandomGenerator(seed) : QRandomGenerator::securelySeeded()) {
    const QSettings settings(m_settingsPath, QSettings::IniFormat);
    m_lesson = qBound(0, settings.value("lesson", 0).toInt(), int(lessons().size()) - 1);
    m_hints = settings.value("hints", false).toBool();
    const QJsonObject weak = QJsonDocument::fromJson(settings.value("weak").toString().toUtf8()).object();
    for (auto it = weak.begin(); it != weak.end(); ++it)
        if (it.value().toDouble() >= forgottenBelow)
            m_weak.insert(it.key(), qMin(it.value().toDouble(), weakestItGets));
    rebuildPool();
    advance(false);
}

QList<Backend::Word> Backend::loadWords(const QString &path, QStringList *problems) {
    QList<Word> words;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (problems)
            *problems << "cannot read " + path;
        return words;
    }
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        Word word;
        word.text = line.section('\t', 0, 0);
        // A character outside the basic plane is two QChars, so count by code point.
        for (const char32_t point : word.text.toUcs4())
            word.characters << QString::fromUcs4(&point, 1);
        bool valid = true;
        for (const QString &zhuyin : line.section('\t', 1).split(' ', Qt::SkipEmptyParts)) {
            word.syllables << Zhuyin::convert(zhuyin);
            valid = valid && word.syllables.last().valid;
        }
        if (valid && !word.syllables.isEmpty() && word.syllables.size() == word.characters.size())
            words << word;
        else if (problems)
            *problems << "not a word and its zhuyin: " + line;
    }
    return words;
}

void Backend::setLesson(int lesson) {
    lesson = qBound(0, lesson, int(lessons().size()) - 1);
    if (lesson == m_lesson)
        return;
    m_lesson = lesson;
    save();
    emit lessonChanged();
    rebuildPool();
    advance(false);
}

void Backend::setHints(bool hints) {
    if (hints == m_hints)
        return;
    m_hints = hints;
    save();
    emit hintsChanged();
}

void Backend::save() const {
    QDir().mkpath(QFileInfo(m_settingsPath).absolutePath());
    QSettings settings(m_settingsPath, QSettings::IniFormat);
    settings.setValue("lesson", m_lesson);
    settings.setValue("hints", m_hints);
    QJsonObject weak;
    for (auto it = m_weak.begin(); it != m_weak.end(); ++it)
        weak.insert(it.key(), it.value());
    settings.setValue("weak", QString::fromUtf8(QJsonDocument(weak).toJson(QJsonDocument::Compact)));
}

QString Backend::sound(const ZhuyinPart &part) {
    return part.zhuyin + " → " + part.pinyin;
}

// The three weakest sounds, weakest first.
QStringList Backend::weakest() const {
    QStringList sounds;
    for (auto it = m_weak.begin(); it != m_weak.end(); ++it)
        if (it.value() >= 0.5)
            sounds << it.key();
    std::sort(sounds.begin(), sounds.end(), [this](const QString &a, const QString &b) {
        return m_weak.value(a) != m_weak.value(b) ? m_weak.value(a) > m_weak.value(b) : a < b;
    });
    return sounds.mid(0, 3);
}

// Counts the sound the typing has reached as missed; asking for the hint
// counts every sound still to come in the syllable.
void Backend::miss(bool andTheRest) {
    const QList<ZhuyinPart> &parts = m_words.at(m_current).syllables.at(m_position).parts;
    int reached = 0;
    for (int end = 0; reached < parts.size() - 1; ++reached) {
        end += parts.at(reached).pinyin.size();
        if (m_typed < end)
            break;
    }
    for (int i = reached; i < (andTheRest ? parts.size() : reached + 1); ++i) {
        if (m_penalized.contains(i))
            continue;
        m_penalized.insert(i);
        double &weak = m_weak[sound(parts.at(i))];
        weak = qMin(weak + 1, weakestItGets);
        m_weakDirty = true;
    }
    if (m_weakDirty)
        emit weakChanged();
}

// A finished syllable: each sound typed without a miss gets a little stronger.
void Backend::settle() {
    const QList<ZhuyinPart> &parts = m_words.at(m_current).syllables.at(m_position).parts;
    bool changed = false;
    for (int i = 0; i < parts.size(); ++i) {
        const auto it = m_weak.find(sound(parts.at(i)));
        if (m_penalized.contains(i) || it == m_weak.end())
            continue;
        *it *= cleanAnswerKeeps;
        if (*it < forgottenBelow)
            m_weak.erase(it);
        changed = true;
    }
    m_penalized.clear();
    if (changed) {
        m_weakDirty = true;
        emit weakChanged();
    }
}

double Backend::weight(int word) const {
    double weight = 1;
    for (const Syllable &syllable : m_words.at(word).syllables)
        for (const ZhuyinPart &part : syllable.parts)
            weight += pullPerWeakness * m_weak.value(sound(part));
    return weight;
}

QString Backend::word() const {
    return m_current < 0 ? QString() : m_words.at(m_current).text;
}

int Backend::speed() const {
    // Too little typing yet for the number to mean anything.
    return m_activeMs < 2000 ? 0 : int(m_done * 60000 / m_activeMs);
}

void Backend::rebuildPool() {
    m_pool.clear();
    m_retries.clear();
    for (int i = 0; i < m_words.size(); ++i)
        for (const Syllable &syllable : m_words.at(i).syllables)
            if (Zhuyin::inLesson(syllable, m_lesson)) {
                m_pool << i;
                break;
            }
}

// The next word: one that was missed a few words ago if it is due, and
// otherwise any other from the lesson, the likelier the weaker its sounds,
// never the same one twice in a row unless the lesson has only one.
void Backend::advance(bool again) {
    if (again && m_current >= 0)
        m_retries.append({m_current, retryAfter + 1});
    for (Retry &retry : m_retries)
        --retry.due;

    int next = -1;
    if (!m_retries.isEmpty() && m_retries.first().due <= 0) {
        next = m_retries.takeFirst().word;
    } else if (!m_pool.isEmpty()) {
        const bool others = m_pool.size() > 1;
        double total = 0;
        for (const int word : m_pool)
            if (!others || word != m_current)
                total += weight(word);
        double at = m_random.generateDouble() * total;
        for (const int word : m_pool) {
            if (others && word == m_current)
                continue;
            next = word;
            at -= weight(word);
            if (at < 0)
                break;
        }
    }
    m_current = next;
    m_penalized.clear();
    if (m_weakDirty) {
        m_weakDirty = false;
        save();
    }

    m_syllables.clear();
    m_misses.clear();
    m_revealed.clear();
    if (m_current >= 0) {
        const Word &word = m_words.at(m_current);
        for (int i = 0; i < word.syllables.size(); ++i) {
            const Syllable &syllable = word.syllables.at(i);
            QVariantList parts;
            for (const ZhuyinPart &part : syllable.parts)
                parts << QVariantMap{{"zhuyin", part.zhuyin}, {"pinyin", part.pinyin}};
            m_syllables << QVariantMap{{"character", word.characters.at(i)},
                                       {"zhuyin", syllable.zhuyin},
                                       {"pinyin", syllable.pinyin},
                                       {"keys", syllable.keys},
                                       {"parts", parts}};
            m_misses << false;
            m_revealed << false;
        }
    }
    m_position = 0;
    m_typed = 0;
    m_wordMissed = false;
    emit wordChanged();
    emit progressChanged();
}

bool Backend::type(const QString &text) {
    bool allRight = true;
    for (const QChar letter : text.toLower()) {
        if (m_current < 0 || letter < 'a' || letter > 'z')
            continue;

        if (m_sinceKey.isValid())
            m_activeMs += qMin(m_sinceKey.elapsed(), longestGapMs);
        m_sinceKey.start();

        const Syllable &syllable = m_words.at(m_current).syllables.at(m_position);
        if (syllable.keys.at(m_typed) != letter) {
            allRight = false;
            m_misses[m_position] = true;
            m_wordMissed = true;
            m_streak = 0;
            miss(false);
            emit progressChanged();
            emit statsChanged();
            emit wrongKey();
            continue;
        }

        if (++m_typed < syllable.keys.size()) {
            emit progressChanged();
            continue;
        }
        ++m_done;
        settle();
        if (!m_misses.at(m_position).toBool()) {
            ++m_correct;
            ++m_streak;
        }
        m_typed = 0;
        if (++m_position < m_words.at(m_current).syllables.size())
            emit progressChanged();
        else
            advance(m_wordMissed);
        emit statsChanged();
    }
    return allRight;
}

void Backend::reveal() {
    if (m_current < 0 || m_revealed.at(m_position).toBool())
        return;
    m_revealed[m_position] = true;
    m_misses[m_position] = true;
    m_wordMissed = true;
    m_streak = 0;
    miss(true);
    emit progressChanged();
    emit statsChanged();
}

void Backend::skip() {
    if (m_current >= 0)
        advance(true);
}

void Backend::resetStats() {
    m_done = m_correct = m_streak = 0;
    m_activeMs = 0;
    m_sinceKey.invalidate();
    m_weak.clear();
    m_penalized.clear();
    m_weakDirty = false;
    save();
    emit statsChanged();
    emit weakChanged();
}
