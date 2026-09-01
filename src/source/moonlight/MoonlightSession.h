// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One live (or connecting) Moonlight streaming session to a single host. Owns
// the launch coordinator: it turns the MoonlightSessionMachine's effects into
// HTTP, RTSP and control-stream actions, threads the transport data (RTSP
// ports, the control connect token, the launch rikey) between them, opens the
// RTP hole-punch pings so the host sees the media ports, and routes inbound
// host events (rumble, trigger rumble, motion requests, RGB LED) back out.
//
// The hot path (controller state -> CONTROLLER_MULTI) is delegated straight to
// MoonlightControlStream::sendControllerMulti; this class does not sit in it.

#pragma once

#include "core/moonlight/MoonlightPadSlots.h"
#include "core/moonlight/MoonlightRtsp.h"
#include "core/moonlight/MoonlightSessionMachine.h"
#include "core/moonlight/MoonlightTelemetry.h"
#include "core/moonlight/MoonlightTouchDiffer.h"
#include "core/moonlight/MoonlightTriggerRumble.h"
#include "core/moonlight/MoonlightWire.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightControlStream.h"
#include "source/moonlight/MoonlightHttp.h"
#include "source/moonlight/MoonlightRtspClient.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <QHash>

#include <array>
#include <atomic>
#include <map>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

class QTimer;
class QUdpSocket;

namespace dish::source::moon {

// Mirrors dish::net::SessionState so the connection row layer treats a Moonlight
// link exactly like a satellite one.
enum class MoonlightLinkState : std::uint8_t { Idle, Linking, Live, Failed };

class MoonlightSession : public QObject {
    Q_OBJECT
  public:
    // `http` is shared (one QNetworkAccessManager per manager); the session
    // borrows it. `controlStream` and `rtsp` are owned here.
    MoonlightSession(MoonlightHttp* http, repository::MoonlightHost host,
                     QObject* parent = nullptr);
    ~MoonlightSession() override;

    const QString& hostUuid() const { return host_.uuid; }
    MoonlightLinkState linkState() const { return linkState_; }
    const repository::MoonlightHost& host() const { return host_; }
    const moonlight::SessionState& machineState() const { return machine_; }
    // The app this session settled on, set by whoever created it. Every later
    // binding joins that app; it is never asked again.
    const QString& appId() const { return appId_; }
    const QString& appName() const { return appName_; }
    // A session has been attempted at least once since this object existed, so
    // an Idle phase means closed rather than never started.
    bool everStarted() const { return everStarted_; }
    // What the host said in the BODY of the refusal that ended the last
    // attempt, verbatim. A host refuses for reasons of its own and phrases them
    // itself; paraphrasing them would drop the only detail the user can act on.
    const QString& refusalMessage() const { return refusalMessage_; }

    void start(const QString& appId, const QString& appName);
    // `handBackApp` forces the /cancel a normal teardown only sends for an app
    // that never went live: the LAST unbind must not strand a running app.
    void stop(bool handBackApp = false);

    // ── Controllers riding this session (reference counting lives here) ──────
    // Assigns the lowest free controller number and announces the pad, either
    // now (the stream is already up) or when it comes up. nullopt means the
    // session already carries four pads, or this slot already holds one.
    std::optional<std::uint8_t> attachController(const QString& slotId, int storedType,
                                                 const moonlight::SourceCapabilities& source);
    // Clears the pad's bit and sends the unplug, then reports how many
    // controllers are left. Zero is the caller's cue to tear the session down.
    std::size_t detachController(const QString& slotId);
    std::size_t controllerCount() const { return slots_.size(); }
    std::optional<std::uint8_t> controllerNumber(const QString& slotId) const;
    QString slotForController(std::uint8_t number) const;

