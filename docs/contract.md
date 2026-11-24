# Dish and Satellite contract: client notes (Linux)

The protocol contract (REST surface, UDP streams, crypto, liveness, identity)
lives in ONE place: **`satellite/docs/contract.md`** in the
[TinkerNorth/satellite](https://github.com/TinkerNorth/satellite) repo. Dish
for Linux offers protocol version 4 and still settles down to 1 against an
older satellite; this file only records where each part of the contract lands
in this tree. When the code and the contract disagree, the contract wins.

`src/core/` and `src/composer/` hold the pure, host-testable logic; the
sockets, `QNetworkAccessManager`, timers and stores live in `src/Network/`
and `src/source/`. `src/core/` is ported by hand from dish-windows, with
deliberate drift where the platforms differ, so a change to the wire is made
in both clients.

| Contract concept | dish-linux home |
|---|---|
| Protocol constants: the version this client offers and the lowest it settles to, opcodes, caps, the extended-button bits, cadences | [`src/core/model/Protocol.h`](../src/core/model/Protocol.h) (`kProtocolVersion`, `kProtocolVersionMin`) |
| HKDF session key, `hmacProof`, packet AEAD (direction nonce, AAD = token) | [`src/core/wire/SessionCrypto.h`](../src/core/wire/SessionCrypto.h), pinned by the cross-repo vectors in `tests/test_session_crypto.cpp` |
| HTTP framing for the control plane | [`src/core/wire/HttpFraming.h`](../src/core/wire/HttpFraming.h) |
| Versioning: the offer, the 409 range, the settled version that keys the frames | [`src/core/reducer/ProtocolNegotiation.h`](../src/core/reducer/ProtocolNegotiation.h) |
| UDP data plane: socket, per-direction counters, replay guard, heartbeat, the stream frames up (INPUT with the extended-buttons word on a session settled at 4, MOTION, BATTERY, TOUCHPAD, POINTER) and the return paths down | [`src/Network/SatelliteClient.h`](../src/Network/SatelliteClient.h), tested in `tests/test_satellite_client_*.cpp` |
| Controller audio frames (MIC_AUDIO up, SPEAKER_AUDIO and HAPTIC_AUDIO down, MIC_LED), the reorder window | `SatelliteClient`; [`src/core/audio/AudioJitter.h`](../src/core/audio/AudioJitter.h) and the policy in `src/core/audio/` |
| Session lifecycle: declarative PUT, HKDF key handoff, proactive re-key, per-slot converge | [`src/Network/WifiConnection.h`](../src/Network/WifiConnection.h), [`src/Network/WifiConnectionManager.h`](../src/Network/WifiConnectionManager.h), [`src/core/reducer/SatelliteSessionMachine.h`](../src/core/reducer/SatelliteSessionMachine.h) |
| Reconcile rules (epoch and bitmap drift, benign adopt versus re-PUT) | [`src/core/reducer/Reconcile.h`](../src/core/reducer/Reconcile.h) |
| Close-notify reason and the follow-up it causes | [`src/core/reducer/CloseNotify.h`](../src/core/reducer/CloseNotify.h) |
| REST error model (terminal 401 codes, 409, 503, transport) | [`src/core/reducer/RestOutcome.h`](../src/core/reducer/RestOutcome.h) |
| Authed REST routes and their headers | [`src/Network/HTTPClient.h`](../src/Network/HTTPClient.h) |
| Pairing paths A and B, and the outcome classification | [`src/core/reducer/PairingMachine.h`](../src/core/reducer/PairingMachine.h), [`src/core/reducer/ReversePairing.h`](../src/core/reducer/ReversePairing.h), [`src/Network/PairingOutcome.h`](../src/Network/PairingOutcome.h) |
| TOFU: the verdict ladder and the TLS pin | [`src/core/net/Tofu.h`](../src/core/net/Tofu.h), [`src/source/http/SatelliteTlsVerifier.cpp`](../src/source/http/SatelliteTlsVerifier.cpp) |
| Satellite identity (`mid:<machineId>`, re-homing) | [`src/Network/ConnectionStore.h`](../src/Network/ConnectionStore.h) |
| Discovery: mDNS `_satellite._udp` and the legacy UDP `:9879` beacon | [`src/source/connection/MdnsDiscovery.h`](../src/source/connection/MdnsDiscovery.h), [`src/source/connection/LANDiscovery.h`](../src/source/connection/LANDiscovery.h) |
| Touchpad mode from the pad's pick, the type's modes and the host's grant; a Steam Controller's trackpad pick | [`src/core/reducer/TouchpadModeResolve.h`](../src/core/reducer/TouchpadModeResolve.h), [`src/core/reducer/TrackpadPickResolve.h`](../src/core/reducer/TrackpadPickResolve.h) |
| The catalog (`GET /api/catalog`), its version and features | [`src/source/http/SatelliteCatalogRepository.h`](../src/source/http/SatelliteCatalogRepository.h), [`src/core/catalog/`](../src/core/catalog/) |
| Exponential reconnect backoff | [`src/core/reducer/Backoff.h`](../src/core/reducer/Backoff.h) |
| Latency readout | [`src/core/reducer/LatencyWindow.h`](../src/core/reducer/LatencyWindow.h) |

`scripts/check-version-consistency.sh` fails a change that moves
`kProtocolVersion` without this file and `CONTRIBUTING.md`, which both name
the version.

The Moonlight (GameStream) host support under `src/core/moonlight/` and
`src/source/moonlight/` is a separate protocol and not part of this contract;
[`MOONLIGHT_HOSTS.md`](MOONLIGHT_HOSTS.md) covers it.
