#include "zhuyin.h"

#include <QHash>
#include <QStringList>

namespace {

// "ㄅ=b ㄆ=p" into a lookup table.
QHash<QString, QString> table(const QString &pairs) {
    QHash<QString, QString> map;
    for (const QString &pair : pairs.split(' ', Qt::SkipEmptyParts))
        map.insert(pair.section('=', 0, 0), pair.section('=', 1));
    return map;
}

const QHash<QString, QString> &initials() {
    static const auto map = table(QStringLiteral(
        "ㄅ=b ㄆ=p ㄇ=m ㄈ=f ㄉ=d ㄊ=t ㄋ=n ㄌ=l ㄍ=g ㄎ=k ㄏ=h ㄐ=j ㄑ=q ㄒ=x "
        "ㄓ=zh ㄔ=ch ㄕ=sh ㄖ=r ㄗ=z ㄘ=c ㄙ=s"));
    return map;
}

// What follows an initial.
const QHash<QString, QString> &rimes() {
    static const auto map = table(QStringLiteral(
        "ㄚ=a ㄛ=o ㄜ=e ㄝ=e ㄞ=ai ㄟ=ei ㄠ=ao ㄡ=ou ㄢ=an ㄣ=en ㄤ=ang ㄥ=eng ㄦ=er "
        "ㄧ=i ㄧㄚ=ia ㄧㄛ=io ㄧㄝ=ie ㄧㄞ=iai ㄧㄠ=iao ㄧㄡ=iu ㄧㄢ=ian ㄧㄣ=in ㄧㄤ=iang ㄧㄥ=ing "
        "ㄨ=u ㄨㄚ=ua ㄨㄛ=uo ㄨㄞ=uai ㄨㄟ=ui ㄨㄢ=uan ㄨㄣ=un ㄨㄤ=uang ㄨㄥ=ong "
        "ㄩ=ü ㄩㄝ=üe ㄩㄢ=üan ㄩㄣ=ün ㄩㄥ=iong"));
    return map;
}

// A syllable with no initial spells its medial as y or w.
const QHash<QString, QString> &wholes() {
    static const auto map = table(QStringLiteral(
        "ㄚ=a ㄛ=o ㄜ=e ㄝ=e ㄞ=ai ㄟ=ei ㄠ=ao ㄡ=ou ㄢ=an ㄣ=en ㄤ=ang ㄥ=eng ㄦ=er "
        "ㄧ=yi ㄧㄚ=ya ㄧㄛ=yo ㄧㄝ=ye ㄧㄞ=yai ㄧㄠ=yao ㄧㄡ=you ㄧㄢ=yan ㄧㄣ=yin ㄧㄤ=yang ㄧㄥ=ying "
        "ㄨ=wu ㄨㄚ=wa ㄨㄛ=wo ㄨㄞ=wai ㄨㄟ=wei ㄨㄢ=wan ㄨㄣ=wen ㄨㄤ=wang ㄨㄥ=weng "
        "ㄩ=yu ㄩㄝ=yue ㄩㄢ=yuan ㄩㄣ=yun ㄩㄥ=yong"));
    return map;
}

const QString tones = QStringLiteral("ˊˇˋ˙");
const QString medials = QStringLiteral("ㄧㄨㄩ");
const QString buzzing = QStringLiteral("ㄓㄔㄕㄖㄗㄘㄙ");
const QString umlaut = QStringLiteral("ü");

} // namespace

Syllable Zhuyin::convert(const QString &zhuyin) {
    Syllable syllable;
    syllable.zhuyin = zhuyin;

    QString body;
    for (const QChar c : zhuyin)
        if (!tones.contains(c))
            body += c;
    if (body.isEmpty())
        return syllable;

    QString rest = body;
    if (initials().contains(body.left(1))) {
        syllable.initial = body.left(1);
        rest = body.mid(1);
    }
    if (!rest.isEmpty() && medials.contains(rest.at(0))) {
        syllable.medial = rest.left(1);
        syllable.final = rest.mid(1);
    } else {
        syllable.final = rest;
    }

    if (syllable.initial.isEmpty()) {
        const QString whole = wholes().value(rest);
        if (whole.isEmpty())
            return syllable;
        syllable.pinyin = whole;
        syllable.parts = {{rest, whole}};
    } else if (rest.isEmpty()) {
        // ㄓ on its own is zhi; ㄅ on its own is nothing.
        if (!buzzing.contains(syllable.initial))
            return syllable;
        syllable.pinyin = initials().value(syllable.initial) + "i";
        syllable.parts = {{syllable.initial, syllable.pinyin}};
    } else {
        QString rime = rimes().value(rest);
        if (rime.isEmpty())
            return syllable;
        // After j, q, and x the ü is written as a plain u.
        if (QStringLiteral("ㄐㄑㄒ").contains(syllable.initial))
            rime.replace(umlaut, "u");
        const QString initial = initials().value(syllable.initial);
        syllable.pinyin = initial + rime;
        syllable.parts = {{syllable.initial, initial}, {rest, rime}};
    }

    syllable.keys = syllable.pinyin;
    syllable.keys.replace(umlaut, "v");
    syllable.valid = true;
    return syllable;
}

const QStringList &Zhuyin::lessons() {
    static const QStringList lessons{
        QString(),
        QStringLiteral("ㄅㄆㄇㄈ ㄉㄊㄋㄌ"),
        QStringLiteral("ㄍㄎㄏ ㄐㄑㄒ"),
        QStringLiteral("ㄓㄔㄕㄖ ㄗㄘㄙ"),
        QStringLiteral("ㄚㄛㄜㄝ ㄞㄟㄠㄡ"),
        QStringLiteral("ㄢㄣㄤㄥㄦ"),
        QStringLiteral("ㄧ ㄨ ㄩ"),
        QStringLiteral("*"),
    };
    return lessons;
}

bool Zhuyin::inLesson(const Syllable &s, int lesson) {
    const QString &symbols = lessons().value(lesson);
    switch (lesson) {
    case 0:
        return true;
    case 1:
    case 2:
    case 3:
        return !s.initial.isEmpty() && symbols.contains(s.initial);
    case 4:
    case 5:
        return !s.final.isEmpty() && symbols.contains(s.final);
    case 6:
        // No initial, so the medial is spelled y, w, or yu.
        return s.initial.isEmpty() && !s.medial.isEmpty();
    case 7: {
        // Where the pinyin drops or changes a letter the zhuyin has: ü,
        // iu for ㄧㄡ, ui for ㄨㄟ, un for ㄨㄣ, and ong for ㄨㄥ.
        const QString rime = s.medial + s.final;
        if (s.medial == QStringLiteral("ㄩ"))
            return true;
        if (s.initial.isEmpty())
            return false;
        return rime == QStringLiteral("ㄧㄡ") || rime == QStringLiteral("ㄨㄟ") || rime == QStringLiteral("ㄨㄣ")
               || rime == QStringLiteral("ㄨㄥ");
    }
    }
    return false;
}
