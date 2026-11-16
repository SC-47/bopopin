#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <QSignalSpy>

#include "backend.h"
#include "theme.h"
#include "translator.h"
#include "zhuyin.h"

namespace {

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

int lines(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
}

// A hyprctl that answers with response.json beside it and counts its calls.
QString fakeHyprctl(const QString &directory) {
    const QString command = directory + "/hyprctl";
    write(command, "#!/bin/bash\necho >> '" + directory.toUtf8() + "/calls'\ncat '" + directory.toUtf8()
                       + "/response.json'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return command;
}

QJsonObject readCatalog(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// The placeholders in a text, sorted: %1 to %99, and the count (%n, %Ln).
QStringList placeholders(const QString &text) {
    static const QRegularExpression placeholder("%L?(?:[1-9][0-9]?|n)");
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

// Every text the sources hand to qsTr() or tr(), and whether a call passes it
// a count. It walks the code a token at a time, so a string never hides a
// comment or the other way round, C++'s adjacent literals join, and QML's
// single quotes count. A text that isn't a literal can't be in a catalog.
QMap<QString, bool> translatables(const QString &folder, QStringList *problems) {
    QMap<QString, bool> found;
    for (const QFileInfo &info : QDir(folder).entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString code = QString::fromUtf8(file.readAll());
        qsizetype i = 0;
        auto literal = [&](QString *text) {
            const QChar quote = code[i++];
            for (; i < code.size() && code[i] != quote; ++i) {
                if (code[i] == '\\' && i + 1 < code.size()) {
                    const QChar escaped = code[++i];
                    const int digits = escaped == 'u' ? 4 : escaped == 'x' ? 2 : 0;
                    bool hex = false;
                    const ushort unit = code.mid(i + 1, digits).toUShort(&hex, 16);
                    if (digits && hex) {
                        *text += QChar(unit);
                        i += digits;
                    } else {
                        *text += escaped == 'n' ? QChar('\n') : escaped == 't' ? QChar('\t') : escaped;
                    }
                } else {
                    *text += code[i];
                }
            }
            ++i;
        };
        auto skipSpace = [&] {
            while (i < code.size() && code[i].isSpace())
                ++i;
        };
        while (i < code.size()) {
            const QChar c = code[i];
            if (code.mid(i, 2) == "//") {
                i = code.indexOf('\n', i);
                if (i < 0)
                    break;
            } else if (code.mid(i, 2) == "/*") {
                i = code.indexOf("*/", i + 2);
                if (i < 0)
                    break;
                i += 2;
            } else if (c == '"' || c == '\'') {
                QString skipped;
                literal(&skipped);
            } else if (c.isLetter() || c == '_') {
                const qsizetype start = i;
                while (i < code.size() && (code[i].isLetterOrNumber() || code[i] == '_'))
                    ++i;
                const QString word = code.mid(start, i - start);
                skipSpace();
                if ((word != "qsTr" && word != "tr") || i >= code.size() || code[i] != '(')
                    continue;
                ++i;
                skipSpace();
                if (i >= code.size() || (code[i] != '"' && code[i] != '\'')) {
                    *problems << info.fileName() + ": not a literal: " + code.mid(start, 40);
                    continue;
                }
                QString text;
                do {
                    literal(&text);
                    skipSpace();
                } while (i < code.size() && code[i] == '"');
                // The arguments after the text, to the call's closing parenthesis.
                int depth = 0, commas = 0;
                while (i < code.size()) {
                    const QChar d = code[i];
                    if (d == '"' || d == '\'') {
                        QString skipped;
                        literal(&skipped);
                        continue;
                    }
                    if (d == '(' || d == '[' || d == '{')
                        ++depth;
                    else if ((d == ')' || d == ']' || d == '}') && depth-- == 0)
                        break;
                    else if (d == ',' && depth == 0)
                        ++commas;
                    ++i;
                }
                found[text] = found.value(text) || commas >= 2;
            } else {
                ++i;
            }
        }
    }
    return found;
}

// Items a Repeater makes hang off the visual tree, not the QObject one that
// findChild() walks.
QQuickItem *findItem(QQuickItem *root, const QString &name) {
    if (root->objectName() == name)
        return root;
    for (QQuickItem *child : root->childItems())
        if (QQuickItem *found = findItem(child, name))
            return found;
    return nullptr;
}

} // namespace

class BopopinTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void zhuyinBecomesPinyin_data() {
        QTest::addColumn<QString>("zhuyin");
        QTest::addColumn<QString>("pinyin");
        QTest::addColumn<QString>("keys");
        const QList<QStringList> rows{
            {"ㄕㄨ", "shu", "shu"},       {"ㄖㄨˋ", "ru", "ru"},       {"ㄈㄚˇ", "fa", "fa"},
            {"ㄌㄧㄡˊ", "liu", "liu"},    {"ㄧㄡˇ", "you", "you"},     {"ㄉㄨㄟˋ", "dui", "dui"},
            {"ㄨㄟˋ", "wei", "wei"},      {"ㄔㄨㄣ", "chun", "chun"},  {"ㄨㄣˊ", "wen", "wen"},
            {"ㄓ", "zhi", "zhi"},         {"ㄙˋ", "si", "si"},         {"ㄗ˙", "zi", "zi"},
            {"ㄩˊ", "yu", "yu"},          {"ㄐㄩˊ", "ju", "ju"},       {"ㄒㄩㄝˊ", "xue", "xue"},
            {"ㄑㄩㄢˊ", "quan", "quan"},  {"ㄩㄢˊ", "yuan", "yuan"},   {"ㄋㄩˇ", "nü", "nv"},
            {"ㄌㄩˋ", "lü", "lv"},        {"ㄒㄩㄥˊ", "xiong", "xiong"}, {"ㄩㄥˋ", "yong", "yong"},
            {"ㄉㄨㄥ", "dong", "dong"},   {"ㄨㄥ", "weng", "weng"},    {"ㄦˊ", "er", "er"},
            {"ㄅㄛ", "bo", "bo"},         {"ㄈㄥ", "feng", "feng"},    {"ㄧㄥ", "ying", "ying"},
            {"ㄐㄧㄚ", "jia", "jia"},     {"ㄏㄨㄤˊ", "huang", "huang"}, {"ㄧㄝˋ", "ye", "ye"},
            {"ㄨㄛˇ", "wo", "wo"},        {"ㄇㄣ˙", "men", "men"},     {"ㄤˊ", "ang", "ang"},
        };
        for (const QStringList &row : rows)
            QTest::newRow(qPrintable(row.at(1) + " " + row.at(0))) << row.at(0) << row.at(1) << row.at(2);
    }

    void zhuyinBecomesPinyin() {
        QFETCH(QString, zhuyin);
        QFETCH(QString, pinyin);
        QFETCH(QString, keys);
        const Syllable syllable = Zhuyin::convert(zhuyin);
        QVERIFY(syllable.valid);
        QCOMPARE(syllable.pinyin, pinyin);
        QCOMPARE(syllable.keys, keys);
        QCOMPARE(syllable.zhuyin, zhuyin);
    }

    void zhuyinPartsExplainTheSpelling() {
        const Syllable liu = Zhuyin::convert("ㄌㄧㄡˊ");
        QCOMPARE(liu.parts.size(), 2);
        QCOMPARE(liu.parts.at(0).zhuyin + "=" + liu.parts.at(0).pinyin, QString("ㄌ=l"));
        QCOMPARE(liu.parts.at(1).zhuyin + "=" + liu.parts.at(1).pinyin, QString("ㄧㄡ=iu"));
        const Syllable zhi = Zhuyin::convert("ㄓ");
        QCOMPARE(zhi.parts.size(), 1);
        QCOMPARE(zhi.parts.at(0).pinyin, QString("zhi"));
        const Syllable you = Zhuyin::convert("ㄧㄡˇ");
        QCOMPARE(you.parts.size(), 1);
        QCOMPARE(you.parts.at(0).pinyin, QString("you"));
    }

    void nonsenseIsNotASyllable() {
        QVERIFY(!Zhuyin::convert("").valid);
        QVERIFY(!Zhuyin::convert("ㄅ").valid);
        QVERIFY(!Zhuyin::convert("shu").valid);
        QVERIFY(!Zhuyin::convert("ㄚㄚ").valid);
        QVERIFY(!Zhuyin::convert("ˊ").valid);
    }

    void everyShippedWordConverts() {
        QStringList problems;
        const QList<Backend::Word> words = Backend::loadWords(":/words.tsv", &problems);
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
        QVERIFY(words.size() > 300);
        // Every lesson has enough words to practice on.
        for (int lesson = 0; lesson < Zhuyin::lessons().size(); ++lesson) {
            int matching = 0;
            for (const Backend::Word &word : words)
                for (const Syllable &syllable : word.syllables)
                    if (Zhuyin::inLesson(syllable, lesson)) {
                        ++matching;
                        break;
                    }
            QVERIFY2(matching >= 30, qPrintable(QString("lesson %1 has %2 words").arg(lesson).arg(matching)));
        }
    }

    void typingWalksThroughTheWord() {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("words.tsv"), "朋友\tㄆㄥˊ ㄧㄡˇ\n"));
        Backend backend(dir.filePath("words.tsv"), dir.filePath("settings.ini"), 1);
        QCOMPARE(backend.word(), QString("朋友"));
        QCOMPARE(backend.syllables().size(), 2);

        QVERIFY(backend.type("pe"));
        QCOMPARE(backend.position(), 0);
        QCOMPARE(backend.typed(), 2);
        QVERIFY(backend.type("NG"));
        QCOMPARE(backend.position(), 1);
        QCOMPARE(backend.typed(), 0);
        QCOMPARE(backend.done(), 1);
        QCOMPARE(backend.correct(), 1);
        QCOMPARE(backend.streak(), 1);

        // A wrong key stays put, marks the syllable, and breaks the streak,
        // but the pinyin shows only once it is asked for.
        QSignalSpy wrong(&backend, &Backend::wrongKey);
        QVERIFY(!backend.type("i"));
        QCOMPARE(wrong.count(), 1);
        QCOMPARE(backend.typed(), 0);
        QVERIFY(backend.misses().at(1).toBool());
        QVERIFY(!backend.revealed().at(1).toBool());
        QCOMPARE(backend.streak(), 0);
        backend.reveal();
        QVERIFY(backend.revealed().at(1).toBool());

        // Finishing it counts as done but not correct, and the only word
        // there is comes round again, clean.
        QVERIFY(backend.type("you"));
        QCOMPARE(backend.done(), 2);
        QCOMPARE(backend.correct(), 1);
        QCOMPARE(backend.position(), 0);
        QVERIFY(!backend.misses().at(1).toBool());

        backend.resetStats();
        QCOMPARE(backend.done(), 0);
        QCOMPARE(backend.speed(), 0);
    }

