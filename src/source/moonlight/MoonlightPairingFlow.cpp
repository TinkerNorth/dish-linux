// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightPairingFlow.h"

#include "core/moonlight/MoonlightPairingCrypto.h"
#include "core/moonlight/MoonlightXml.h"
#include "source/moonlight/MoonlightLog.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace dish::source::moon {
namespace {

bool fillRandom(std::array<std::uint8_t, 16>& out) {
    return mooncrypto::randomBytes(out.data(), out.size());
}

// The host reports a refused pairing phase in the body, not the status line, so
// the phase log carries both.
void logPhase(const char* phase, const QString& address, int status, const std::string& xml) {
    const auto refusal = moonxml::parseStatus(xml);
    const QString says = refusal ? QStringLiteral("%1 %2")
                                       .arg(refusal->code)
                                       .arg(QString::fromStdString(refusal->message))
                                 : QStringLiteral("(no root element)");
    qCInfo(lcMoon) << "pair" << phase << "on" << address << "HTTP" << status << "host" << says
                   << "paired" << moonxml::pairedFlag(xml);
}

} // namespace

MoonlightPairingFlow::MoonlightPairingFlow(MoonlightHttp* http, QObject* parent)
    : QObject(parent), http_(http) {}

void MoonlightPairingFlow::start(const QString& hostUuid, const QString& address, int httpPort,
                                 int httpsPort, const QString& clientCertPem,
                                 const QString& clientKeyPem, const QString& deviceName) {
    ++attempt_;
    active_ = true;
    hostUuid_ = hostUuid;
    address_ = address;
    httpPort_ = httpPort;
    httpsPort_ = httpsPort;
    deviceName_ = deviceName;

    std::array<std::uint8_t, 16> salt{};
    std::array<std::uint8_t, 16> challenge{};
    std::array<std::uint8_t, 16> secret{};
    std::uint32_t pinRandom = 0;
    if (!fillRandom(salt) || !fillRandom(challenge) || !fillRandom(secret) ||
        !mooncrypto::randomBytes(reinterpret_cast<std::uint8_t*>(&pinRandom), sizeof(pinRandom))) {
        fail(QStringLiteral("crypto"));
        return;
    }
    pin_ = QString::fromStdString(moonpair::pinFromRandom(pinRandom));
    session_ = std::make_unique<moonpair::PairingSession>(clientCertPem.toStdString(),
                                                          clientKeyPem.toStdString(), salt,
                                                          pin_.toStdString(), challenge, secret);
    emit pinReady(pin_);
    phase1();
}

void MoonlightPairingFlow::cancel() {
    ++attempt_;
    active_ = false;
    session_.reset();
    pin_.clear();
}

void MoonlightPairingFlow::fail(const QString& reasonToken) {
    qCWarning(lcMoon) << "pairing with" << address_ << "gave up:" << reasonToken;
    active_ = false;
    session_.reset();
    emit finished(false, reasonToken, QString());
}

void MoonlightPairingFlow::phase1() {
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("devicename"), deviceName_);
    query.addQueryItem(QStringLiteral("updateState"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("phrase"), QStringLiteral("getservercert"));
    query.addQueryItem(QStringLiteral("salt"), QString::fromStdString(session_->saltHex()));
    query.addQueryItem(QStringLiteral("clientcert"),
                       QString::fromStdString(session_->clientCertHex()));
    // Blocks host-side until the user types the PIN, hence the long timeout.
    http_->getPlain(
        address_, httpPort_, QStringLiteral("/pair"), query,
        [this, attempt = attempt_](int status, const QByteArray& body) {
            if (!current(attempt)) { return; }
            const std::string xml = body.toStdString();
            logPhase("getservercert", address_, status, xml);
            if (status != 200 || !moonxml::pairedFlag(xml)) {
                fail(status == 0 ? QStringLiteral("unreachable") : QStringLiteral("declined"));
                return;
            }
            const auto plaincert = moonxml::tagValue(xml, "plaincert");
            if (!plaincert || !session_->acceptServerCert(*plaincert)) {
                fail(QStringLiteral("crypto"));
                return;
            }
            phase2();
        },
        MoonlightHttp::kPairingTimeoutMs);
}

