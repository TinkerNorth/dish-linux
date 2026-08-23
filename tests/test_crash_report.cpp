// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The redaction is the security-relevant half of the local crash-report flow:
// the user is about to paste this into a public issue tracker, and anything the
// scrub misses is published by someone who trusted it. These pin the four
// categories it promises to remove.

#include "UI/CrashReport.h"

#include <catch2/catch_test_macros.hpp>

using namespace dish::crash;

namespace {
const QString kHome = QStringLiteral("/home/someone");
} // namespace

TEST_CASE("the home directory collapses to a tilde") {
    const QString raw = QStringLiteral("at /home/someone/.config/dish/thing.cpp:42");
    const QString out = redact(raw, kHome);
    REQUIRE(out.contains("~/.config/dish/thing.cpp:42"));
    REQUIRE_FALSE(out.contains("/home/someone"));
}

TEST_CASE("a pairing key on the stack is redacted") {
    // 64 hex chars: exactly what a 32-byte pairing or session key looks like.
    const QString key = QStringLiteral("abc0e83e1405fc3869407d9f7055401e"
                                       "d7120625e81d7e036008f791e18b7560");
    const QString out = redact(QStringLiteral("key=") + key, kHome);
    REQUIRE(out == "key=[redacted]");
}

TEST_CASE("a short hex value is left alone") {
    // Below the 32-char floor: an address or a small opcode dump is the actual
    // content of a backtrace, and scrubbing it would gut the report.
    const QString out = redact(QStringLiteral("at 0x7ffff5445b7e in raise"), kHome);
    REQUIRE(out.contains("0x7ffff5445b7e"));
}

TEST_CASE("LAN addresses and mDNS names are removed") {
    const QString raw =
        QStringLiteral("connecting to 192.168.68.101:9876 (Lauras-MacBook-Pro.local)");
    const QString out = redact(raw, kHome);
    REQUIRE_FALSE(out.contains("192.168.68.101"));
    REQUIRE_FALSE(out.contains("Lauras-MacBook-Pro"));
    REQUIRE(out.contains("[ip]"));
    REQUIRE(out.contains("[host].local"));
    // The port is not identifying and is worth keeping for diagnosis.
    REQUIRE(out.contains(":9876"));
}

TEST_CASE("an empty home does not blank the whole report") {
    // getenv("HOME") can legitimately come back empty; replacing "" would
    // otherwise splice a tilde between every character.
    const QString raw = QStringLiteral("frame #0 in main");
    REQUIRE(redact(raw, QString()) == raw);
    REQUIRE(redact(raw, QStringLiteral("/")) == raw);
}

TEST_CASE("the report carries version and environment above the backtrace") {
    const QString out = buildReport(QStringLiteral("frame #0"), kHome);
    REQUIRE(out.startsWith("Dish "));
    REQUIRE(out.contains("Qt: "));
    REQUIRE(out.contains("--- backtrace ---"));
    REQUIRE(out.contains("frame #0"));
}

TEST_CASE("the report is redacted before it is ever shown") {
    // buildReport is what both the on-screen preview and the issue URL use, so
    // the scrub has to happen inside it rather than at one of the call sites.
    const QString out = buildReport(QStringLiteral("at /home/someone/secret"), kHome);
    REQUIRE_FALSE(out.contains("/home/someone"));
}

TEST_CASE("a huge backtrace still yields a usable issue URL") {
    const QString huge = QString(200000, QLatin1Char('A'));
    const QUrl url = issueUrl(buildReport(huge, kHome));
    REQUIRE(url.isValid());
    REQUIRE(url.toEncoded().size() <= kMaxIssueUrlBytes);
    REQUIRE(url.toString().contains("github.com/TinkerNorth/dish-linux/issues/new"));
}

TEST_CASE("a small report is not truncated") {
    const QString report = buildReport(QStringLiteral("frame #0 in main"), kHome);
    const QUrl url = issueUrl(report);
    REQUIRE(url.toEncoded().size() <= kMaxIssueUrlBytes);
    REQUIRE_FALSE(url.toString().contains("truncated"));
}
