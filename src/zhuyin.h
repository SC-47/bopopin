#pragma once

#include <QList>
#include <QString>

// One zhuyin symbol group and the pinyin letters it becomes, for the hint
// shown after a wrong key (ㄌ → l, ㄧㄡ → iu).
struct ZhuyinPart {
    QString zhuyin;
    QString pinyin;
};

// One syllable: its zhuyin as written (with its tone mark), the pinyin as it
// is spelled (with ü), and the keys an input method wants for it (v for ü).
struct Syllable {
    bool valid = false;
    QString zhuyin;
    QString pinyin;
    QString keys;
    QString initial;
    QString medial;
    QString final;
    QList<ZhuyinPart> parts;
};

namespace Zhuyin {

Syllable convert(const QString &zhuyin);

// The lessons, in order: "" is every word, "*" the spellings that differ
// most from zhuyin, and the rest name the symbols they drill.
const QStringList &lessons();
bool inLesson(const Syllable &syllable, int lesson);

} // namespace Zhuyin
