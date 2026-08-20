// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/http/SatelliteTlsVerifier.h"

#include "core/net/Tofu.h"

namespace dish::http {

bool verifyPeerCertificate(const QString& satelliteId, repository::SatellitePinRepository& pins,
                           const QByteArray& certDer, const std::function<void()>& onMismatch) {
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
        if (onMismatch) { onMismatch(); }
        return false; // leave the trusted pin intact
    }
    return false;
}

} // namespace dish::http