    // Hot path (SDL input thread): forward one controller's state. The number
    // is resolved once at bind time and passed in; the active mask is read from
    // an atomic, so neither costs a lookup here.
    void sendControllerState(std::uint8_t controllerNumber, std::uint16_t internalButtons,
                             std::uint8_t lt, std::uint8_t rt, std::int16_t lx, std::int16_t ly,
                             std::int16_t rx, std::int16_t ry);
    // Motion, on the SDL sensor thread. Gated by the host's MOTION_EVENT
    // subscriptions per (pad, motion type) and rate-limited to what it asked
    // for: one session drives up to four pads and a host subscribes to each
    // independently, so a session-wide flag would start every pad's stream the
    // moment one game opened one sensor. Returns whether the sample went out.
    bool sendMotion(std::uint8_t controllerNumber, std::uint8_t motionType, float x, float y,
                    float z);
    // Battery, on the same thread as motion. Unconditional: a host that
    // declared the capability gets the level whenever the pad reports one.
    void sendBattery(std::uint8_t controllerNumber, std::uint8_t state, std::uint8_t percentage);
    // One diffed touch event. Never rate-gated: the events are transitions, and
    // dropping one strands a contact on the host.
    void sendTouch(std::uint8_t controllerNumber, const moonlight::TouchEvent& event);

    bool motionRequested(std::uint8_t controllerNumber, std::uint8_t motionType) const {
        return motionGate_.wanted(controllerNumber, motionType);
    }
    // Forget a pad's subscriptions when it unbinds, so a returning pad waits to
    // be asked again rather than resuming a stream the host has forgotten.
    void forgetMotionSubscriptions(std::uint8_t controllerNumber) {
        motionGate_.clear(controllerNumber);
    }

    // Host->client actuation, delivered on the Qt main thread. The controller
    // number is carried through: a session drives up to four pads, so an event
    // that named none of them could only be applied to the wrong one.
    // Already MIXED and already mapped onto the pad's two motors: the host's
    // body and trigger rumble streams both land here (no pad this client can
    // claim has trigger motors), and the wire's lowFrequency is the large
    // motor. Handing over `strong`/`weak` rather than the wire's low/high is
    // what stops the actuator having to re-derive that mapping, which is where
    // this path used to invert the two.
    using RumbleHandler = std::function<void(std::uint8_t controllerNumber, std::uint16_t strong,
                                             std::uint16_t weak)>;
    using LedHandler = std::function<void(std::uint8_t controllerNumber, std::uint8_t r,
                                          std::uint8_t g, std::uint8_t b)>;
    void setRumbleHandler(RumbleHandler handler) { rumbleHandler_ = std::move(handler); }
    void setLedHandler(LedHandler handler) { ledHandler_ = std::move(handler); }

  signals:
    void linkStateChanged();
    // Terminal failure reason token, for the UI toast.
    void failed(const QString& reasonToken);

    // Internal: the control stream's service thread reports through these, and
    // a queued connection back to this object lands each report on the Qt
    // loop. A signal rather than a lambda invokeMethod because the queued
    // call is then built inside Qt, whose hand-off TSan already knows to
    // trust; a lambda posted from the service thread is allocated in our own
    // code and read on the Qt thread, which reads as a race between the two.
    void controlLinkChanged(bool connected);
    void hostEventReceived(dish::moonwire::HostEvent event);

  private:
    void dispatch(const moonlight::SessionEvent& event);
    void run(const moonlight::Reduction& reduction);
    void runEffect(moonlight::SessionEffect effect);
    void setLinkState(MoonlightLinkState state);
    // Wraps a reply handler so it is dropped if this session is gone by the
    // time the reply lands. The gateway is the manager's and outlives every
    // session, so nothing severs a handler on its own; a forget, or the
    // manager's own teardown, deletes a session with its /serverinfo or
    // /launch still in flight.
    MoonlightHttp::BodyCb guarded(MoonlightHttp::BodyCb cb);
    // The second half of the serverinfo check, over mutual TLS: the only call
    // that can answer whether the host still trusts this client.
    void askTrust();
    // Announces one attached pad to the host. No-op unless the control link is
    // up; startStreaming() re-announces every pad when it comes up.
    void announcePad(const QString& slotId);

