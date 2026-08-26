// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The one guarantee the mutual-TLS calls make that nothing else can observe: no
// session is ever offered for resumption. A resumed TLS session carries the peer
// identity forward instead of asking for the certificate again, so a Moonlight
// host's verify callback never runs, and Sunshine answers that with a fatal
// internal_error alert (RFC 8446 alert 80) and no log line at all, at TLS 1.2 as
// well as 1.3. Qt shares and persists sessions across the connections one
// QNetworkAccessManager makes, which is exactly the shape that triggers it, so
// every switch is pinned here rather than left to a future refactor.

#include "source/moonlight/MoonlightHttp.h"

#include <catch2/catch_test_macros.hpp>

#include <QSsl>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QString>

using dish::source::moon::MoonlightHttp;

TEST_CASE("the mutual-TLS configuration never offers a session to resume", "[moonlight][tls]") {
    const QSslConfiguration ssl = MoonlightHttp::tlsConfiguration(QString(), QString());

    CHECK(ssl.testSslOption(QSsl::SslOptionDisableSessionTickets));
    CHECK(ssl.testSslOption(QSsl::SslOptionDisableSessionSharing));
    CHECK(ssl.testSslOption(QSsl::SslOptionDisableSessionPersistence));
    // Not vacuous: the configuration this starts from would still offer a
    // session by at least one route, so the switches above are this code's
    // doing and not the framework's. Written as "at least one" rather than
    // switch by switch, because which of them Qt leaves on is Qt's business and
    // changing it must not turn this red.
    const QSslConfiguration byDefault = QSslConfiguration::defaultConfiguration();
    const bool defaultWouldResume =
        !byDefault.testSslOption(QSsl::SslOptionDisableSessionTickets) ||
        !byDefault.testSslOption(QSsl::SslOptionDisableSessionSharing) ||
        !byDefault.testSslOption(QSsl::SslOptionDisableSessionPersistence);
    CHECK(defaultWouldResume);
}

TEST_CASE("peer verification is off because trust is the pairing pin", "[moonlight][tls]") {
    // Both ends are self-signed, so chain verification can only fail. The reply
    // handler compares the presented certificate against the one the pairing
    // handshake verified and reports a mismatch as unreachable, which is the
    // trust decision this replaces.
    const QSslConfiguration ssl = MoonlightHttp::tlsConfiguration(QString(), QString());
    CHECK(ssl.peerVerifyMode() == QSslSocket::VerifyNone);
}

TEST_CASE("a usable identity is presented, and an unusable one is left out", "[moonlight][tls]") {
    SECTION("empty PEMs carry no client credential") {
        const QSslConfiguration ssl = MoonlightHttp::tlsConfiguration(QString(), QString());
        CHECK(ssl.localCertificate().isNull());
        CHECK(ssl.privateKey().isNull());
    }
    SECTION("unparsable PEMs are dropped rather than half-applied") {
        const QSslConfiguration ssl = MoonlightHttp::tlsConfiguration(
            QStringLiteral("-----BEGIN CERTIFICATE-----\nnot base64\n-----END CERTIFICATE-----\n"),
            QStringLiteral("-----BEGIN PRIVATE KEY-----\nnot base64\n-----END PRIVATE KEY-----\n"));
        CHECK(ssl.localCertificate().isNull());
        CHECK(ssl.privateKey().isNull());
        // The resumption switches hold whatever the identity turned out to be.
        CHECK(ssl.testSslOption(QSsl::SslOptionDisableSessionTickets));
        CHECK(ssl.testSslOption(QSsl::SslOptionDisableSessionSharing));
        CHECK(ssl.testSslOption(QSsl::SslOptionDisableSessionPersistence));
    }
}
