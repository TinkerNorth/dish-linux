// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightSession.h"

#include "Util/Hex.h"
#include "core/moonlight/MoonlightButtonMap.h"
#include "core/moonlight/MoonlightPairingCrypto.h"
#include "core/moonlight/MoonlightProtocol.h"
#include "core/moonlight/MoonlightXml.h"
#include "source/moonlight/MoonlightLog.h"

#include <QMetaObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrlQuery>

#include <cstring>
#include <optional>

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

// Cadence of the RTP hole-punch pings, matching what real clients send. One
// datagram per port would be fragile: lose it and the host never learns our
// media address.
constexpr int kRtpPingIntervalMs = 500;

// What the host said in the body of its reply, for one log line.
QString hostSays(const std::optional<moonxml::Status>& status) {
    if (!status) { return QStringLiteral("(no root element)"); }
    return QStringLiteral("%1 %2").arg(status->code).arg(QString::fromStdString(status->message));
}

QString failureToken(moonlight::SessionFailure failure) {
    switch (failure) {
    case moonlight::SessionFailure::Unreachable:
        return QStringLiteral("unreachable");
    case moonlight::SessionFailure::NotPaired:
        return QStringLiteral("notPaired");
    case moonlight::SessionFailure::LaunchRejected:
        return QStringLiteral("launchRejected");
    case moonlight::SessionFailure::AppAlreadyRunning:
        return QStringLiteral("appAlreadyRunning");
    case moonlight::SessionFailure::RtspRejected:
        return QStringLiteral("rtspRejected");
    case moonlight::SessionFailure::ControlLost:
        return QStringLiteral("controlLost");
    case moonlight::SessionFailure::HostEnded:
    default:
        return QStringLiteral("hostEnded");
    }
}

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
                qCInfo(lcMoon) << "control link to" << host_.address << (connected ? "up" : "down");
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

MoonlightSession::~MoonlightSession() {
    // No /cancel from here: the shared HTTP gateway is a sibling child of the
    // manager and may already be gone. The manager stops every session first,
    // which is where a stranded app is handed back.
    launched_ = false;
    teardown();
}

void MoonlightSession::start(const QString& appId, std::uint8_t emulatedType,
                             std::uint8_t capabilities) {
    appId_ = appId;
    emulatedType_ = emulatedType;
    capabilities_ = capabilities;
    rtspCseq_ = 1;
    rikeyReady_ = false;
    launched_ = false;
    wentLive_ = false;
    stream_ = moonrtsp::StreamConfig{};
    qCInfo(lcMoon) << "session start on" << host_.address << "app" << appId_ << "type"
                   << emulatedType_ << "caps" << capabilities_;
    dispatch(moonlight::moon_event::StartRequested{});
}

void MoonlightSession::stop() {
    qCInfo(lcMoon) << "session stop requested on" << host_.address;
    dispatch(moonlight::moon_event::StopRequested{});
}

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
        if (machine_.failure) {
            const QString token = failureToken(*machine_.failure);
            qCWarning(lcMoon) << "session on" << host_.address << "gave up:" << token;
            emit failed(token);
        }
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
                            qCWarning(lcMoon)
                                << "serverinfo on" << host_.address << "answered HTTP" << status;
                            dispatch(moonlight::moon_event::ServerInfoFailed{});
                            return;
                        }
                        const std::string xml = body.toStdString();
                        const auto info = moonxml::parseServerInfo(xml);
                        if (!info) {
                            const auto refusal = moonxml::parseStatus(xml);
                            qCWarning(lcMoon) << "serverinfo on" << host_.address
                                              << "unusable: host" << hostSays(refusal);
                            dispatch(moonlight::moon_event::ServerInfoFailed{});
                            return;
                        }
                        // Ask for the host's own display rather than a small mode: an
                        // Apollo/Vibepollo virtual display follows what the client asks
                        // for, and a small request resizes the user's desktop under them.
                        if (const auto mode = moonxml::preferredDisplayMode(info->displayModes)) {
                            stream_.width = mode->width;
                            stream_.height = mode->height;
                            if (mode->refreshRate > 0) { stream_.fps = mode->refreshRate; }
                        }
                        qCInfo(lcMoon) << "serverinfo on" << host_.address << "paired"
                                       << (info->pairStatus == 1) << "currentgame"
                                       << info->currentGame << "mode" << stream_.width << "x"
                                       << stream_.height << "@" << stream_.fps;
                        moonlight::moon_event::ServerInfoOk ev;
                        ev.paired = info->pairStatus == 1;
                        ev.currentGame = info->currentGame;
                        dispatch(ev);
                    });
}