    void aMissedWordComesBack() {
        QTemporaryDir dir;
        QByteArray list;
        for (const char *word : {"大", "他", "馬", "怕", "拿", "拉", "發", "八"})
            list += QByteArray(word) + "\tㄉㄚˋ\n";
        list += "朋友\tㄆㄥˊ ㄧㄡˇ\n";
        QVERIFY(write(dir.filePath("words.tsv"), list));
        Backend backend(dir.filePath("words.tsv"), dir.filePath("settings.ini"), 7);
        while (backend.word() != "朋友")
            backend.type("da");
        backend.type("x");
        backend.type("pengyou");
        QStringList next;
        for (int i = 0; i < 4; ++i) {
            next << backend.word();
            backend.type(backend.word() == "朋友" ? "pengyou" : "da");
        }
        QCOMPARE(next.indexOf("朋友"), 3);
    }

    void weakSoundsComeUpMoreOften() {
        QTemporaryDir dir;
        QByteArray list = "牛\tㄋㄧㄡˊ\n六\tㄌㄧㄡˋ\n";
        for (const char *word : {"大", "他", "馬", "怕", "拿", "拉", "發", "八"})
            list += QByteArray(word) + "\tㄉㄚˋ\n";
        QVERIFY(write(dir.filePath("words.tsv"), list));
        const QString iu = "ㄧㄡ → iu";
        auto keys = [](const Backend &backend) {
            return backend.word() == "牛" ? "niu" : backend.word() == "六" ? "liu" : "da";
        };
        {
            Backend backend(dir.filePath("words.tsv"), dir.filePath("settings.ini"), 5);
            QVERIFY(backend.weakest().isEmpty());
            // Three misses on iu, each on its own visit; wrong keys on one
            // visit count once, and only against the sound they were on.
            for (int misses = 0; misses < 3;) {
                if (backend.word() == "牛" || backend.word() == "六") {
                    backend.type(backend.word() == "牛" ? "n" : "l");
                    backend.type("xxx");
                    backend.type("iu");
                    ++misses;
                } else {
                    backend.type("da");
                }
            }
            QVERIFY(backend.weakness(iu) > 1.5);
            QCOMPARE(backend.weakness("ㄋ → n"), 0.0);
            QCOMPARE(backend.weakness("ㄌ → l"), 0.0);
            QCOMPARE(backend.weakest(), QStringList{iu});

            // Two words in ten have it, but they now come up twice as often,
            // even as each clean answer eases the pull.
            int weak = 0;
            for (int i = 0; i < 10; ++i) {
                weak += backend.word() == "牛" || backend.word() == "六";
                backend.type(keys(backend));
            }
            QVERIFY2(weak >= 4, qPrintable(QString::number(weak)));
        }

        // It is remembered, eases with clean answers, and can be forgotten.
        Backend again(dir.filePath("words.tsv"), dir.filePath("settings.ini"), 5);
        const double remembered = again.weakness(iu);
        QVERIFY(remembered > 0);
        while (again.word() != "牛" && again.word() != "六")
            again.type("da");
        again.type(keys(again));
        QVERIFY(again.weakness(iu) < remembered);
        again.resetStats();
        QVERIFY(again.weakest().isEmpty());
        Backend fresh(dir.filePath("words.tsv"), dir.filePath("settings.ini"), 5);
        QCOMPARE(fresh.weakness(iu), 0.0);
    }

