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

#include "core/moonlight/MoonlightRtsp.h"
#include "core/moonlight/MoonlightSessionMachine.h"
#include "core/moonlight/MoonlightWire.h"
#include "repository/MoonlightHostRepository.h"
#include "source/moonlight/MoonlightControlStream.h"
#include "source/moonlight/MoonlightHttp.h"
#include "source/moonlight/MoonlightRtspClient.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

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

    // `emulatedType` is a moonproto::kControllerType* (Xbox/PS/Nintendo) or the
    // repo's Auto sentinel; `capabilities` the local pad's real feature bits.
    void start(const QString& appId, std::uint8_t emulatedType, std::uint8_t capabilities);
    void stop();

    // Hot path (SDL input thread): forward the current controller state.
    void sendControllerState(std::uint16_t internalButtons, std::uint8_t lt, std::uint8_t rt,
                             std::int16_t lx, std::int16_t ly, std::int16_t rx, std::int16_t ry);
    // Motion, on the SDL sensor thread. Gated by a host MOTION_EVENT request.
    void sendMotion(std::uint8_t motionType, float x, float y, float z);
    bool motionRequested() const { return motionRequested_; }

    // Host->client actuation, delivered on the Qt main thread.
    using RumbleHandler = std::function<void(std::uint16_t low, std::uint16_t high)>;
    using LedHandler = std::function<void(std::uint8_t r, std::uint8_t g, std::uint8_t b)>;
    void setRumbleHandler(RumbleHandler handler) { rumbleHandler_ = std::move(handler); }
    void setLedHandler(LedHandler handler) { ledHandler_ = std::move(handler); }

  signals:
    void linkStateChanged();
    // Terminal failure reason token, for the UI toast.
    void failed(const QString& reasonToken);

  private:
    void dispatch(const moonlight::SessionEvent& event);
    void run(const moonlight::Reduction& reduction);
    void runEffect(moonlight::SessionEffect effect);
    void setLinkState(MoonlightLinkState state);

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

    // Marshals a host event from the control thread onto the Qt main thread.
    void onHostEvent(const moonwire::HostEvent& event);

    MoonlightHttp* http_;
    repository::MoonlightHost host_;
    std::unique_ptr<MoonlightControlStream> control_;
    std::unique_ptr<MoonlightRtspClient> rtsp_;

    moonlight::SessionState machine_;
    MoonlightLinkState linkState_ = MoonlightLinkState::Idle;

    // Per-attempt parameters.
    QString appId_;
    std::uint8_t emulatedType_ = moonproto::kControllerTypeXbox;
    std::uint8_t capabilities_ = 0;

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

    bool motionRequested_ = false;

    RumbleHandler rumbleHandler_;
    LedHandler ledHandler_;
};

} // namespace dish::source::moon
