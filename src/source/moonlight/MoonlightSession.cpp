// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/moonlight/MoonlightSession.h"

#include "Util/Hex.h"
#include "core/moonlight/MoonlightButtonMap.h"
#include "core/moonlight/MoonlightPairingCrypto.h"
#include "core/moonlight/MoonlightProtocol.h"
#include "core/moonlight/MoonlightXml.h"
#include "source/moonlight/MoonlightLog.h"

#include <QMetaType>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUrlQuery>

#include <chrono>
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
    case moonlight::SessionFailure::TrustLost:
        return QStringLiteral("trustLost");
    case moonlight::SessionFailure::HostReplaced:
        return QStringLiteral("hostReplaced");
    case moonlight::SessionFailure::LaunchRejected:
        return QStringLiteral("launchRejected");
    case moonlight::SessionFailure::AppAlreadyRunning:
        return QStringLiteral("appAlreadyRunning");
    case moonlight::SessionFailure::ResumeFailed:
        return QStringLiteral("resumeFailed");
    case moonlight::SessionFailure::RtspRejected:
        return QStringLiteral("rtspRejected");
    case moonlight::SessionFailure::ControlLost:
        return QStringLiteral("controlLost");
    case moonlight::SessionFailure::Dropped:
        return QStringLiteral("dropped");
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
    // Both handlers run on the control stream's service thread and only emit;
    // the queued connections below are what hop onto the Qt loop.
    qRegisterMetaType<moonwire::HostEvent>();
    control_->setLinkHandler([this](bool connected) { emit controlLinkChanged(connected); });
    control_->setEventHandler(
        [this](const moonwire::HostEvent& event) { emit hostEventReceived(event); });
    QObject::connect(
        this, &MoonlightSession::controlLinkChanged, this,
        [this](bool connected) {
            qCInfo(lcMoon) << "control link to" << host_.address << (connected ? "up" : "down");
            dispatch(connected ? moonlight::SessionEvent{moonlight::moon_event::ControlConnected{}}
                               : moonlight::SessionEvent{moonlight::moon_event::ControlLost{}});
        },
        Qt::QueuedConnection);
    QObject::connect(this, &MoonlightSession::hostEventReceived, this,
                     &MoonlightSession::onHostEvent, Qt::QueuedConnection);

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

void MoonlightSession::start(const QString& appId, const QString& appName) {
    appId_ = appId;
    appName_ = appName;
    rtspCseq_ = 1;
    rikeyReady_ = false;
    launched_ = false;
    wentLive_ = false;
    everStarted_ = true;
    refusalMessage_.clear();
    stream_ = moonrtsp::StreamConfig{};
    qCInfo(lcMoon) << "session start on" << host_.address << "app" << appId_ << "pads"
                   << slots_.size();
    dispatch(moonlight::moon_event::StartRequested{});
}

void MoonlightSession::stop(bool handBackApp) {
    qCInfo(lcMoon) << "session stop requested on" << host_.address << "hand back" << handBackApp;
    handBackOnTeardown_ = handBackApp;
    dispatch(moonlight::moon_event::StopRequested{});
    handBackOnTeardown_ = false;
}

std::optional<std::uint8_t>
MoonlightSession::attachController(const QString& slotId, int storedType,
                                   const moonlight::SourceCapabilities& source) {
    const auto number = slots_.assign(slotId.toStdString());
    if (!number) { return std::nullopt; }
    PadDeclaration pad;
    pad.number = *number;
    pad.type = moonlight::resolveControllerType(storedType, source.motion);
    pad.capabilities = moonlight::declaredCapabilities(pad.type, source);
    pad.buttons = moonlight::declaredButtons(pad.capabilities);
    pads_.insert(slotId, pad);
    activeMask_.store(slots_.activeMask(), std::memory_order_relaxed);
    qCInfo(lcMoon) << "pad" << slotId << "takes controller" << pad.number << "on" << host_.address
                   << "type" << pad.type << "caps" << pad.capabilities << "mask"
                   << activeMask_.load(std::memory_order_relaxed);
    announcePad(slotId);
    return number;
}

