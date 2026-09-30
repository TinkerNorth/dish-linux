// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// What exchangeHttp promises about when its outcome arrives and what the code receiving it may do,
// against a real TLS listener on loopback. Every client leans on all three: an outcome delivered
// from inside the call re-enters a caller that has not finished starting it, one delivered after
// its owner has gone runs against an object that no longer exists, and an exchange that touches
// itself after delivering cannot let the receiver tear its owner down.
//
// The last case survives a plain build by luck if the exchange does touch itself afterwards; under
// AddressSanitizer, which CI runs, it is a reported use-after-free.

#include "source/http/HttpExchange.h"

#include "FakePairingListener.h"

#include <catch2/catch_test_macros.hpp>

#include <QByteArray>
#include <QObject>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QString>
#include <QUrl>

#include <memory>

using dish::http::exchangeHttp;
using dish::http::HttpDone;
using dish::http::HttpRequest;
using dish::http::HttpResult;
using dish::test::FakePairingListener;
using dish::test::settle;
using dish::test::spinFor;

namespace {

// Far longer than any case here waits, so no outcome below is the deadline's.
constexpr int kDeadlineMs = 10000;

// A GET from loopback `port`, over TLS or plain TCP.
HttpRequest getFrom(int port, bool tls) {
    HttpRequest request;
    request.url = QUrl(QStringLiteral("%1://127.0.0.1:%2/v1/capabilities")
                           .arg(tls ? QStringLiteral("https") : QStringLiteral("http"))
                           .arg(port));
    request.method = QByteArrayLiteral("GET");
    if (tls) {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        request.tls = ssl;
    }
    request.timeoutMs = kDeadlineMs;
    return request;
}

// Every outcome delivered, and the status of the last.
struct Delivered {
    int count = 0;
    int status = -1;
};

HttpDone countingInto(Delivered& delivered) {
    return [&delivered](const HttpResult& result) {
        ++delivered.count;
        delivered.status = result.response.status;
    };
}

} // namespace

TEST_CASE("exchange: the outcome never arrives inside the call that asked for it",
          "[http][exchange]") {
    // Port 0 is refused on the spot, so this failure is known before exchangeHttp returns: the
    // one outcome that could be handed over from inside it.
    QObject owner;
    Delivered delivered;

    exchangeHttp(&owner, getFrom(0, /*tls=*/false), {}, countingInto(delivered));
    CHECK(delivered.count == 0);

    REQUIRE(spinFor([&delivered] { return delivered.count > 0; }));
    settle();
    CHECK(delivered.count == 1);
    CHECK(delivered.status == 0);
}

TEST_CASE("exchange: an owner destroyed first takes the outcome with it", "[http][exchange]") {
    FakePairingListener listener;
    REQUIRE(listener.listening());
    listener.hold();
    auto owner = std::make_unique<QObject>();
    Delivered delivered;

    exchangeHttp(owner.get(), getFrom(listener.port(), /*tls=*/true), {}, countingInto(delivered));
    REQUIRE(spinFor([&listener] { return !listener.requests().empty(); }));
    owner.reset();
    listener.release();
    settle(500);

    CHECK(delivered.count == 0);
}

TEST_CASE("exchange: the outcome may destroy the owner it is delivered to", "[http][exchange]") {
    // A client whose last wait this answer ends tears itself down right here, with the exchange
    // still on the stack.
    FakePairingListener listener;
    REQUIRE(listener.listening());
    auto owner = std::make_unique<QObject>();
    Delivered delivered;

    exchangeHttp(owner.get(), getFrom(listener.port(), /*tls=*/true), {},
                 [&delivered, &owner](const HttpResult& result) {
                     ++delivered.count;
                     delivered.status = result.response.status;
                     owner.reset();
                 });
    REQUIRE(spinFor([&delivered] { return delivered.count > 0; }));
    settle();

    CHECK(delivered.count == 1);
    CHECK(delivered.status == 200);
    CHECK(owner == nullptr);
}
