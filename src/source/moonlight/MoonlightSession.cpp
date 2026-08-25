// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightSession.h"

#include "Util/Hex.h"
#include "core/moonlight/MoonlightButtonMap.h"
#include "core/moonlight/MoonlightPairingCrypto.h"
#include "core/moonlight/MoonlightProtocol.h"
#include "core/moonlight/MoonlightXml.h"

#include <QMetaObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrlQuery>

#include <cstring>

namespace dish::source::moon {
namespace {

QString stepStreamId(moonlight::RtspStep step) {
    switch (step) {
    case moonlight::RtspStep::SetupAudio:
        return QStringLiteral("audio");
    case moonlight::RtspStep::SetupVideo:
        return QStringLiteral("video");
    case moonlight::RtspStep::SetupControl:
        return QStringLiteral("control");
    default:
        return {};
    }
}

// The floor mode string the launch request asks for: cheap for the host to set
// up, and the video is discarded anyway.
constexpr const char* kLaunchMode = "1280x720x30";

// Cadence of the RTP hole-punch pings, matching what real clients send. One
// datagram per port would be fragile: lose it and the host never learns our
// media address.
constexpr int kRtpPingIntervalMs = 500;

} // namespace

MoonlightSession::MoonlightSession(MoonlightHttp* http, repository::MoonlightHost host,
                                   QObject* parent)
    : QObject(parent), http_(http), host_(std::move(host)),
      control_(std::make_unique<MoonlightControlStream>()),
      rtsp_(std::make_unique<MoonlightRtspClient>()) {
    control_->setLinkHandler([this](bool connected) {
        // Hops off the control-stream service thread onto the Qt loop.
        QMetaObject::invokeMethod(
            this,
            [this, connected] {
                dispatch(connected
                             ? moonlight::SessionEvent{moonlight::moon_event::ControlConnected{}}
                             : moonlight::SessionEvent{moonlight::moon_event::ControlLost{}});
            },
            Qt::QueuedConnection);
    });
    control_->setEventHandler([this](const moonwire::HostEvent& event) { onHostEvent(event); });

    QObject::connect(rtsp_.get(), &MoonlightRtspClient::connected, this,
                     [this] { dispatch(moonlight::moon_event::RtspReady{}); });
    QObject::connect(rtsp_.get(), &MoonlightRtspClient::transportError, this,
                     [this] { dispatch(moonlight::moon_event::RtspFailed{}); });

    rtpPingTimer_ = new QTimer(this);
    rtpPingTimer_->setInterval(kRtpPingIntervalMs);
    QObject::connect(rtpPingTimer_, &QTimer::timeout, this, &MoonlightSession::sendRtpPings);
}

MoonlightSession::~MoonlightSession() { teardown(); }

void MoonlightSession::start(const QString& appId, std::uint8_t emulatedType,
                             std::uint8_t capabilities) {
    appId_ = appId;
    emulatedType_ = emulatedType;
    capabilities_ = capabilities;
    rtspCseq_ = 1;
    dispatch(moonlight::moon_event::StartRequested{});
}

void MoonlightSession::stop() { dispatch(moonlight::moon_event::StopRequested{}); }

void MoonlightSession::dispatch(const moonlight::SessionEvent& event) {
    run(moonlight::reduce(machine_, event));
}

void MoonlightSession::run(const moonlight::Reduction& reduction) {
    if (reduction.next) {
        machine_ = *reduction.next;
        switch (machine_.phase) {
        case moonlight::SessionPhase::Idle:
            setLinkState(MoonlightLinkState::Idle);
            break;
        case moonlight::SessionPhase::Streaming:
            setLinkState(MoonlightLinkState::Live);
            break;
        case moonlight::SessionPhase::Failed:
            setLinkState(MoonlightLinkState::Failed);
            break;
        default:
            setLinkState(MoonlightLinkState::Linking);
            break;
        }
    }
    for (const auto effect : reduction.effects) { runEffect(effect); }
}

void MoonlightSession::runEffect(moonlight::SessionEffect effect) {
    using moonlight::SessionEffect;
    switch (effect) {
    case SessionEffect::FetchServerInfo:
        fetchServerInfo();
        break;
    case SessionEffect::SendLaunch:
        sendLaunch();
        break;
    case SessionEffect::OpenRtsp:
        openRtsp();
        break;
    case SessionEffect::SendRtspOptions:
        sendRtspStep(moonlight::RtspStep::Options);
        break;
    case SessionEffect::SendRtspDescribe:
        sendRtspStep(moonlight::RtspStep::Describe);
        break;
    case SessionEffect::SendRtspSetupAudio:
        sendRtspStep(moonlight::RtspStep::SetupAudio);
        break;
    case SessionEffect::SendRtspSetupVideo:
        sendRtspStep(moonlight::RtspStep::SetupVideo);
        break;
    case SessionEffect::SendRtspSetupControl:
        sendRtspStep(moonlight::RtspStep::SetupControl);
        break;
    case SessionEffect::SendRtspAnnounce:
        sendRtspStep(moonlight::RtspStep::Announce);
        break;
    case SessionEffect::SendRtspPlay:
        sendRtspStep(moonlight::RtspStep::Play);
        break;
    case SessionEffect::ConnectControl:
        connectControl();
        break;
    case SessionEffect::StartStreaming:
        startStreaming();
        break;
    case SessionEffect::SendTermination:
        if (control_) { control_->stop(true); }
        break;
    case SessionEffect::Teardown:
        teardown();
        break;
    case SessionEffect::NotifyFailure:
        if (machine_.failure) { emit failed(QStringLiteral("moonlight")); }
        break;
    }
}

void MoonlightSession::setLinkState(MoonlightLinkState state) {
    if (linkState_ == state) { return; }
    linkState_ = state;
    emit linkStateChanged();
}

void MoonlightSession::fetchServerInfo() {
    http_->getPlain(host_.address, host_.httpPort, QStringLiteral("/serverinfo"), QUrlQuery(),
                    [this](int status, const QByteArray& body) {
                        if (status != 200) {
                            dispatch(moonlight::moon_event::ServerInfoFailed{});
                            return;
                        }
                        const auto info = moonxml::parseServerInfo(body.toStdString());
                        if (!info) {
                            dispatch(moonlight::moon_event::ServerInfoFailed{});
                            return;
                        }
                        moonlight::moon_event::ServerInfoOk ev;
                        ev.paired = info->pairStatus == 1;
                        ev.currentGame = info->currentGame;
                        dispatch(ev);
                    });
}

void MoonlightSession::sendLaunch() {
    // A fresh control-stream key per launch (Wolf keys the control AES-GCM on
    // this rikey; rikeyid feeds nothing this client must vary, so it stays 0).
    if (!mooncrypto::randomBytes(rikey_.data(), rikey_.size())) {
        dispatch(moonlight::moon_event::LaunchFailed{});
        return;
    }
    rikeyId_ = 0;

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("appid"), appId_);
    query.addQueryItem(QStringLiteral("mode"), QString::fromLatin1(kLaunchMode));
    query.addQueryItem(QStringLiteral("additionalStates"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("sops"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("rikey"),
                       QString::fromStdString(util::toHex(rikey_.data(), rikey_.size())));
    query.addQueryItem(QStringLiteral("rikeyid"), QString::number(rikeyId_));
    query.addQueryItem(QStringLiteral("localAudioPlayMode"), QStringLiteral("0"));
    query.addQueryItem(QStringLiteral("surroundAudioInfo"), QStringLiteral("196610"));
    const QString path = machine_.resuming ? QStringLiteral("/resume") : QStringLiteral("/launch");
    http_->getTls(host_.address, host_.httpsPort, path, query, host_.serverCertPem,
                  [this](int status, const QByteArray& body) {
                      if (status != 200) {
                          dispatch(moonlight::moon_event::LaunchFailed{});
                          return;
                      }
                      const auto launch = moonxml::parseLaunch(body.toStdString());
                      if (!launch || !launch->launched) {
                          dispatch(moonlight::moon_event::LaunchFailed{});
                          return;
                      }
                      rtspTarget_ = QStringLiteral("rtsp://%1:%2")
                                        .arg(QString::fromStdString(launch->rtspHost))
                                        .arg(launch->rtspPort);
                      rtspPort_ = launch->rtspPort;
                      // The host's launch reply may hand out a fake session IP;
                      // dial the host we already know, not the parroted string.
                      rtspHostAddress_ = host_.address;
                      dispatch(moonlight::moon_event::LaunchOk{});
                  });
}

void MoonlightSession::openRtsp() { rtsp_->open(rtspHostAddress_, rtspPort_); }

void MoonlightSession::sendRtspStep(moonlight::RtspStep step) {
    QString request;
    switch (step) {
    case moonlight::RtspStep::Options:
        request =
            QString::fromStdString(moonrtsp::formatOptions(rtspCseq_++, rtspTarget_.toStdString()));
        break;
    case moonlight::RtspStep::Describe:
        request = QString::fromStdString(
            moonrtsp::formatDescribe(rtspCseq_++, rtspTarget_.toStdString()));
        break;
    case moonlight::RtspStep::SetupAudio:
    case moonlight::RtspStep::SetupVideo:
    case moonlight::RtspStep::SetupControl:
        request = QString::fromStdString(moonrtsp::formatSetup(
            rtspCseq_++, stepStreamId(step).toStdString(), rtspSessionId_.toStdString()));
        break;
    case moonlight::RtspStep::Announce: {
        const auto payload = moonrtsp::buildAnnouncePayload(moonrtsp::StreamConfig{});
        request = QString::fromStdString(
            moonrtsp::formatAnnounce(rtspCseq_++, rtspSessionId_.toStdString(), payload));
        break;
    }
    case moonlight::RtspStep::Play:
        request = QString::fromStdString(moonrtsp::formatPlay(
            rtspCseq_++, rtspTarget_.toStdString(), rtspSessionId_.toStdString()));
        break;
    }

    rtsp_->request(request, [this, step](const std::optional<moonrtsp::Response>& response) {
        if (!response || !response->ok()) {
            dispatch(moonlight::moon_event::RtspFailed{});
            return;
        }
        // Absorb the per-step transport data the later phases need.
        if (const auto id = moonrtsp::sessionId(*response); id && rtspSessionId_.isEmpty()) {
            rtspSessionId_ = QString::fromStdString(*id);
        }
        if (step == moonlight::RtspStep::SetupAudio) {
            audioPort_ = moonrtsp::transportPort(*response).value_or(0);
            audioPingPayload_ =
                QByteArray::fromStdString(moonrtsp::pingPayload(*response).value_or(""));
        } else if (step == moonlight::RtspStep::SetupVideo) {
            videoPort_ = moonrtsp::transportPort(*response).value_or(0);
            videoPingPayload_ =
                QByteArray::fromStdString(moonrtsp::pingPayload(*response).value_or(""));
        } else if (step == moonlight::RtspStep::SetupControl) {
            controlPort_ = moonrtsp::transportPort(*response).value_or(0);
            controlConnectData_ = moonrtsp::connectData(*response).value_or(0);
        }
        dispatch(moonlight::moon_event::RtspStepOk{});
    });
}

void MoonlightSession::connectControl() {
    if (controlPort_ <= 0) {
        dispatch(moonlight::moon_event::ControlLost{});
        return;
    }
    if (!control_->start(host_.address.toStdString(), static_cast<std::uint16_t>(controlPort_),
                         controlConnectData_, rikey_)) {
        dispatch(moonlight::moon_event::ControlLost{});
    }
    // Success/failure of the ENet connect arrives via the link handler.
}

void MoonlightSession::startStreaming() {
    // Announce the pad so the host plugs a virtual controller, then open the
    // media ports with hole-punch pings so the host sees them (their inbound
    // payloads are discarded; we never decode a frame).
    control_->sendControllerArrival(0, emulatedType_, capabilities_, moonproto::kStandardButtons);

    const auto makeSocket = [this](int port) -> QUdpSocket* {
        if (port <= 0) { return nullptr; }
        auto* udp = new QUdpSocket(this);
        // Whatever the host streams back is drained and dropped, so the OS
        // buffer never fills and no frame is ever decoded.
        QObject::connect(udp, &QUdpSocket::readyRead, udp, [udp] {
            while (udp->hasPendingDatagrams()) {
                udp->readDatagram(nullptr, 0); // discard without copying
            }
        });
        return udp;
    };
    rtpVideoSocket_ = makeSocket(videoPort_);
    rtpAudioSocket_ = makeSocket(audioPort_);
    rtpPingSequence_ = 0;
    sendRtpPings();
    rtpPingTimer_->start();
}

void MoonlightSession::sendRtpPings() {
    std::uint8_t ping[moonwire::kRtpPingSize];
    const auto punch = [this, &ping](QUdpSocket* udp, int port, const QByteArray& payload) {
        if (udp == nullptr || port <= 0) { return; }
        // The SETUP-provided payload identifies our session to the host; the
        // legacy 4-byte "PING" is the fallback when none was supplied.
        const std::size_t len = moonwire::encodeRtpPing(
            ping, payload.constData(), static_cast<std::size_t>(payload.size()), rtpPingSequence_);
        udp->writeDatagram(reinterpret_cast<const char*>(ping), static_cast<qint64>(len),
                           QHostAddress(host_.address), static_cast<quint16>(port));
    };
    punch(rtpVideoSocket_, videoPort_, videoPingPayload_);
    punch(rtpAudioSocket_, audioPort_, audioPingPayload_);
    ++rtpPingSequence_;
}

void MoonlightSession::teardown() {
    if (control_) { control_->stop(false); }
    if (rtsp_) { rtsp_->close(); }
    if (rtpPingTimer_ != nullptr) { rtpPingTimer_->stop(); }
    delete rtpVideoSocket_;
    rtpVideoSocket_ = nullptr;
    delete rtpAudioSocket_;
    rtpAudioSocket_ = nullptr;
    audioPingPayload_.clear();
    videoPingPayload_.clear();
    rtpPingSequence_ = 0;
    motionRequested_ = false;
}

void MoonlightSession::sendControllerState(std::uint16_t internalButtons, std::uint8_t lt,
                                           std::uint8_t rt, std::int16_t lx, std::int16_t ly,
                                           std::int16_t rx, std::int16_t ry) {
    // Active mask has bit 0 set for our single controller.
    control_->sendControllerMulti(0, 0x0001, moonmap::toMoonlightButtons(internalButtons), lt, rt,
                                  lx, ly, rx, ry);
}

void MoonlightSession::sendMotion(std::uint8_t motionType, float x, float y, float z) {
    if (!motionRequested_) { return; }
    control_->sendControllerMotion(0, motionType, x, y, z);
}

void MoonlightSession::onHostEvent(const moonwire::HostEvent& event) {
    // Copy the POD event onto the Qt main thread; the handlers touch UI-thread
    // plumbing (the SDL output queue) via the manager.
    QMetaObject::invokeMethod(
        this,
        [this, event] {
            switch (event.type) {
            case moonwire::HostEventType::Rumble:
            case moonwire::HostEventType::RumbleTriggers:
                if (rumbleHandler_) { rumbleHandler_(event.rumbleLow, event.rumbleHigh); }
                break;
            case moonwire::HostEventType::RgbLed:
                if (ledHandler_) { ledHandler_(event.red, event.green, event.blue); }
                break;
            case moonwire::HostEventType::MotionRequest:
                // rate 0 stops motion; any non-zero rate starts it.
                motionRequested_ = event.motionRateHz != 0;
                break;
            case moonwire::HostEventType::Termination:
                dispatch(moonlight::moon_event::HostTerminated{});
                break;
            case moonwire::HostEventType::Unknown:
                break;
            }
        },
        Qt::QueuedConnection);
}

} // namespace dish::source::moon
