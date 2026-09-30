// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/http/SatelliteTlsVerifier.h"

#include "core/net/Tofu.h"

#include <utility>

namespace dish::http {

namespace {

// A satellite reinstalled after an approval request it never finished presents a new certificate
// to a pin nothing was ever paired behind; refusing it would leave that satellite unpairable.
bool onChangedCertificate(const QString& satelliteId, repository::SatellitePinRepository& pins,
                          const std::string& presented, const std::function<void()>& onMismatch,
                          bool pinGuardsAPairing) {
    if (!pinGuardsAPairing) {
        pins.pin(satelliteId, QString::fromStdString(presented));
        return true;
    }
    if (onMismatch) { onMismatch(); }
    return false; // leave the trusted pin intact
}

} // namespace

bool verifyPeerCertificate(const QString& satelliteId, repository::SatellitePinRepository& pins,
                           const QByteArray& certDer, const std::function<void()>& onMismatch,
                           bool pinGuardsAPairing) {
    // No peer certificate at all: reject, but this is NOT a pin mismatch.
    if (certDer.isEmpty()) { return false; }

    const std::string presented =
        net::sha256FingerprintHex(reinterpret_cast<const std::uint8_t*>(certDer.constData()),
                                  static_cast<std::size_t>(certDer.size()));

    const auto stored = pins.pinnedFingerprint(satelliteId);
    std::optional<std::string> storedStd;
    if (stored.has_value()) { storedStd = stored->toStdString(); }

    switch (net::tofuVerdict(storedStd, presented)) {
    case net::TofuVerdict::TrustFirstUse:
        pins.pin(satelliteId, QString::fromStdString(presented));
        return true;
    case net::TofuVerdict::Match:
        return true;
    case net::TofuVerdict::Mismatch:
        return onChangedCertificate(satelliteId, pins, presented, onMismatch, pinGuardsAPairing);
    }
    return false;
}

std::function<bool(const QString& host, const QByteArray& certDer, bool& pinMismatch)>
pinVerifierOver(repository::SatellitePinRepository& pins, PairingAt pairedAt) {
    return [&pins, pairedAt = std::move(pairedAt)](const QString& host, const QByteArray& certDer,
                                                   bool& pinMismatch) {
        return verifyPeerCertificate(
            host, pins, certDer, [&pinMismatch] { pinMismatch = true; }, pairedAt(host));
    };
}

} // namespace dish::http
