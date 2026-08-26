// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Owns the whole Moonlight-host subsystem beside the satellite WifiConnection
// pool: the client identity, the remembered-host store, mDNS discovery, the
// PIN pairing flow and the live sessions. Presents the same shape the rest of
// the app already consumes for satellites (a list of rows with a link state,
// pair/forget commands, a per-slot hot-path sender), so the UI and the
// controller-routing plumbing treat a Moonlight host as one more connection.
//
// ONE SESSION PER HOST, REFERENCE COUNTED. A Moonlight session carries up to
// four controllers (a controller number plus an active mask), so it belongs to
// the HOST and not to a binding. The first binding on a host starts or joins
// it and settles the app; every later binding only announces its own pad. The
// last unbind hands the app back with /cancel, so nothing is left stranded.
//
// TRUST IS REMEMBERED AND VERIFIED LAZILY. There is no bidirectional liveness
// to watch: probe() re-asks the host on entering a screen and before starting a
// session, and nothing polls.

#pragma once

#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightSessionUi.h"
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
#include <optional>

class QThread;

namespace dish::source::moon {

// One app the host offers, as /applist returns it.
struct MoonlightApp {
    QString id;
    QString title;
};

// A row the UI renders, aligned with the satellite ConnectionSummary vocabulary.
struct MoonlightRow {
    QString uuid;
    QString name;
    QString address;
    bool paired = false;
    bool discovered = false;
    MoonlightLinkState link = MoonlightLinkState::Idle;
    // Remembered trust, verified this visit where the host answered. Never a
    // liveness light: a Moonlight host cannot report one.
    moonlight::HostTrust trust = moonlight::HostTrust::NotPaired;
    // The richer phase the row chip reads, converged with dish-windows.
    moonlight::HostPhase phase = moonlight::HostPhase::Idle;
    // Controllers currently riding this host's session.
    int controllers = 0;
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
    // A uuid this subsystem knows: a remembered host, or one discovery found.
    bool knows(const QString& uuid) const;
    std::optional<MoonlightRow> row(const QString& uuid) const;

    // Kicks a background mDNS scan; merges results into the discovered set.
    void startDiscovery();

    // Adds (or refreshes) a host by address the user typed; it appears as an
    // unpaired discovered row so the user can start pairing.
    void addManualHost(const QString& address, const QString& name = QString(),
                       int httpPort = 47989, int httpsPort = 47984);

    // Begins PIN pairing. pairingPinChanged() then carries the PIN to show.
    void pair(const QString& uuid);
    void cancelPairing();
    QString pairingPin() const { return pairingFlow_->pin(); }
    QString pairingHostUuid() const { return pairingFlow_->hostUuid(); }
    bool pairingActive() const { return pairingFlow_->active(); }
    // The last attempt finished not-ok and the user has not started another.
    bool pairingRefused(const QString& uuid) const;

    // Re-asks the host what it is: reachable, still paired, still the same
    // machine. Client-initiated by definition; call it on entering a screen and
    // before starting a session, never on a timer.
    void probe(const QString& uuid);

    // GET /applist over the pinned mutual-TLS channel. The list is HTTPS and
    // paired-only, so an unpaired host answers 404 and this reports a failure
    // the UI renders rather than an empty list it would present as truth.
    void refreshApps(const QString& uuid);
    QList<MoonlightApp> apps(const QString& uuid) const;

    void forget(const QString& uuid);

    // Tells a paired host to end whatever app it is running, tearing down our
    // own session first when we hold one. The protocol's own way out of "an app
    // is already running", and the only one when the host will not hand that
    // session over. /cancel answers 200 either way, so this re-probes after.
    void quitHostApp(const QString& uuid);

    void setLastApp(const QString& uuid, const QString& appId, const QString& appName);
    void setControllerType(const QString& uuid, int type);

    // ── Bindings (the reference count) ──────────────────────────────────────
    // Records the binding and, once the host is paired, starts or joins its
    // session and announces this pad. Returns the controller number, or nullopt
    // when the host is not paired yet (the binding is still recorded: a binding
    // is a durable intent and the session is attempted when the pad is used).
    std::optional<std::uint8_t> bindController(const QString& slotId, const QString& uuid,
                                               int storedType,
                                               const moonlight::SourceCapabilities& source);
    void unbindController(const QString& slotId);
    QString boundHostFor(const QString& slotId) const;
    QStringList boundSlots(const QString& uuid) const;
    int controllerCount(const QString& uuid) const;
    std::optional<std::uint8_t> controllerNumber(const QString& slotId) const;

    // Everything the binding-flow render contract reads about one host, from
    // the point of view of `slotId` (empty for a binding that does not exist
    // yet). Pure data; MoonlightSessionUi turns it into exactly one state.
    moonlight::SessionUiInputs uiInputs(const QString& uuid, const QString& slotId) const;

    // The live session for a host, or nullptr. The routing layer holds the
    // returned pointer only for the duration of one call.
    MoonlightSession* session(const QString& uuid) const;

    // Host->local actuation: wired to the same SDL output plumbing the
    // satellite rumble/LED path uses. The slot is already resolved from the
    // event's controller number, because one session drives up to four pads.
    using RumbleSink =
        std::function<void(const QString& slotId, std::uint16_t low, std::uint16_t high)>;
    using LedSink =
        std::function<void(const QString& slotId, std::uint8_t r, std::uint8_t g, std::uint8_t b)>;
    void setRumbleSink(RumbleSink sink) { rumbleSink_ = std::move(sink); }
    void setLedSink(LedSink sink) { ledSink_ = std::move(sink); }

  signals:
    void rowsChanged();
    void scanningChanged();
    void pairingChanged();
    // reasonToken: "" on success, else "unreachable"|"wrongPin"|"declined"|"crypto".
    void pairingFinished(const QString& uuid, bool ok, const QString& reasonToken);
    // reasonToken: one of MoonlightSession's, e.g. "unreachable"|"trustLost"
    // |"appAlreadyRunning"|"resumeFailed"|"dropped"|"hostEnded".
    void sessionFailed(const QString& uuid, const QString& reasonToken);
    void hostAppCancelled(const QString& uuid, bool ok);
    void appsChanged(const QString& uuid);
    void probeFinished(const QString& uuid);

  private:
    // What the last probe of one host learned. Absent means never asked.
    struct HostProbe {
        bool inFlight = false;
        bool answered = false;
        bool paired = false;
        bool identityChanged = false;
        bool trustRejected = false;
    };

    // The /applist read for one host.
    struct AppCache {
        bool inFlight = false;
        bool read = false;
        bool failed = false;
        QList<MoonlightApp> apps;
    };

    void ensureIdentityLoaded();
    void onDiscovered(const QList<DiscoveredMoonlightHost>& hosts);
    MoonlightSession* ensureSession(const repository::MoonlightHost& host);
    void wireSession(MoonlightSession* session, const QString& uuid);
    // Starts the session if nothing is running on it yet. The app comes from
    // the remembered pick, or the first row /applist returned, or the host's
    // own default.
    void ensureSessionRunning(MoonlightSession* session, const repository::MoonlightHost& host);

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
    QHash<QString, HostProbe> probes_;
    QHash<QString, AppCache> appCache_;
    // The host whose last pairing attempt was refused, cleared when another
    // one starts or the row is forgotten.
    QString pairingRefusedUuid_;
    // slotId -> host uuid. The binding table; the session's own PadSlots owns
    // the controller numbers.
    QHash<QString, QString> bindings_;

    RumbleSink rumbleSink_;
    LedSink ledSink_;
};

} // namespace dish::source::moon
