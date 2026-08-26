// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Owns the whole Moonlight-host subsystem beside the satellite WifiConnection
// pool: the client identity, the remembered-host store, mDNS discovery, the
// PIN pairing flow and the live sessions. Presents the same shape the rest of
// the app already consumes for satellites (a list of rows with a link state,
// pair/connect/forget commands, a per-slot hot-path sender), so the UI and the
// controller-routing plumbing treat a Moonlight host as one more connection.

#pragma once

#include "repository/MoonlightHostRepository.h"
#include "repository/MoonlightIdentityRepository.h"
#include "source/moonlight/MoonlightDiscovery.h"
#include "source/moonlight/MoonlightHttp.h"
#include "source/moonlight/MoonlightPairingFlow.h"
#include "source/moonlight/MoonlightSession.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>

#include <memory>

class QThread;

namespace dish::source::moon {

// A row the UI renders, aligned with the satellite ConnectionSummary vocabulary.
struct MoonlightRow {
    QString uuid;
    QString name;
    QString address;
    bool paired = false;
    bool discovered = false;
    MoonlightLinkState link = MoonlightLinkState::Idle;
    QString lastAppId;
    QString lastAppName;
    int controllerType = repository::kMoonlightControllerTypeAuto;
};

class MoonlightManager : public QObject {
    Q_OBJECT
  public:
    // `settings` co-tenants the shared connection-store file; nullptr → default.
    // Taken by const reference, not by value as the repositories take it: this
    // shares the pointer with three collaborators rather than sinking it.
    explicit MoonlightManager(const std::shared_ptr<QSettings>& settings = nullptr,
                              QObject* parent = nullptr);
    ~MoonlightManager() override;

    QList<MoonlightRow> rows() const;
    bool isScanning() const { return scanning_; }

    // Kicks a background mDNS scan; merges results into the discovered set.
    void startDiscovery();

    // Adds (or refreshes) a host by address the user typed; it appears as an
    // unpaired discovered row so the user can start pairing.
    void addManualHost(const QString& address, int httpPort = 47989, int httpsPort = 47984);

    // Begins PIN pairing. pairingPinChanged() then carries the PIN to show.
    void pair(const QString& uuid);
    void cancelPairing();
    QString pairingPin() const { return pairingFlow_->pin(); }
    QString pairingHostUuid() const { return pairingFlow_->hostUuid(); }
    bool pairingActive() const { return pairingFlow_->active(); }

    // Launches `appId` on a paired host and brings the control link up. When a
    // slot is later bound, sendReport routes to this session.
    void connectHost(const QString& uuid, const QString& appId, std::uint8_t emulatedType,
                     std::uint8_t capabilities);
    void disconnect(const QString& uuid);
    void forget(const QString& uuid);

    // Tells a paired host to end whatever app it is running. The protocol's own
    // way out of "an app is already running", and the only one when the host
    // will not hand that session over.
    void cancelHostApp(const QString& uuid);

    void setLastApp(const QString& uuid, const QString& appId, const QString& appName);
    void setControllerType(const QString& uuid, int type);

    // The live session for a host, or nullptr. The routing layer holds the
    // returned pointer only for the duration of one call.
    MoonlightSession* session(const QString& uuid) const;

    // Host->local actuation: wired to the same SDL output plumbing the
    // satellite rumble/LED path uses.
    using RumbleSink =
        std::function<void(const QString& uuid, std::uint16_t low, std::uint16_t high)>;
    using LedSink =
        std::function<void(const QString& uuid, std::uint8_t r, std::uint8_t g, std::uint8_t b)>;
    void setRumbleSink(RumbleSink sink) { rumbleSink_ = std::move(sink); }
    void setLedSink(LedSink sink) { ledSink_ = std::move(sink); }

  signals:
    void rowsChanged();
    void scanningChanged();
    void pairingChanged();
    // reasonToken: "" on success, else "unreachable"|"wrongPin"|"declined"|"crypto".
    void pairingFinished(const QString& uuid, bool ok, const QString& reasonToken);
    // reasonToken: "unreachable"|"notPaired"|"launchRejected"|"appAlreadyRunning"
    // |"rtspRejected"|"controlLost"|"hostEnded".
    void sessionFailed(const QString& uuid, const QString& reasonToken);
    void hostAppCancelled(const QString& uuid, bool ok);

  private:
    void ensureIdentityLoaded();
    void onDiscovered(const QList<DiscoveredMoonlightHost>& hosts);
    MoonlightSession* ensureSession(const repository::MoonlightHost& host);
    void wireSession(MoonlightSession* session, const QString& uuid);

    std::shared_ptr<QSettings> settings_;
    repository::MoonlightIdentityRepository identityRepo_;
    repository::MoonlightHostRepository hostRepo_;
    MoonlightHttp* http_;
    std::unique_ptr<MoonlightPairingFlow> pairingFlow_;

    QString deviceName_;
    bool identityReady_ = false;

    bool scanning_ = false;
    // Discovered-but-not-yet-remembered hosts, keyed by a synthetic uuid
    // ("addr:<address>") until serverinfo hands back the real one.
    QHash<QString, MoonlightRow> discovered_;
    QHash<QString, MoonlightSession*> sessions_;

    RumbleSink rumbleSink_;
    LedSink ledSink_;
};

} // namespace dish::source::moon
