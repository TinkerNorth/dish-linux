// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The i18n contract, asserted against the COMPILED catalogues rather than the
// source that produced them.
//
// Three things here can only be checked this way. Qt's locale fallback is
// library behaviour, so "does a de_DE machine actually find dish_de.qm" is a
// question about QTranslator, not about our code. The order of <numerusform>
// slots is decided by Qt's plural rules for the target language and is nowhere
// in our files — Bosnian's three forms are one/few/other in that sequence
// because Qt says so, and scripts/seed-from-android.py writes Android's
// quantity buckets into those slots on exactly that assumption. And English is
// only correct at n=1 because dish_en.qm carries a singular the source string
// cannot.
//
// The fourth block is the cheap one that catches the most: a translation whose
// placeholders drifted from its source renders a bare number with no noun, or
// a literal "%2", in whatever language the user happens to run. That is
// invisible to every other test in this suite.

#include <catch2/catch_test_macros.hpp>

#include "Util/Localization.h"

#include <QDir>
#include <QFile>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QTranslator>
#include <QXmlStreamReader>

namespace {

QString qmDir() { return QStringLiteral(DISH_QM_DIR); }

QString tsPath(const QString& locale) {
    return QStringLiteral(DISH_TS_DIR "/dish_%1.ts").arg(locale);
}

// Every locale the app ships a catalogue for.
const QStringList kLocales{QStringLiteral("en"), QStringLiteral("bs"), QStringLiteral("de"),
                           QStringLiteral("es"), QStringLiteral("fr"), QStringLiteral("pt_BR")};

bool catalogsBuilt() { return QFile::exists(qmDir() + QStringLiteral("/dish_en.qm")); }

// %1..%9 and %n, which are the only placeholder forms Qt substitutes.
QSet<QString> placeholders(const QString& text) {
    static const QRegularExpression re(QStringLiteral("%([1-9]|n)"));
    QSet<QString> found;
    auto it = re.globalMatch(text);
    while (it.hasNext()) { found.insert(it.next().captured(1)); }
    return found;
}

struct Message {
    QString context;
    QString source;
    QStringList translations; // one entry, or one per numerusform
    bool numerus = false;
};

// Reads a .ts into the messages that actually carry a translation. Untranslated
// entries are dropped: lrelease leaves them out of the .qm, so Qt falls back to
// the source text and there is nothing to check.
QList<Message> readCatalog(const QString& path) {
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    QXmlStreamReader xml(&file);

    QList<Message> messages;
    QString context;
    Message current;
    bool inMessage = false;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const auto name = xml.name();
            if (name == QLatin1String("name") && !inMessage) {
                context = xml.readElementText();
            } else if (name == QLatin1String("message")) {
                inMessage = true;
                current = Message{};
                current.context = context;
                current.numerus =
                    xml.attributes().value(QLatin1String("numerus")) == QLatin1String("yes");
            } else if (name == QLatin1String("source") && inMessage) {
                current.source = xml.readElementText();
            } else if (name == QLatin1String("numerusform") && inMessage) {
                current.translations << xml.readElementText();
            } else if (name == QLatin1String("translation") && inMessage && !current.numerus) {
                current.translations << xml.readElementText();
            }
        } else if (xml.isEndElement() && xml.name() == QLatin1String("message")) {
            inMessage = false;
            const bool anyText =
                std::any_of(current.translations.cbegin(), current.translations.cend(),
                            [](const QString& t) { return !t.trimmed().isEmpty(); });
            if (anyText) { messages << current; }
        }
    }
    REQUIRE_FALSE(xml.hasError());
    return messages;
}

// The context a given source string lives in, so the runtime lookups below do
// not hard-code a QML file name that a refactor could move.
QString contextFor(const QList<Message>& catalog, const QString& source) {
    for (const Message& m : catalog) {
        if (m.source == source) { return m.context; }
    }
    return {};
}

} // namespace

TEST_CASE("every shipped locale has a compiled catalogue", "[i18n]") {
    if (!catalogsBuilt()) { SKIP("built without Qt LinguistTools — no .qm to check"); }
    for (const QString& locale : kLocales) {
        INFO("locale " << locale.toStdString());
        CHECK(QFile::exists(qmDir() + QStringLiteral("/dish_%1.qm").arg(locale)));
    }
}

// What main.cpp does on a real machine. The regional forms are the ones that
// matter: a desktop reports de_DE, not de, and a catalogue named dish_de.qm has
// to be reachable from it.
TEST_CASE("system locales resolve to the right catalogue", "[i18n]") {
    if (!catalogsBuilt()) { SKIP("built without Qt LinguistTools — no .qm to check"); }
    const QList<QPair<QString, QString>> cases{
        {QStringLiteral("en_US"), QStringLiteral("dish_en")},
        {QStringLiteral("en_GB"), QStringLiteral("dish_en")},
        {QStringLiteral("de_DE"), QStringLiteral("dish_de")},
        {QStringLiteral("de_AT"), QStringLiteral("dish_de")},
        {QStringLiteral("es_ES"), QStringLiteral("dish_es")},
        {QStringLiteral("es_MX"), QStringLiteral("dish_es")},
        {QStringLiteral("fr_FR"), QStringLiteral("dish_fr")},
        {QStringLiteral("fr_CA"), QStringLiteral("dish_fr")},
        {QStringLiteral("bs_BA"), QStringLiteral("dish_bs")},
        // The one catalogue with a region in its own name: a Brazilian user must
        // land on pt_BR rather than a generic pt that does not exist.
        {QStringLiteral("pt_BR"), QStringLiteral("dish_pt_BR")},
    };
    for (const auto& [localeName, expectedCatalog] : cases) {
        INFO("locale " << localeName.toStdString());
        QTranslator translator;
        REQUIRE(dish::i18n::loadCatalog(translator, QLocale(localeName), qmDir()));
        CHECK(translator.filePath().contains(expectedCatalog));
    }
}