void MoonlightSession::sendLaunch() {
    // One control-stream key per attempt (Wolf keys the control AES-GCM on
    // this rikey; rikeyid feeds nothing this client must vary, so it stays 0).
    // A launch that promotes to /resume keeps the key it already announced.
    if (!rikeyReady_) {
        if (!mooncrypto::randomBytes(rikey_.data(), rikey_.size())) {
            qCWarning(lcMoon) << "launch on" << host_.address
                              << "aborted: no entropy for the rikey";
            dispatch(moonlight::moon_event::LaunchFailed{});
            return;
        }
        rikeyId_ = 0;
        rikeyReady_ = true;
    }

    const bool resuming = machine_.resuming;
    QUrlQuery query;
    if (!resuming) {
        query.addQueryItem(QStringLiteral("appid"), appId_);
        query.addQueryItem(
            QStringLiteral("mode"),
            QStringLiteral("%1x%2x%3").arg(stream_.width).arg(stream_.height).arg(stream_.fps));
        query.addQueryItem(QStringLiteral("additionalStates"), QStringLiteral("1"));
        // sops=0: never let the host change the user's display settings.
        query.addQueryItem(QStringLiteral("sops"), QStringLiteral("0"));
    }
    query.addQueryItem(QStringLiteral("rikey"),
                       QString::fromStdString(util::toHex(rikey_.data(), rikey_.size())));
    query.addQueryItem(QStringLiteral("rikeyid"), QString::number(rikeyId_));
    // 1, not 0. The user of a dish is sitting AT the host with the pad in their
    // hands, so asking the host not to play audio locally would silence their
    // own speakers for the length of the session.
    query.addQueryItem(QStringLiteral("localAudioPlayMode"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("surroundAudioInfo"), QStringLiteral("196610"));
    const QString path = resuming ? QStringLiteral("/resume") : QStringLiteral("/launch");
    http_->getTls(host_.address, host_.httpsPort, path, query, host_.serverCertPem,
                  [this, path](int status, const QByteArray& body) {
                      const std::string xml = body.toStdString();
                      const auto refusal = moonxml::parseStatus(xml);
                      const auto launch = moonxml::parseLaunch(xml);
                      qCInfo(lcMoon)
                          << path << "on" << host_.address << "HTTP" << status << "host"
                          << hostSays(refusal) << "rtsp port" << (launch ? launch->rtspPort : 0);
                      if (status == 200 && launch && launch->launched) {
                          rtspTarget_ = QStringLiteral("rtsp://%1:%2")
                                            .arg(QString::fromStdString(launch->rtspHost))
                                            .arg(launch->rtspPort);
                          rtspPort_ = launch->rtspPort;
                          // The host's launch reply may hand out a fake session IP;
                          // dial the host we already know, not the parroted string.
                          rtspHostAddress_ = host_.address;
                          launched_ = true;
                          dispatch(moonlight::moon_event::LaunchOk{});
                          return;
                      }
                      // A HOST SAYS NO IN THE BODY, NOT IN THE STATUS LINE: a second
                      // /launch is answered HTTP 200 carrying status_code="400" and "An
                      // app is already running on this host". Reading only the HTTP
                      // status turns that into a missing sessionUrl0 further down and
                      // names the wrong thing.
                      if (refusal && refusal->appAlreadyRunning()) {
                          qCInfo(lcMoon) << host_.address << "already has an app running; resume"
                                         << refusal->resume;
                          dispatch(moonlight::moon_event::LaunchBusy{refusal->resume});
                          return;
                      }
                      qCWarning(lcMoon) << path << "refused by" << host_.address << ":"
                                        << QString::fromUtf8(body.left(512));
                      dispatch(moonlight::moon_event::LaunchFailed{});
                  });
}

void MoonlightSession::openRtsp() {
    qCInfo(lcMoon) << "rtsp handshake to" << rtspHostAddress_ << rtspPort_ << "target"
                   << rtspTarget_;
    rtsp_->open(rtspHostAddress_, rtspPort_);
}

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
        const auto payload = moonrtsp::buildAnnouncePayload(stream_);
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
            qCInfo(lcMoon) << "setup audio port" << audioPort_ << "ping payload"
                           << audioPingPayload_.size() << "bytes";
            ensureRtpPings();
        } else if (step == moonlight::RtspStep::SetupVideo) {
            videoPort_ = moonrtsp::transportPort(*response).value_or(0);
            videoPingPayload_ =
                QByteArray::fromStdString(moonrtsp::pingPayload(*response).value_or(""));
            qCInfo(lcMoon) << "setup video port" << videoPort_ << "ping payload"
                           << videoPingPayload_.size() << "bytes";
            ensureRtpPings();
        } else if (step == moonlight::RtspStep::SetupControl) {
            controlPort_ = moonrtsp::transportPort(*response).value_or(0);
            controlConnectData_ = moonrtsp::connectData(*response).value_or(0);
            qCInfo(lcMoon) << "setup control port" << controlPort_ << "connect data"
                           << controlConnectData_;
        }
        dispatch(moonlight::moon_event::RtspStepOk{});
    });
}