    // Effect handlers.
    void fetchServerInfo();
    void sendLaunch();
    void openRtsp();
    void sendRtspStep(moonlight::RtspStep step);
    void connectControl();
    void startStreaming();
    void teardown();
    // Opens the media sockets and starts the ping timer the moment SETUP names
    // a port. The host counts its initial-ping deadline from its own session
    // start, not from when our control channel comes up, so waiting for the
    // ENet connect is already too late. Idempotent.
    void ensureRtpPings();
    // One ping per media port. Repeated every tick until teardown so a lost
    // datagram cannot leave the host blind to our media address.
    void sendRtpPings();
    // Hands back an app the host started for us and we could not use, so the
    // next attempt is not refused by our own leftovers.
    void cancelStrandedApp();

    // Applies one host event; runs on the Qt main thread, delivered by the
    // queued hostEventReceived connection.
    void onHostEvent(const moonwire::HostEvent& event);

    MoonlightHttp* http_;
    repository::MoonlightHost host_;
    std::unique_ptr<MoonlightControlStream> control_;
    std::unique_ptr<MoonlightRtspClient> rtsp_;

    moonlight::SessionState machine_;
    MoonlightLinkState linkState_ = MoonlightLinkState::Idle;

    // Per-attempt parameters. The app is per SESSION: only the binding that
    // creates it picks one, and every later binding joins whatever is running.
    QString appId_;
    QString appName_;

    // What one attached pad declares. Resolved once at attach time so the
    // announce is a lookup and never a decision.
    struct PadDeclaration {
        std::uint8_t number = 0;
        std::uint8_t type = moonproto::kControllerTypeXbox;
        std::uint8_t capabilities = 0;
        std::uint32_t buttons = moonproto::kStandardButtons;
    };
    moonlight::PadSlots slots_;
    QHash<QString, PadDeclaration> pads_;
    // The CONTROLLER_MULTI active mask, published for the hot path. Written on
    // the Qt thread by attach/detach, read on the SDL input thread.
    std::atomic<std::uint16_t> activeMask_{0};

    bool everStarted_ = false;
    QString refusalMessage_;
    // The teardown must hand the app back even though it went live: the last
    // controller has left, so nothing is riding it any more.
    bool handBackOnTeardown_ = false;

    // What the launch mode and the ANNOUNCE SDP ask for: the host's own
    // display, so a virtual-display host does not resize the user's desktop.
    moonrtsp::StreamConfig stream_;

    // Transport data threaded between phases. The rikey is minted once per
    // attempt so a launch that promotes to /resume keys the control stream
    // with the same secret it already announced.
    std::array<std::uint8_t, 16> rikey_{};
    bool rikeyReady_ = false;
    std::uint32_t rikeyId_ = 0;
    // A launch succeeded, so the host is holding an app on our behalf.
    bool launched_ = false;
    // The session reached Streaming, so a later drop is not a setup failure.
    bool wentLive_ = false;
    QString rtspTarget_; // parroted host string from the launch response
    QString rtspHostAddress_;
    int rtspPort_ = 0;
    QString rtspSessionId_;
    int controlPort_ = 0;
    std::uint32_t controlConnectData_ = 0;
    int audioPort_ = 0;
    int videoPort_ = 0;
    // The SETUP-provided X-SS-Ping-Payload per stream; empty falls back to the
    // legacy 4-byte "PING".
    QByteArray audioPingPayload_;
    QByteArray videoPingPayload_;
    int rtspCseq_ = 1;

    // The RTP hole-punch senders, alive only while Streaming. Payloads are
    // discarded on readyRead; the timer re-pings so a lost datagram (or a NAT
    // rebind) cannot strand the media ports.
    QUdpSocket* rtpVideoSocket_ = nullptr;
    QUdpSocket* rtpAudioSocket_ = nullptr;
    QTimer* rtpPingTimer_ = nullptr;
    std::uint32_t rtpPingSequence_ = 0;

    // Written from the control-stream receive thread, read from the input
    // thread; the gate carries its own lock.
    mutable moonlight::MoonlightMotionGate motionGate_;
    // Per controller number, the live mix of the host's two rumble streams. Only
    // touched on the control receive thread (both handlers run there), so no
    // lock of its own.
    std::map<int, moonlight::RumbleMix> rumbleMix_;

    RumbleHandler rumbleHandler_;
    LedHandler ledHandler_;
};

} // namespace dish::source::moon

Q_DECLARE_METATYPE(dish::moonwire::HostEvent)