std::size_t MoonlightSession::detachController(const QString& slotId) {
    const auto released = slots_.release(slotId.toStdString());
    pads_.remove(slotId);
    const std::uint16_t mask = slots_.activeMask();
    activeMask_.store(mask, std::memory_order_relaxed);
    // The unplug IS the packet: the controller is still named, its bit is gone.
    if (released && control_ && control_->isConnected()) {
        control_->sendControllerMulti(*released, mask, 0, 0, 0, 0, 0, 0, 0);
    }
    qCInfo(lcMoon) << "pad" << slotId << "left" << host_.address << "mask" << mask << "remaining"
                   << slots_.size();
    return slots_.size();
}

std::optional<std::uint8_t> MoonlightSession::controllerNumber(const QString& slotId) const {
    return slots_.numberFor(slotId.toStdString());
}

QString MoonlightSession::slotForController(std::uint8_t number) const {
    const auto slot = slots_.slotFor(number);
    return slot ? QString::fromStdString(*slot) : QString();
}

void MoonlightSession::announcePad(const QString& slotId) {
    if (!control_ || !control_->isConnected()) { return; }
    const auto it = pads_.constFind(slotId);
    if (it == pads_.constEnd()) { return; }
    control_->sendControllerArrival(it->number, it->type, it->capabilities, it->buttons);
    // And its first state, zeroed, carrying the active mask: a bound pad nobody
    // has touched yet is then a pad the game can see rather than a name the
    // host is still waiting to hear from.
    control_->sendControllerMulti(it->number, activeMask_.load(std::memory_order_relaxed), 0, 0, 0,
                                  0, 0, 0, 0);
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

MoonlightHttp::BodyCb MoonlightSession::guarded(MoonlightHttp::BodyCb cb) {
    QPointer<MoonlightSession> self(this);
    return [self, cb = std::move(cb)](int status, const QByteArray& body) {
        if (self.isNull()) { return; }
        cb(status, body);
    };
}

void MoonlightSession::fetchServerInfo() {
    // TWO QUESTIONS, TWO CALLS. The plaintext port answers whether anything is
    // there and whether it is the host the record anchors, and nothing else:
    // Sunshine computes PairStatus on the mutual-TLS route alone and hands every
    // plaintext caller a 0, its own paired devices included, so a session gated
    // on this flag never started against a live host, which no amount of
    // pairing made visible. The trust question is asked by askTrust().
    http_->getPlain(
        host_.address, host_.httpPort, QStringLiteral("/serverinfo"), QUrlQuery(),
        guarded([this](int status, const QByteArray& body) {
            if (status != 200) {
                qCWarning(lcMoon) << "serverinfo on" << host_.address << "answered HTTP" << status;
                dispatch(moonlight::moon_event::ServerInfoFailed{});
                return;
            }
            const std::string xml = body.toStdString();
            const auto info = moonxml::parseServerInfo(xml);
            if (!info) {
                const auto refusal = moonxml::parseStatus(xml);
                qCWarning(lcMoon) << "serverinfo on" << host_.address << "unusable: host"
                                  << hostSays(refusal);
                dispatch(moonlight::moon_event::ServerInfoFailed{});
                return;
            }
            const QString reported = QString::fromStdString(info->uuid);
            if (!reported.isEmpty() && !host_.uuid.isEmpty() &&
                !host_.uuid.startsWith(QLatin1String("addr:")) && reported != host_.uuid) {
                // Another machine behind the address: the stored certificate
                // anchors nothing, and a TLS call would only fail less usefully.
                qCWarning(lcMoon) << host_.address << "answers as" << reported << "remembered as"
                                  << host_.uuid << ": host replaced";
                moonlight::moon_event::ServerInfoOk ev;
                ev.remembered = host_.paired();
                ev.identityChanged = true;
                dispatch(ev);
                return;
            }
            if (!host_.paired()) {
                // Never paired: there is no certificate to present, so the
                // trust question has its answer already.
                qCInfo(lcMoon) << "serverinfo on" << host_.address << "answered; not paired";
                moonlight::moon_event::ServerInfoOk ev;
                ev.paired = false;
                ev.remembered = false;
                dispatch(ev);
                return;
            }
            askTrust();
        }));
}

void MoonlightSession::askTrust() {
    http_->getTls(host_.address, host_.httpsPort, QStringLiteral("/serverinfo"), QUrlQuery(),
                  host_.serverCertPem, guarded([this](int status, const QByteArray& body) {
                      moonlight::moon_event::ServerInfoOk ev;
                      ev.remembered = true;
                      const auto info = status == 200 ? moonxml::parseServerInfo(body.toStdString())
                                                      : std::optional<moonxml::ServerInfo>{};
                      if (!info) {
                          // The plaintext port answered a moment ago, so this is
                          // the host declining the certificate, not a cable.
                          qCWarning(lcMoon) << host_.address << "refused mutual TLS (HTTP" << status
                                            << "): trust lost";
                          ev.paired = false;
                          dispatch(ev);
                          return;
                      }
                      ev.paired = info->pairStatus == 1;
                      ev.currentGame = info->currentGame;
                      qCInfo(lcMoon)
                          << "serverinfo on" << host_.address << "over TLS: paired" << ev.paired
                          << "currentgame" << info->currentGame << "mode" << stream_.width << "x"
                          << stream_.height << "@" << stream_.fps;
                      dispatch(ev);
                  }));
}

void MoonlightSession::sendLaunch() {
    // One control-stream key per attempt (Wolf keys the control AES-GCM on
    // this rikey). The rikeyid is minted with it: nothing this client sends is
    // keyed on it today, but a host is free to be, and the other two clients
    // mint one, so a constant here would be an assumption about a host we do
    // not control. A launch that promotes to /resume keeps both.
    if (!rikeyReady_) {
        std::array<std::uint8_t, 4> id{};
        if (!mooncrypto::randomBytes(rikey_.data(), rikey_.size()) ||
            !mooncrypto::randomBytes(id.data(), id.size())) {
            qCWarning(lcMoon) << "launch on" << host_.address
                              << "aborted: no entropy for the rikey";
            dispatch(moonlight::moon_event::LaunchFailed{});
            return;
        }
        rikeyId_ = static_cast<std::uint32_t>(id[0]) | (static_cast<std::uint32_t>(id[1]) << 8) |
                   (static_cast<std::uint32_t>(id[2]) << 16) |
                   (static_cast<std::uint32_t>(id[3]) << 24);
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
                  guarded([this, path](int status, const QByteArray& body) {
                      const std::string xml = body.toStdString();
                      const auto refusal = moonxml::parseStatus(xml);
                      const auto launch = moonxml::parseLaunch(xml);
                      qCInfo(lcMoon)
                          << path << "on" << host_.address << "HTTP" << status << "host"
                          << hostSays(refusal) << "rtsp port" << (launch ? launch->rtspPort : 0);
                      if (status == 200 && launch && launch->launched) {
                          if (machine_.phase != moonlight::SessionPhase::Launching) {
                              // THE LAUNCH WE WALKED AWAY FROM CAME GOOD ANYWAY. The
                              // last pad left while the reply was in flight, so the
                              // reducer will not act on it and nothing is riding the
                              // session. The host is holding an app on our behalf all
                              // the same: hand it straight back, or it sits there
                              // refusing every later /launch.
                              qCInfo(lcMoon) << path << "on" << host_.address
                                             << "answered after the session closed; handing back";
                              cancelStrandedApp();
                              return;
                          }
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
                      if (refusal && !refusal->message.empty()) {
                          refusalMessage_ = QString::fromStdString(refusal->message);
                      } else if (refusal) {
                          refusalMessage_ = QString::number(refusal->code);
                      }
                      dispatch(moonlight::moon_event::LaunchFailed{});
                  }));
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
    // Announce EVERY attached pad so the host plugs one virtual controller per
    // binding. The media ports have been pinged since SETUP named them.
    wentLive_ = true;
    qCInfo(lcMoon) << "session live on" << host_.address << "announcing" << pads_.size() << "pads";
    for (auto it = pads_.constBegin(); it != pads_.constEnd(); ++it) {
        control_->sendControllerArrival(it->number, it->type, it->capabilities, it->buttons);
        control_->sendControllerMulti(it->number, activeMask_.load(std::memory_order_relaxed), 0, 0,
                                      0, 0, 0, 0, 0);
    }
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
    if (moonlight::shouldHandBackApp(launched_, wentLive_, handBackOnTeardown_)) {
        cancelStrandedApp();
    }
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
    // A new session is a new set of subscriptions: the host has forgotten the
    // old ones, so resuming them would stream to nobody.
    motionGate_.clearAll();
    rumbleMix_.clear();
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

void MoonlightSession::sendControllerState(std::uint8_t controllerNumber,
                                           std::uint16_t internalButtons, std::uint8_t lt,
                                           std::uint8_t rt, std::int16_t lx, std::int16_t ly,
                                           std::int16_t rx, std::int16_t ry) {
    control_->sendControllerMulti(controllerNumber, activeMask_.load(std::memory_order_relaxed),
                                  moonmap::toMoonlightButtons(internalButtons), lt, rt, lx, ly, rx,
                                  ry);
}

bool MoonlightSession::sendMotion(std::uint8_t controllerNumber, std::uint8_t motionType, float x,
                                  float y, float z) {
    // Steady clock, not wall clock: a system time change must not open the gate
    // for a second or wedge it shut for an hour.
    const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (!motionGate_.shouldSend(controllerNumber, motionType, nowUs)) { return false; }
    control_->sendControllerMotion(controllerNumber, motionType, x, y, z);
    return true;
}

void MoonlightSession::sendBattery(std::uint8_t controllerNumber, std::uint8_t state,
                                   std::uint8_t percentage) {
    control_->sendControllerBattery(controllerNumber, state, percentage);
}

void MoonlightSession::sendTouch(std::uint8_t controllerNumber,
                                 const moonlight::TouchEvent& event) {
    control_->sendControllerTouch(controllerNumber, event.eventType, event.pointerId, event.x,
                                  event.y, event.pressure);
}

void MoonlightSession::onHostEvent(const moonwire::HostEvent& event) {
    // On the Qt main thread: the handlers touch UI-thread plumbing (the SDL
    // output queue) via the manager.
    switch (event.type) {
    case moonwire::HostEventType::Rumble:
    case moonwire::HostEventType::RumbleTriggers: {
        // The two streams both land on the pad's two motors, because no pad this
        // client can Direct-claim has impulse-trigger motors: xpad binds an Xbox
        // pad as evdev-only and publishes no hidraw node for it. Mixing per
        // motor by maximum is what stops a trigger update cancelling a body
        // rumble that is still running. See core/moonlight/MoonlightTriggerRumble.h.
        auto& mix = rumbleMix_[event.controllerNumber];
        mix = event.type == moonwire::HostEventType::RumbleTriggers
                  ? moonlight::withTriggerRumble(mix, event.rumbleLow, event.rumbleHigh)
                  : moonlight::withBodyRumble(mix, event.rumbleLow, event.rumbleHigh);
        const auto mixed = moonlight::mixRumble(mix);
        if (rumbleHandler_) {
            rumbleHandler_(static_cast<std::uint8_t>(event.controllerNumber), mixed.strong,
                           mixed.weak);
        }
        break;
    }
    case moonwire::HostEventType::RgbLed:
        if (ledHandler_) {
            ledHandler_(static_cast<std::uint8_t>(event.controllerNumber), event.red, event.green,
                        event.blue);
        }
        break;
    case moonwire::HostEventType::MotionRequest:
        // Per (pad, type) and at the requested rate; 0 stops that one stream.
        motionGate_.onMotionRequest(event.controllerNumber, event.motionRateHz, event.motionType);
        break;
    case moonwire::HostEventType::Termination:
        qCInfo(lcMoon) << host_.address << "sent TERMINATION";
        dispatch(moonlight::moon_event::HostTerminated{});
        break;
    case moonwire::HostEventType::Unknown:
        break;
    }
}

} // namespace dish::source::moon