void MoonlightSession::connectControl() {
    if (controlPort_ <= 0) {
        qCWarning(lcMoon) << "control setup named no port on" << host_.address;
        dispatch(moonlight::moon_event::ControlLost{});
        return;
    }
    if (!control_->start(host_.address.toStdString(), static_cast<std::uint16_t>(controlPort_),
                         controlConnectData_, rikey_)) {
        qCWarning(lcMoon) << "control stream would not start against" << host_.address
                          << controlPort_;
        dispatch(moonlight::moon_event::ControlLost{});
    }
    // Success/failure of the ENet connect arrives via the link handler.
}

void MoonlightSession::startStreaming() {
    // Announce the pad so the host plugs a virtual controller. The media ports
    // have been pinged since SETUP named them.
    wentLive_ = true;
    qCInfo(lcMoon) << "session live on" << host_.address << "announcing pad type" << emulatedType_;
    control_->sendControllerArrival(0, emulatedType_, capabilities_, moonproto::kStandardButtons);
    ensureRtpPings();
}

void MoonlightSession::ensureRtpPings() {
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
    if (rtpVideoSocket_ == nullptr) { rtpVideoSocket_ = makeSocket(videoPort_); }
    if (rtpAudioSocket_ == nullptr) { rtpAudioSocket_ = makeSocket(audioPort_); }
    if (rtpVideoSocket_ == nullptr && rtpAudioSocket_ == nullptr) { return; }
    sendRtpPings();
    if (!rtpPingTimer_->isActive()) { rtpPingTimer_->start(); }
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
    // A launch that never reached Streaming left the host holding an app on our
    // behalf. Hand it back, or every later attempt is refused by our own
    // leftovers. A link that drops after going live is left alone: the host
    // will let us resume it, and closing somebody's game out from under them is
    // worse than the tidying is worth.
    if (launched_ && !wentLive_) { cancelStrandedApp(); }
    launched_ = false;
    wentLive_ = false;
    if (control_) { control_->stop(false); }
    if (rtsp_) { rtsp_->close(); }
    if (rtpPingTimer_ != nullptr) { rtpPingTimer_->stop(); }
    delete rtpVideoSocket_;
    rtpVideoSocket_ = nullptr;
    delete rtpAudioSocket_;
    rtpAudioSocket_ = nullptr;
    audioPingPayload_.clear();
    videoPingPayload_.clear();
    audioPort_ = 0;
    videoPort_ = 0;
    controlPort_ = 0;
    controlConnectData_ = 0;
    rtspSessionId_.clear();
    rtpPingSequence_ = 0;
    motionRequested_ = false;
}

void MoonlightSession::cancelStrandedApp() {
    qCInfo(lcMoon) << "handing back the app" << host_.address << "started for us";
    http_->getTls(host_.address, host_.httpsPort, QStringLiteral("/cancel"), QUrlQuery(),
                  host_.serverCertPem,
                  [address = host_.address](int status, const QByteArray& body) {
                      const auto refusal = moonxml::parseStatus(body.toStdString());
                      qCInfo(lcMoon) << "cancel on" << address << "HTTP" << status << "host"
                                     << hostSays(refusal);
                  });
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
                qCInfo(lcMoon) << host_.address << "sent TERMINATION";
                dispatch(moonlight::moon_event::HostTerminated{});
                break;
            case moonwire::HostEventType::Unknown:
                break;
            }
        },
        Qt::QueuedConnection);
}

} // namespace dish::source::moon
