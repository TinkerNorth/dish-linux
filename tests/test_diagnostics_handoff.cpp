// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Linux mail hand-off (source/system/DiagnosticsHandoff.h) against a real
// process: an xdg-email stand-in written to a temporary folder that records the
// argv it was given and exits with the code the case asks for, and a program
// that is not there at all. Nothing here opens a browser or a mail client; the
// URLs the fallback would open are recorded instead.

#include "core/diagnostics/MailHandoff.h"
#include "source/system/DiagnosticsHandoff.h"

#include "TestEventLoop.h"

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QUrl>

#include <optional>
#include <vector>

using dish::source::DiagnosticsHandoff;
using dish::source::DiagnosticsMail;

namespace {

// What the fallback would have opened, in order.
struct OpenedUrls {
    std::vector<QUrl> urls;

    void open(const QUrl& url) { urls.push_back(url); }
};

// A folder holding the stand-in xdg-email and the attachment it is handed.
struct MailRig {
    QTemporaryDir dir;
    OpenedUrls opened;
    std::optional<bool> handedOff;
    QString handedFileName;

    QString scriptPath() const { return dir.filePath(QStringLiteral("xdg-email")); }
    QString argsPath() const { return dir.filePath(QStringLiteral("args")); }

    // `body` is the shell after the argv is recorded: an exit, or a wait and then an exit.
    void writeScript(const QByteArray& body) const {
        QFile script(scriptPath());
        REQUIRE(script.open(QIODevice::WriteOnly));
        script.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \"" + argsPath().toUtf8() + "\"\n" + body +
                     "\n");
        script.close();
        REQUIRE(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    }

    DiagnosticsMail mail() const {
        return DiagnosticsMail{QStringLiteral("support@tinkernorth.com"),
                               QStringLiteral("Dish diagnostics: Dish for Linux 2.8.0 on AYN Thor"),
                               QStringLiteral("line 1\nline 2"),
                               dir.filePath(QStringLiteral("dish-diagnostics.json"))};
    }

