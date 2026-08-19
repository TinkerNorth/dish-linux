// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The fake "SSL session" is just the cert DER buffer, empty meaning no peer
// cert. kFp123 is the SHA-256 of DER {1, 2, 3}.

#include "source/http/SatelliteTlsVerifier.h"

#include "QSettingsFixture.h"
#include "repository/SatellitePinRepository.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QString>

using dish::http::verifyPeerCertificate;
using dish::repository::SatellitePinRepository;
using dish::test::makeSharedSettings;

namespace {
const QString kSat = QStringLiteral("satellite:mid:test");
const QString kFp123 =
    QStringLiteral("039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81");

QByteArray der(std::initializer_list<char> bytes) {
    QByteArray out;
    out.reserve(static_cast<qsizetype>(bytes.size()));
    for (const char b : bytes) { out.append(b); }
    return out;
}
} // namespace

TEST_CASE("first contact pins and accepts", "[tlsverify]") {
    SatellitePinRepository pins(makeSharedSettings());
    CHECK(verifyPeerCertificate(kSat, pins, der({1, 2, 3})));
    CHECK(pins.pinnedFingerprint(kSat) == kFp123);
}

TEST_CASE("the same cert later matches without re-pinning", "[tlsverify]") {
    SatellitePinRepository pins(makeSharedSettings());
    CHECK(verifyPeerCertificate(kSat, pins, der({1, 2, 3})));
    const auto afterFirst = pins.pinnedFingerprint(kSat);
    CHECK(verifyPeerCertificate(kSat, pins, der({1, 2, 3})));
    CHECK(pins.pinnedFingerprint(kSat) == afterFirst);
}

TEST_CASE("a different cert after pinning is rejected and the pin left intact", "[tlsverify]") {
    SatellitePinRepository pins(makeSharedSettings());
    pins.pin(kSat, kFp123);
    CHECK_FALSE(verifyPeerCertificate(kSat, pins, der({9, 9, 9})));
    CHECK(pins.pinnedFingerprint(kSat) == kFp123);
}

TEST_CASE("a session without a peer cert is rejected", "[tlsverify]") {
    SatellitePinRepository pins(makeSharedSettings());
    CHECK_FALSE(verifyPeerCertificate(kSat, pins, QByteArray()));
}

TEST_CASE("a mismatch reports via onMismatch", "[tlsverify]") {
    SatellitePinRepository pins(makeSharedSettings());
    pins.pin(kSat, kFp123);
    int mismatches = 0;
    CHECK_FALSE(verifyPeerCertificate(kSat, pins, der({9, 9, 9}), [&] { ++mismatches; }));
    CHECK(mismatches == 1);
}

TEST_CASE("first use and match never invoke onMismatch", "[tlsverify]") {
    SatellitePinRepository pins(makeSharedSettings());
    int mismatches = 0;
    CHECK(verifyPeerCertificate(kSat, pins, der({1, 2, 3}), [&] { ++mismatches; }));
    CHECK(verifyPeerCertificate(kSat, pins, der({1, 2, 3}), [&] { ++mismatches; }));
    CHECK(mismatches == 0);
}

TEST_CASE("a missing peer cert is not counted a mismatch", "[tlsverify]") {
    SatellitePinRepository pins(makeSharedSettings());
    int mismatches = 0;
    CHECK_FALSE(verifyPeerCertificate(kSat, pins, QByteArray(), [&] { ++mismatches; }));
    CHECK(mismatches == 0);
}

TEST_CASE("pins are kept per satellite id", "[tlsverify]") {
    SatellitePinRepository pins(makeSharedSettings());
    int mismatches = 0;
    CHECK(verifyPeerCertificate(QStringLiteral("a"), pins, der({1, 2, 3}), [&] { ++mismatches; }));
    CHECK(verifyPeerCertificate(QStringLiteral("b"), pins, der({4, 5, 6}), [&] { ++mismatches; }));
    CHECK(mismatches == 0); // a first use for each id, not a mismatch
    CHECK(pins.pinnedFingerprint(QStringLiteral("a")).value().size() == 64);
    CHECK(pins.pinnedFingerprint(QStringLiteral("b")).value().size() == 64);
}