    void askingForTheHintCountsEverySoundLeft() {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("words.tsv"), "六\tㄌㄧㄡˋ\n"));
        Backend backend(dir.filePath("words.tsv"), dir.filePath("settings.ini"), 1);
        backend.reveal();
        QCOMPARE(backend.weakness("ㄌ → l"), 1.0);
        QCOMPARE(backend.weakness("ㄧㄡ → iu"), 1.0);
        backend.type("liu");
        // Asked again after a clean ㄌ, only what is left counts.
        backend.type("l");
        backend.reveal();
        backend.type("iu");
        QCOMPARE(backend.weakness("ㄌ → l"), 0.7);
        QCOMPARE(backend.weakness("ㄧㄡ → iu"), 2.0);
    }

    void lessonsPickTheirWordsAndAreRemembered() {
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("words.tsv"), "朋友\tㄆㄥˊ ㄧㄡˇ\n女生\tㄋㄩˇ ㄕㄥ\n爸爸\tㄅㄚˋ ㄅㄚ˙\n"));
        {
            Backend backend(dir.filePath("words.tsv"), dir.filePath("settings.ini"), 1);
            QVERIFY(!backend.hints());
            backend.setLesson(7);
            for (int i = 0; i < 5; ++i) {
                QCOMPARE(backend.word(), QString("女生"));
                QVERIFY(backend.type("nvsheng"));
            }
            backend.setLesson(6);
            QCOMPARE(backend.word(), QString("朋友"));
            backend.setHints(true);
        }
        Backend again(dir.filePath("words.tsv"), dir.filePath("settings.ini"), 1);
        QCOMPARE(again.lesson(), 6);
        QVERIFY(again.hints());
    }

    void aMissingWordListLeavesNothingToType() {
        QTemporaryDir dir;
        Backend backend(dir.filePath("none.tsv"), dir.filePath("settings.ini"), 1);
        QCOMPARE(backend.wordCount(), 0);
        QVERIFY(backend.word().isEmpty());
        QVERIFY(backend.type("abc"));
        backend.skip();
    }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
        QCOMPARE(theme.radius(), 0);
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("theme"));
        QVERIFY(write(dir.path() + "/theme/colors.toml", "mode = \"light\"\n"
                                                         "theme_type = \"dark\"\n"
                                                         "accent = \"#112233\"\n"
                                                         "bg = #fafafa # paper\n"
                                                         "foreground = '#202020'\n"
                                                         "color1 = \"#aa0000\"\n"));

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#fafafa"));
        QCOMPARE(theme.foreground(), QColor("#202020"));
        QCOMPARE(theme.colors().value("red").value<QColor>(), QColor("#aa0000"));
        QVERIFY(theme.light());
    }

    void themeAsksHyprlandAgainOnlyWhenItReloads() {
        // The fake Hyprland lives outside the theme directory, whose changes
        // the Theme also follows.
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));

        // Once on start, and once more when the event socket connects.
        Theme theme(dir.path(), nullptr, command, events.fullServerName());
        QTRY_VERIFY(events.hasPendingConnections());
        QLocalSocket *hyprland = events.nextPendingConnection();
        QTRY_COMPARE(lines(hypr.filePath("calls")), 2);
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":10}"));
        QTest::qWait(300);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        // Events arrive in pieces; only a whole configreloaded line counts.
        hyprland->write("activewindow>>kitty,~\nconfigrel");
        hyprland->flush();
        QTest::qWait(100);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        hyprland->write("oaded>>\n");
        QTRY_COMPARE(theme.radius(), 10);
        QCOMPARE(lines(hypr.filePath("calls")), 3);
    }

    void themeReachesHyprlandOnceItListens() {
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        Theme theme(dir.path(), nullptr, command, hypr.filePath("events.sock"));
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":8}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));
        QTRY_VERIFY_WITH_TIMEOUT(events.hasPendingConnections(), 5000);
        QTRY_COMPARE(theme.radius(), 8);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorSeesAFirstPick() {
        // Before the first pick there is no ~/.config/omarchy to watch.
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/da.json", R"({"Save": "Gem"})"));
        Translator translator(dir.path() + "/omarchy/language", dir.path());
        QVERIFY(QDir(dir.path()).mkpath("omarchy"));
        QVERIFY(write(dir.path() + "/omarchy/language", "da\n"));
        QTRY_COMPARE(translator.translate("Main", "Save"), QString("Gem"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"},
                                                   "%n files": {"one": "%n file", "other": "%n files"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        // An untranslated counted string falls back to English, with English rules.
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 21), QString("%n files"));
    }

    void englishCatalogListsEveryString() {
        // en.json is what translators work from: every text the sources
        // translate is in it, as itself or, when counted, as its English
        // plural forms, and nothing else is.
        const QString sources = QFileInfo(QFINDTESTDATA("../src/Main.qml")).absolutePath();
        QStringList problems;
        const QMap<QString, bool> used = translatables(sources, &problems);
        const QJsonObject english = readCatalog(sources + "/i18n/en.json");
        for (auto it = used.begin(); it != used.end(); ++it) {
            const QJsonValue value = english.value(it.key());
            if (value.isUndefined())
                problems << "missing from en.json: " + it.key();
            else if (it.value() && (value.toObject().keys() != QStringList{"one", "other"}
                                    || value.toObject().value("other").toString() != it.key()))
                problems << "counted, so needs its one and other forms: " + it.key();
            else if (!it.value() && value.toString() != it.key())
                problems << "should be itself: " + it.key();
        }
        for (const QString &key : english.keys())
            if (!used.contains(key))
                problems << "in en.json but no source translates it: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogsMatchTheEnglishOne() {
        // A catalog may lack a string (it shows in English meanwhile), but what
        // it has translates something in en.json, with the same placeholders,
        // and a counted string has every form its language uses.
        const QString folder = QFileInfo(QFINDTESTDATA("../src/i18n/en.json")).absolutePath();
        const QJsonObject english = readCatalog(folder + "/en.json");
        QStringList problems;
        for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files)) {
            const QString code = file.completeBaseName();
            const QJsonObject catalog = readCatalog(file.filePath());
            if (catalog.isEmpty())
                problems << code + ": not a JSON object, or empty";
            // Some languages reach a form only at a million ("many" in French).
            QSet<QString> categories{"other", Translator::pluralCategory(code, 1000000)};
            for (long long n = 0; n <= 1000; ++n)
                categories << Translator::pluralCategory(code, n);
            for (auto it = catalog.begin(); it != catalog.end(); ++it) {
                const QString where = code + ": " + it.key() + ": ";
                const QJsonValue source = english.value(it.key());
                if (source.isUndefined()) {
                    problems << where + "not in en.json";
                } else if (source.isString()) {
                    if (!it.value().isString() || it.value().toString().isEmpty())
                        problems << where + "needs a translation";
                    else if (placeholders(it.value().toString()) != placeholders(it.key()))
                        problems << where + "has other placeholders than the English";
                } else {
                    const QJsonObject forms = it.value().toObject();
                    const QStringList wanted = placeholders(source.toObject().value("other").toString());
                    for (const QString &category : categories)
                        if (forms.value(category).toString().isEmpty())
                            problems << where + "needs its " + category + " form";
                    // A form other than "other" may say its number in words.
                    for (auto form = forms.begin(); form != forms.end(); ++form) {
                        const QStringList has = placeholders(form.value().toString());
                        QStringList needed = wanted;
                        if (form.key() != "other")
                            needed.removeIf([&](const QString &p) { return p.endsWith('n') && !has.contains(p); });
                        if (has != needed)
                            problems << where + form.key() + " has other placeholders than the English";
                    }
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void windowLoadsAndAnswersTheKeyboard() {
        // A temp language, theme, and settings, so the test never depends on the desktop's.
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language");
        QCoreApplication::installTranslator(&translator);
        Theme theme(dir.path());
        Backend backend(":/words.tsv", dir.filePath("settings.ini"), 3);

        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this, [&](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window));

        // Typing the first syllable's keys fills it in and moves on.
        const QString first = backend.word();
        const int count = backend.syllables().size();
        const QString keys = backend.syllables().first().toMap().value("keys").toString();
        auto *typed = findItem(window->contentItem(), "typed0");
        auto *rest = findItem(window->contentItem(), "rest0");
        QVERIFY(typed && rest);
        // Nothing is given away until Tab asks for it.
        auto *breakdown = window->findChild<QObject *>("breakdown");
        QCOMPARE(rest->property("text").toString(), QString());
        QCOMPARE(breakdown->property("opacity").toDouble(), 0.0);
        QTest::keyClick(window, Qt::Key_Tab);
        QCOMPARE(rest->property("text").toString(), backend.syllables().first().toMap().value("pinyin").toString());
        QCOMPARE(breakdown->property("opacity").toDouble(), 1.0);
        QTest::keyClick(window, keys.at(0).toLatin1());
        QCOMPARE(backend.typed(), keys.size() > 1 ? 1 : 0);
        for (const QChar key : keys.mid(1))
            QTest::keyClick(window, key.toLatin1());
        QCOMPARE(backend.done(), 1);
        QCOMPARE(window->findChild<QObject *>("done")->property("text").toString(), QString("Done 1"));
        // Asking for the pinyin counted as a miss.
        QCOMPARE(window->findChild<QObject *>("accuracy")->property("text").toString(), QString("Accuracy 0%"));

        // The rest of the word brings the next one.
        for (int i = 1; i < count; ++i)
            for (const QChar key : backend.syllables().at(i).toMap().value("keys").toString())
                QTest::keyClick(window, key.toLatin1());
        QCOMPARE(backend.done(), count);
        QVERIFY(backend.word() != first);

        // A wrong key shows nothing, however many there are; Ctrl+H shows
        // the pinyin all the time.
        const QChar expected = backend.syllables().first().toMap().value("keys").toString().at(0);
        for (int i = 0; i < 4; ++i)
            QTest::keyClick(window, expected == 'q' ? 'w' : 'q');
        QVERIFY(backend.misses().first().toBool());
        QCOMPARE(breakdown->property("opacity").toDouble(), 0.0);
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        QVERIFY(backend.hints());
        QCOMPARE(breakdown->property("opacity").toDouble(), 1.0);
        QTest::keyClick(window, Qt::Key_H, Qt::ControlModifier);
        QVERIFY(!backend.hints());

        // Escape skips, and Ctrl and an arrow change the lesson.
        const QString skipped = backend.word();
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(backend.word() != skipped);
        QTest::keyClick(window, Qt::Key_Right, Qt::ControlModifier);
        QCOMPARE(backend.lesson(), 1);
        QTest::keyClick(window, Qt::Key_Left, Qt::ControlModifier);
        QCOMPARE(backend.lesson(), 0);

        // The selected lesson's caption reads on the accent.
        auto *chip = findItem(window->contentItem(), "lesson0");
        QVERIFY(chip);
        QCOMPARE(chip->property("color").value<QColor>(), theme.accent());

        // While the shortcuts are up, letters and Escape belong to the popup.
        auto *help = window->findChild<QObject *>("help");
        QTest::keyClick(window, Qt::Key_F1);
        QTRY_VERIFY(help->property("opened").toBool());
        const QString shown = backend.word();
        const int done = backend.done();
        QTest::keyClick(window, 'a');
        // A Material popup animates out: `opened` drops at once, `visible` after.
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY(!help->property("visible").toBool());
        QCOMPARE(backend.word(), shown);
        QCOMPARE(backend.done(), done);
        QCOMPARE(backend.typed(), 0);

        QVERIFY2(warnings.isEmpty(), qPrintable(warnings.join('\n')));
        QCoreApplication::removeTranslator(&translator);
    }
};

QTEST_MAIN(BopopinTests)
#include "bopopin_tests.moc"