    QStringList recordedArgs() const {
        QFile args(argsPath());
        if (!args.open(QIODevice::ReadOnly)) { return {}; }
        return QString::fromUtf8(args.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    void onHandedOff(bool viaMailClient, const QString& fileName) {
        handedOff = viaMailClient;
        handedFileName = fileName;
    }
};

void listen(DiagnosticsHandoff& handoff, MailRig& rig) {
    QObject::connect(&handoff, &DiagnosticsHandoff::handedOff, &handoff,
                     [&rig](bool via, const QString& name) { rig.onHandedOff(via, name); });
}

dish::source::UrlOpener recorderOf(MailRig& rig) {
    return [&rig](const QUrl& url) { rig.opened.open(url); };
}

bool answered(const MailRig& rig) { return rig.handedOff.has_value(); }

} // namespace

// xdg-utils 1.1.3 turns --attach into a mailto: link's `attach=` parameter, and only Thunderbird
// makes that an attachment: every other mail app gets the link without the file while xdg-email
// still exits 0. So a hand-off that went still shows the folder, and opens no mailto: of its own.
TEST_CASE("handoff: xdg-email that takes the mail also shows the file's folder",
          "[diagnostics][mail]") {
    MailRig rig;
    REQUIRE(rig.dir.isValid());
    rig.writeScript("exit 0");
    DiagnosticsHandoff handoff(rig.scriptPath(), recorderOf(rig), nullptr);
    listen(handoff, rig);

    REQUIRE(handoff.send(rig.mail()));
    REQUIRE(dish::test::spinFor([&rig] { return answered(rig); }));
    CHECK(*rig.handedOff);
    REQUIRE(rig.opened.urls.size() == 1U);
    CHECK(rig.opened.urls[0] == QUrl::fromLocalFile(rig.dir.path()));
    CHECK(rig.handedFileName == QStringLiteral("dish-diagnostics.json"));
    CHECK(rig.recordedArgs() == QStringList{QStringLiteral("--attach"), rig.mail().attachmentPath,
                                            QStringLiteral("--subject"), rig.mail().subject,
                                            QStringLiteral("--body"), QStringLiteral("line 1"),
                                            QStringLiteral("line 2"),
                                            QStringLiteral("support@tinkernorth.com")});
}

TEST_CASE("handoff: xdg-email that fails opens mailto and the folder", "[diagnostics][mail]") {
    MailRig rig;
    REQUIRE(rig.dir.isValid());
    // 3: a tool xdg-email needs is missing, which is what a desktop with no mail client answers.
    rig.writeScript("exit 3");
    DiagnosticsHandoff handoff(rig.scriptPath(), recorderOf(rig), nullptr);
    listen(handoff, rig);

    REQUIRE(handoff.send(rig.mail()));
    REQUIRE(dish::test::spinFor([&rig] { return answered(rig); }));
    CHECK_FALSE(*rig.handedOff);
    CHECK(rig.handedFileName == QStringLiteral("dish-diagnostics.json"));
    REQUIRE(rig.opened.urls.size() == 2U);
    CHECK(rig.opened.urls[0].scheme() == QStringLiteral("mailto"));
    CHECK(rig.opened.urls[0].path() == QStringLiteral("support@tinkernorth.com"));
    CHECK(rig.opened.urls[1] == QUrl::fromLocalFile(rig.dir.path()));
}

TEST_CASE("handoff: with no xdg-email on the machine the fallback runs at once",
          "[diagnostics][mail]") {
    MailRig rig;
    REQUIRE(rig.dir.isValid());
    DiagnosticsHandoff handoff(rig.dir.filePath(QStringLiteral("no-such-xdg-email")),
                               recorderOf(rig), nullptr);
    listen(handoff, rig);

    REQUIRE(handoff.send(rig.mail()));
    REQUIRE(answered(rig));
    CHECK_FALSE(*rig.handedOff);
    REQUIRE(rig.opened.urls.size() == 2U);
    CHECK(rig.opened.urls[0].scheme() == QStringLiteral("mailto"));
}

TEST_CASE("handoff: one mail at a time", "[diagnostics][mail]") {
    MailRig rig;
    REQUIRE(rig.dir.isValid());
    rig.writeScript("sleep 1\nexit 0");
    DiagnosticsHandoff handoff(rig.scriptPath(), recorderOf(rig), nullptr);
    listen(handoff, rig);

    REQUIRE(handoff.send(rig.mail()));
    CHECK_FALSE(handoff.send(rig.mail()));
    REQUIRE(dish::test::spinFor([&rig] { return answered(rig); }));
    CHECK(*rig.handedOff);
    SECTION("the next mail goes once the last one has its answer") {
        rig.handedOff.reset();
        CHECK(handoff.send(rig.mail()));
        CHECK(dish::test::spinFor([&rig] { return answered(rig); }));
    }
}

TEST_CASE("handoff: the file is written whole into its folder", "[diagnostics][mail]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString folder = dir.filePath(QStringLiteral("diagnostics"));
    const QString path = dish::source::saveDiagnosticsFile(folder, QStringLiteral("d.json"),
                                                           QByteArrayLiteral("{}\n"));
    REQUIRE(path == folder + QStringLiteral("/d.json"));
    QFile file(path);
    REQUIRE(file.open(QIODevice::ReadOnly));
    CHECK(file.readAll() == QByteArrayLiteral("{}\n"));
    SECTION("no folder, no file") {
        CHECK(
            dish::source::saveDiagnosticsFile(QString(), QStringLiteral("d.json"), "{}").isEmpty());
    }
}

TEST_CASE("handoff: Save shows the file's folder", "[diagnostics][mail]") {
    MailRig rig;
    REQUIRE(rig.dir.isValid());
    const DiagnosticsHandoff handoff(QStringLiteral("unused"), recorderOf(rig), nullptr);
    handoff.reveal(rig.mail().attachmentPath);
    REQUIRE(rig.opened.urls.size() == 1U);
    CHECK(rig.opened.urls[0] == QUrl::fromLocalFile(rig.dir.path()));
}