void MoonlightPairingFlow::phase2() {
    const auto challenge = session_->clientChallengeHex();
    if (!challenge) {
        fail(QStringLiteral("crypto"));
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("devicename"), deviceName_);
    query.addQueryItem(QStringLiteral("updateState"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("clientchallenge"), QString::fromStdString(*challenge));
    http_->getPlain(address_, httpPort_, QStringLiteral("/pair"), query,
                    [this, attempt = attempt_](int status, const QByteArray& body) {
                        if (!current(attempt)) { return; }
                        const std::string xml = body.toStdString();
                        logPhase("clientchallenge", address_, status, xml);
                        const auto response = moonxml::tagValue(xml, "challengeresponse");
                        if (status != 200 || !moonxml::pairedFlag(xml) || !response) {
                            fail(status == 0 ? QStringLiteral("unreachable")
                                             : QStringLiteral("declined"));
                            return;
                        }
                        const auto next = session_->acceptChallengeResponse(*response);
                        if (!next) {
                            fail(QStringLiteral("crypto"));
                            return;
                        }
                        phase3(QString::fromStdString(*next));
                    });
}

void MoonlightPairingFlow::phase3(const QString& serverChallengeResp) {
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("devicename"), deviceName_);
    query.addQueryItem(QStringLiteral("updateState"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("serverchallengeresp"), serverChallengeResp);
    http_->getPlain(address_, httpPort_, QStringLiteral("/pair"), query,
                    [this, attempt = attempt_](int status, const QByteArray& body) {
                        if (!current(attempt)) { return; }
                        const std::string xml = body.toStdString();
                        logPhase("serverchallengeresp", address_, status, xml);
                        const auto secret = moonxml::tagValue(xml, "pairingsecret");
                        if (status != 200 || !moonxml::pairedFlag(xml) || !secret) {
                            fail(status == 0 ? QStringLiteral("unreachable")
                                             : QStringLiteral("declined"));
                            return;
                        }
                        // The failing case here is exactly what a mistyped PIN
                        // produces: the phase-2 hash never matches.
                        if (!session_->acceptPairingSecret(*secret)) {
                            fail(QStringLiteral("wrongPin"));
                            return;
                        }
                        phase4();
                    });
}

void MoonlightPairingFlow::phase4() {
    const auto clientSecret = session_->clientPairingSecretHex();
    if (!clientSecret) {
        fail(QStringLiteral("crypto"));
        return;
    }
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("devicename"), deviceName_);
    query.addQueryItem(QStringLiteral("updateState"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("clientpairingsecret"),
                       QString::fromStdString(*clientSecret));
    http_->getPlain(address_, httpPort_, QStringLiteral("/pair"), query,
                    [this, attempt = attempt_](int status, const QByteArray& body) {
                        if (!current(attempt)) { return; }
                        logPhase("clientpairingsecret", address_, status, body.toStdString());
                        if (status != 200 || !moonxml::pairedFlag(body.toStdString())) {
                            fail(status == 0 ? QStringLiteral("unreachable")
                                             : QStringLiteral("wrongPin"));
                            return;
                        }
                        phase5();
                    });
}

void MoonlightPairingFlow::phase5() {
    // Over HTTPS with the freshly-authorized client cert, pinned against the
    // server cert phase 1 delivered: proves the secure channel end to end.
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("devicename"), deviceName_);
    query.addQueryItem(QStringLiteral("updateState"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("phrase"), QStringLiteral("pairchallenge"));
    const QString serverCert = QString::fromStdString(session_->serverCertPem());
    http_->getTls(address_, httpsPort_, QStringLiteral("/pair"), query, serverCert,
                  [this, serverCert, attempt = attempt_](int status, const QByteArray& body) {
                      if (!current(attempt)) { return; }
                      logPhase("pairchallenge", address_, status, body.toStdString());
                      if (status != 200 || !moonxml::pairedFlag(body.toStdString())) {
                          fail(QStringLiteral("unreachable"));
                          return;
                      }
                      active_ = false;
                      session_.reset();
                      emit finished(true, QString(), serverCert);
                  });
}

} // namespace dish::source::moon