// A locale with no catalogue must fail cleanly so main.cpp leaves the source
// strings alone instead of installing an empty translator.
TEST_CASE("an unshipped locale loads nothing", "[i18n]") {
    if (!catalogsBuilt()) { SKIP("built without Qt LinguistTools — no .qm to check"); }
    QTranslator translator;
    CHECK_FALSE(dish::i18n::loadCatalog(translator, QLocale(QStringLiteral("ja_JP")), qmDir()));
}

// The reason dish_en.ts exists at all. Without it the source string is the only
// English Qt has, and a source can carry one form: "1 slots free".
TEST_CASE("English plurals pick the singular at one", "[i18n]") {
    if (!catalogsBuilt()) { SKIP("built without Qt LinguistTools — no .qm to check"); }
    const auto catalog = readCatalog(tsPath(QStringLiteral("en")));
    const QString source = QStringLiteral("%n slots free");
    const QString context = contextFor(catalog, source);
    REQUIRE_FALSE(context.isEmpty());

    QTranslator translator;
    REQUIRE(dish::i18n::loadCatalog(translator, QLocale(QStringLiteral("en_US")), qmDir()));

    CHECK(translator.translate(context.toUtf8(), source.toUtf8(), nullptr, 1) ==
          QStringLiteral("%n slot free"));
    CHECK(translator.translate(context.toUtf8(), source.toUtf8(), nullptr, 2) ==
          QStringLiteral("%n slots free"));
}

// Bosnian is why %n replaced the old singular/plural pairs: two forms cannot
// express one/few/other, and this pins the ORDER the seeding script writes into
// the numerusform slots.
TEST_CASE("Bosnian selects all three plural forms", "[i18n]") {
    if (!catalogsBuilt()) { SKIP("built without Qt LinguistTools — no .qm to check"); }
    const auto catalog = readCatalog(tsPath(QStringLiteral("bs")));
    const QString source = QStringLiteral("%n paired");
    const QString context = contextFor(catalog, source);
    REQUIRE_FALSE(context.isEmpty());

    QTranslator translator;
    REQUIRE(dish::i18n::loadCatalog(translator, QLocale(QStringLiteral("bs_BA")), qmDir()));

    const auto at = [&](int n) {
        return translator.translate(context.toUtf8(), source.toUtf8(), nullptr, n);
    };
    // one: 1 and 21. few: 2-4 and 22. other: 0, 5, 11-14.
    const QString one = at(1);
    const QString few = at(2);
    const QString other = at(5);
    REQUIRE_FALSE(one.isEmpty());
    CHECK(one != few);
    CHECK(few != other);
    CHECK(one != other);
    // The categories repeat by the last digit, with the teens forced to "other".
    CHECK(at(21) == one);
    CHECK(at(22) == few);
    CHECK(at(11) == other);
    CHECK(at(0) == other);
}

// The broad net. A translated string whose placeholders drifted from its source
// renders a stray "%2" or silently drops a value, and only in that language.
TEST_CASE("translations keep their source's placeholders", "[i18n]") {
    for (const QString& locale : kLocales) {
        const auto catalog = readCatalog(tsPath(locale));
        INFO("locale " << locale.toStdString());
        REQUIRE_FALSE(catalog.isEmpty());
        for (const Message& m : catalog) {
            const auto expected = placeholders(m.source);
            for (const QString& translation : m.translations) {
                if (translation.trimmed().isEmpty()) {
                    continue; // a partly-filled plural is caught below, not here
                }
                INFO("context " << m.context.toStdString() << " source " << m.source.toStdString()
                                << " translation " << translation.toStdString());
                CHECK(placeholders(translation) == expected);
            }
        }
    }
}

// A plural entry with some forms filled and others blank renders empty at the
// counts that hit the blank slot, which reads as a missing string rather than
// an untranslated one.
TEST_CASE("plural entries are all-or-nothing", "[i18n]") {
    for (const QString& locale : kLocales) {
        const auto catalog = readCatalog(tsPath(locale));
        INFO("locale " << locale.toStdString());
        for (const Message& m : catalog) {
            if (!m.numerus) { continue; }
            INFO("context " << m.context.toStdString() << " source " << m.source.toStdString());
            for (const QString& form : m.translations) { CHECK_FALSE(form.trimmed().isEmpty()); }
        }
    }
}

// English is the fallback every other language degrades to, so its plural
// entries are the one set that must never be left for later.
TEST_CASE("every English plural form is written", "[i18n]") {
    QFile file(tsPath(QStringLiteral("en")));
    REQUIRE(file.open(QIODevice::ReadOnly));
    QXmlStreamReader xml(&file);
    int numerus = 0;
    int filled = 0;
    bool inNumerusMessage = false;
    QStringList forms;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QLatin1String("message")) {
                inNumerusMessage =
                    xml.attributes().value(QLatin1String("numerus")) == QLatin1String("yes");
                forms.clear();
                if (inNumerusMessage) { ++numerus; }
            } else if (xml.name() == QLatin1String("numerusform") && inNumerusMessage) {
                forms << xml.readElementText();
            }
        } else if (xml.isEndElement() && xml.name() == QLatin1String("message") &&
                   inNumerusMessage) {
            const bool complete =
                !forms.isEmpty() && std::all_of(forms.cbegin(), forms.cend(), [](const QString& f) {
                    return !f.trimmed().isEmpty();
                });
            if (complete) { ++filled; }
            inNumerusMessage = false;
        }
    }
    REQUIRE(numerus > 0);
    CHECK(filled == numerus);
}
