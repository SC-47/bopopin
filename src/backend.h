#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QRandomGenerator>
#include <QSet>
#include <QStringList>
#include <QVariantList>

#include "zhuyin.h"

// The practice session: which word is up, how far into it the typing is,
// and the score. QML sends it letters and shows what it says.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList lessons READ lessons CONSTANT)
    Q_PROPERTY(int wordCount READ wordCount CONSTANT)
    Q_PROPERTY(int lesson READ lesson WRITE setLesson NOTIFY lessonChanged)
    Q_PROPERTY(bool hints READ hints WRITE setHints NOTIFY hintsChanged)
    Q_PROPERTY(QString word READ word NOTIFY wordChanged)
    Q_PROPERTY(QVariantList syllables READ syllables NOTIFY wordChanged)
    Q_PROPERTY(int position READ position NOTIFY progressChanged)
    Q_PROPERTY(int typed READ typed NOTIFY progressChanged)
    Q_PROPERTY(QVariantList misses READ misses NOTIFY progressChanged)
    Q_PROPERTY(QVariantList revealed READ revealed NOTIFY progressChanged)
    Q_PROPERTY(int done READ done NOTIFY statsChanged)
    Q_PROPERTY(int correct READ correct NOTIFY statsChanged)
    Q_PROPERTY(int streak READ streak NOTIFY statsChanged)
    Q_PROPERTY(int speed READ speed NOTIFY statsChanged)
    Q_PROPERTY(QStringList weakest READ weakest NOTIFY weakChanged)

public:
    struct Word {
        QString text;
        QStringList characters;
        QList<Syllable> syllables;
    };

    // Tests pass their own word list, a temp settings file, and a seed.
    explicit Backend(const QString &wordsPath = ":/words.tsv", const QString &settingsPath = {}, quint32 seed = 0,
                     QObject *parent = nullptr);

    // One word per line: the text, a tab, and a zhuyin syllable per character.
    static QList<Word> loadWords(const QString &path, QStringList *problems = nullptr);

    QStringList lessons() const { return Zhuyin::lessons(); }
    int wordCount() const { return m_words.size(); }
    int lesson() const { return m_lesson; }
    void setLesson(int lesson);
    bool hints() const { return m_hints; }
    void setHints(bool hints);

    QString word() const;
    QVariantList syllables() const { return m_syllables; }
    int position() const { return m_position; }
    int typed() const { return m_typed; }
    QVariantList misses() const { return m_misses; }
    QVariantList revealed() const { return m_revealed; }

    int done() const { return m_done; }
    int correct() const { return m_correct; }
    int streak() const { return m_streak; }
    int speed() const;

    // Every sound, as "ㄧㄡ → iu", has a weakness: up one for a miss, shrinking
    // with each clean answer. Words with weak sounds are picked more often.
    double weakness(const QString &sound) const { return m_weak.value(sound); }
    QStringList weakest() const;

    // Letters from the keyboard; true when every one was the next expected.
    Q_INVOKABLE bool type(const QString &text);
    // Shows the pinyin of the character in turn. Asking counts as a miss.
    Q_INVOKABLE void reveal();
    Q_INVOKABLE void skip();
    Q_INVOKABLE void resetStats();

signals:
    void lessonChanged();
    void hintsChanged();
    void wordChanged();
    void progressChanged();
    void statsChanged();
    void wrongKey();
    void weakChanged();

private:
    struct Retry {
        int word;
        int due;
    };

    void rebuildPool();
    void advance(bool again);
    void save() const;
    static QString sound(const ZhuyinPart &part);
    void miss(bool andTheRest);
    void settle();
    double weight(int word) const;

    QList<Word> m_words;
    QString m_settingsPath;
    QRandomGenerator m_random;
    QList<int> m_pool;
    QList<Retry> m_retries;
    int m_lesson = 0;
    bool m_hints = false;
    int m_current = -1;
    QVariantList m_syllables;
    int m_position = 0;
    int m_typed = 0;
    QVariantList m_misses;
    QVariantList m_revealed;
    bool m_wordMissed = false;
    int m_done = 0;
    int m_correct = 0;
    int m_streak = 0;
    qint64 m_activeMs = 0;
    QElapsedTimer m_sinceKey;
    QHash<QString, double> m_weak;
    // The parts of the syllable in turn already counted as missed, so four
    // wrong keys on one sound count once.
    QSet<int> m_penalized;
    bool m_weakDirty = false;
};
