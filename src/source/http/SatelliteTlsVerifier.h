// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// SatelliteTlsVerifier — the TOFU cert-pinning gate HTTPClient runs from its
// post-handshake hook. The satellite presents a self-signed cert with no CA
// chain, so trust is Trust-On-First-Use: pin the SHA-256 of the cert first seen
// for a satellite id, then reject any later cert whose fingerprint differs.
//
// It composes the pure verdict ladder (core/net/Tofu) with the durable pin store
// (repository/SatellitePinRepository), and takes no Qt or TLS types so it tests
// against a fake cert byte buffer.

#pragma once

#include "repository/SatellitePinRepository.h"

#include <QByteArray>
#include <QString>

#include <functional>

namespace dish::http {

// Consults + updates `pins`. An empty `certDer` models "no peer certificate".
//   * no cert            -> false, onMismatch NOT called
//   * never pinned       -> pin(satelliteId, fp), return true (first use)
//   * fingerprint match  -> true (do not re-pin)
//   * fingerprint differ -> false, pin LEFT INTACT (anti-MITM), onMismatch once
//   * fingerprint differ, and no pairing behind the pin
//                        -> pin(satelliteId, fp), return true: a first use again, since a pin
//                           with no pairing behind it protects nothing
bool verifyPeerCertificate(const QString& satelliteId, repository::SatellitePinRepository& pins,
                           const QByteArray& certDer, const std::function<void()>& onMismatch = {},
                           bool pinGuardsAPairing = true);

// Whether a key is on file for a satellite at `host`: the pairing the pin there stands in front of.
using PairingAt = std::function<bool(const QString& host)>;

// The check above over `pins`, in the shape HTTPClient runs on every handshake: one long-lived
// verifier handed a fresh flag per request, raised only for a changed cert. That flag is all that
// tells a changed identity from a dead link once the abort has erased the status and the body.
// `pins` must outlive the verifier.
std::function<bool(const QString& host, const QByteArray& certDer, bool& pinMismatch)>
pinVerifierOver(repository::SatellitePinRepository& pins, PairingAt pairedAt);

} // namespace dish::http
