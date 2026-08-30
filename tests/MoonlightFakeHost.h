// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A GameStream host on loopback, complete enough to take a session all the way
// to Streaming: plaintext HTTP for /serverinfo and the pairing phases, mutual
// TLS for /applist, /launch, /resume and /cancel, an RTSP endpoint that answers
// the seven-step handshake, a real ENet listener for the control stream, and
// the two UDP media ports the hole-punch pings land on.
//
// WHY A WHOLE HOST AND NOT A STUB. Every question the behaviour spec asks about
// quitting is the same question: did the client tell the host to close the app,
// or did it not. That is a REQUEST, and a request is only observable at the far
// end of a socket. `shouldHandBackApp` can be asserted as a pure function and
// still be wired to nothing, or wired to a teardown that never runs; the live
// failures on the Android client were all of exactly that shape. So the tests
// that matter count what arrived here.
//
// It answers as Sunshine does, including the two shapes that have caught this
// client before: a refusal carried in the BODY of an HTTP 200, and a control
// stream that ends either by a TERMINATION or by simply going away. Which of
// those two happened is the difference between M20 and M21, and the only way to
// prove the client tells them apart is to make a host do each.
//
// Every listener binds 127.0.0.1:0, so a run costs no fixed port and reaches no
// machine but this one.

#pragma once

#include "Util/Hex.h"
#include "core/moonlight/MoonlightControlCipher.h"
#include "core/moonlight/MoonlightPairingCrypto.h"
#include "core/moonlight/MoonlightProtocol.h"
#include "core/moonlight/MoonlightWire.h"
#include "repository/SettingsKeys.h"

#include <enet/enet.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QObject>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QUrlQuery>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace dish::test {

// A self-signed RSA identity, minted once for the whole run. The prime search
// behind a 2048-bit key takes a variable and occasionally long time, and
// neither end of this handshake gains anything from a fresh one per case; an
// install has exactly one identity anyway.
inline const mooncrypto::ClientIdentity& fixtureHostIdentity() {
    static const mooncrypto::ClientIdentity id =
        mooncrypto::generateClientIdentity().value_or(mooncrypto::ClientIdentity{});
    return id;
}

inline const mooncrypto::ClientIdentity& fixtureClientIdentity() {
    static const mooncrypto::ClientIdentity id =
        mooncrypto::generateClientIdentity().value_or(mooncrypto::ClientIdentity{});
    return id;
}

// Writes the identity a MoonlightManager over `settings` will find, so it does
// not stop to mint one of its own before the first call it makes.
inline void seedClientIdentity(QSettings& settings) {
    const auto& id = fixtureClientIdentity();
    settings.setValue(QLatin1String(repository::keys::kMoonlightCertKey),
                      QString::fromStdString(id.certPem));
    settings.setValue(QLatin1String(repository::keys::kMoonlightKeyKey),
                      QString::fromStdString(id.privateKeyPem));
    settings.setValue(QLatin1String(repository::keys::kMoonlightUniqueIdKey),
                      QStringLiteral("7b5d0738cbb54d3e"));
    settings.sync();
}

// Catch2 owns no event loop, and everything below is asynchronous by nature.
// Spin the suite's QCoreApplication until the condition holds, with a ceiling
// so a stall fails the case instead of hanging the run.
inline bool spinFor(const std::function<bool()>& ready, int timeoutMs = 20000) {
    QElapsedTimer clock;
    clock.start();
    while (!ready() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return ready();
}

// Let pending work land when there is no signal to wait on, which is the shape
// every "and nothing else happened" assertion needs.
inline void settle(int ms = 300) {
    spinFor([] { return false; }, ms);
}

// One HTTP request, split the way the assertions read it.
struct FakeRequest {
    QString path;
    QUrlQuery query;
    bool tls = false;
};

class FakeMoonlightHost : public QObject {
  public:
    FakeMoonlightHost() {
        const auto& identity = fixtureHostIdentity();
        if (identity.certPem.empty()) { return; }
        certPem_ = QString::fromStdString(identity.certPem);
        keyPem_ = QString::fromStdString(identity.privateKeyPem);

        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        const auto certs = QSslCertificate::fromData(certPem_.toUtf8(), QSsl::Pem);
        if (certs.isEmpty()) { return; }
        ssl.setLocalCertificate(certs.first());
        ssl.setPrivateKey(QSslKey(keyPem_.toUtf8(), QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey));
        // The client pins the certificate itself and offers its own; asking for
        // one back would only add a way for the fixture to refuse a valid call.
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone);
        tls_.setSslConfiguration(ssl);

        QObject::connect(&plain_, &QTcpServer::newConnection, this,
                         [this] { serveHttp(plain_.nextPendingConnection(), /*tls=*/false); });
        // QSslServer queues a socket only once its handshake has finished, and
        // that is a different signal from the one a plain accept raises.
        QObject::connect(&tls_, &QTcpServer::pendingConnectionAvailable, this, [this] {
            while (auto* sock = tls_.nextPendingConnection()) { serveHttp(sock, /*tls=*/true); }
        });
        QObject::connect(&rtsp_, &QTcpServer::newConnection, this, [this] { serveRtsp(); });

        listening_ =
            plain_.listen(QHostAddress::LocalHost, 0) && tls_.listen(QHostAddress::LocalHost, 0) &&
            rtsp_.listen(QHostAddress::LocalHost, 0) && video_.bind(QHostAddress::LocalHost, 0) &&
            audio_.bind(QHostAddress::LocalHost, 0) && startControl();
        QObject::connect(&video_, &QUdpSocket::readyRead, this, [this] { drain(video_); });
        QObject::connect(&audio_, &QUdpSocket::readyRead, this, [this] { drain(audio_); });
    }

    ~FakeMoonlightHost() override { stopControl(); }

    FakeMoonlightHost(const FakeMoonlightHost&) = delete;
    FakeMoonlightHost& operator=(const FakeMoonlightHost&) = delete;
    FakeMoonlightHost(FakeMoonlightHost&&) = delete;
    FakeMoonlightHost& operator=(FakeMoonlightHost&&) = delete;

    bool listening() const { return listening_; }
    int httpPort() const { return static_cast<int>(plain_.serverPort()); }
    int httpsPort() const { return static_cast<int>(tls_.serverPort()); }
    // What a paired record stores: the certificate every TLS reply is pinned
    // against.
    QString certPem() const { return certPem_; }

    // ── What the host will say ──────────────────────────────────────────────
    // Set before the client dials; read on the Qt thread that serves it.
    int pairStatus = 1;
    int currentGame = 0;
    QString uniqueId = QStringLiteral("fixture-uuid");
    // A refusal in the BODY of an HTTP 200, which is how a real host says an
    // app is already running. `launchResume` is its <resume> flag.
    bool launchOk = true;
    bool launchResume = false;
    // Refuse every RTSP step, which is a launch that succeeded and a stream
    // that never came up.
    bool rtspAnswers = true;
    // Accept the RTSP connection and never answer, so the session sits with the
    // host holding an app for it.
    bool rtspStalls = false;
    // What the human types into this host's PIN page. A different PIN derives a
    // different key, which is exactly what a mistyped one looks like from here.
    QString typedPin;
    // Turn the request down at phase 1, the way a host with pairing switched
    // off does.
    bool pairDeclines = false;
    // Accept the pairing request and never answer it, which is a host waiting
    // for a human to walk over and type the code.
    bool pairStalls = false;

    // ── What the client did ─────────────────────────────────────────────────
    int seen(const QString& path) const {
        int n = 0;
        for (const auto& request : requests_) {
            if (request.path == path) { ++n; }
        }
        return n;
    }
    const QList<FakeRequest>& requests() const { return requests_; }
    QStringList paths() const {
        QStringList out;
        for (const auto& request : requests_) { out.append(request.path); }
        return out;
    }
    void forgetRequests() { requests_.clear(); }
    int mediaPings() const { return mediaPings_; }

    bool controlConnected() const { return controlConnected_.load(std::memory_order_relaxed); }
    // CONTROLLER_ARRIVAL packets the client sealed and sent on the live link.
    int arrivals() const { return arrivals_.load(std::memory_order_relaxed); }
    // The active mask on the most recent CONTROLLER_MULTI, which is how a pad
    // says it has been unplugged.
    int lastActiveMask() const { return lastMask_.load(std::memory_order_relaxed); }

    // ── What the host does to the session ───────────────────────────────────
    // THE ENET HOST BELONGS TO ITS SERVICE THREAD. Both endings are asked for
    // here and carried out there: sharing an ENetHost between two threads
    // needs a lock, and a lock held across a blocking service call is held
    // essentially all the time, which starves whatever else wants it.

    // The clean end: an encrypted TERMINATION, keyed with the rikey the client
    // put in its own launch request.
    void endSession() { endRequested_.store(true, std::memory_order_relaxed); }

    // The other end: the link simply goes away, with nothing said. A client
    // that treats this as a host-ended session would close somebody's game.
    void dropLink() { dropRequested_.store(true, std::memory_order_relaxed); }

  private:
    // ── HTTP, both ports ────────────────────────────────────────────────────
    void serveHttp(QTcpSocket* sock, bool tls) {
        if (sock == nullptr) { return; }
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        auto pending = std::make_shared<QByteArray>();
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, pending, tls] {
            pending->append(sock->readAll());
            if (!pending->contains("\r\n\r\n")) { return; }
            const QUrl url = requestUrl(*pending);
            FakeRequest request;
            request.path = url.path();
            request.query = QUrlQuery(url.query());
            request.tls = tls;
            requests_.append(request);
            // A host parks the pairing request until the PIN is typed, so the
            // stall is silence on an open socket rather than a refusal.
            if (pairStalls && request.path == QLatin1String("/pair")) { return; }
            reply(sock, bodyFor(request));
        });
    }

    static QUrl requestUrl(const QByteArray& head) {
        const QList<QByteArray> line = head.left(head.indexOf("\r\n")).split(' ');
        return QUrl(QString::fromUtf8(line.size() > 1 ? line.at(1) : QByteArray()));
    }

    static void reply(QTcpSocket* sock, const QByteArray& body) {
        sock->write(QByteArray("HTTP/1.1 200 OK\r\nContent-Length: ") +
                    QByteArray::number(body.size()) + QByteArray("\r\nConnection: close\r\n\r\n") +
                    body);
        sock->flush();
        sock->disconnectFromHost();
    }

    QByteArray bodyFor(const FakeRequest& request) {
        if (request.path == QLatin1String("/serverinfo")) { return serverInfoBody(request.tls); }
        if (request.path == QLatin1String("/applist")) { return appListBody(); }
        if (request.path == QLatin1String("/cancel")) {
            // 200 whether or not anything was running, which is exactly why the
            // client has to re-probe rather than believe it.
            return QByteArrayLiteral("<root status_code=\"200\"><cancel>1</cancel></root>");
        }
        if (request.path == QLatin1String("/launch") || request.path == QLatin1String("/resume")) {
            return launchBody(request);
        }
        if (request.path == QLatin1String("/pair")) { return pairBody(request); }
        return QByteArrayLiteral("<root status_code=\"404\"></root>");
    }

    // PairStatus 0 on the plaintext port whatever the truth, as a live Sunshine
    // host answers every plaintext caller including one it holds a pairing
    // for; the flag is computed on the mutual-TLS route alone. A client that
    // reads the plaintext flag as an answer fails here the way it fails there.
    QByteArray serverInfoBody(bool tls) const {
        return QStringLiteral("<root status_code=\"200\">"
                              "<hostname>Fixture</hostname>"
                              "<uniqueid>%1</uniqueid>"
                              "<appversion>7.1.431</appversion>"
                              "<state>SUNSHINE_SERVER_FREE</state>"
                              "<PairStatus>%2</PairStatus>"
                              "<currentgame>%3</currentgame>"
                              "</root>")
            .arg(uniqueId)
            .arg(tls ? pairStatus : 0)
            .arg(currentGame)
            .toUtf8();
    }

    static QByteArray appListBody() {
        return QByteArrayLiteral("<root status_code=\"200\"><App><IsHdrSupported>0</IsHdrSupported>"
                                 "<AppTitle>Desktop</AppTitle><ID>1</ID></App></root>");
    }

    QByteArray launchBody(const FakeRequest& request) {
        if (!launchOk) {
            // The shape that reads as success to anything looking only at the
            // status line: HTTP 200 carrying the host's own refusal.
            return QStringLiteral("<root status_code=\"400\" status_message=\"An app is already "
                                  "running on this host\"><resume>%1</resume></root>")
                .arg(launchResume ? 1 : 0)
                .toUtf8();
        }
        // The rikey the client minted for this attempt is the control stream's
        // key, so the host reads it here and seals its own packets with it.
        installRikey(request.query.queryItemValue(QStringLiteral("rikey")));
        return QStringLiteral("<root status_code=\"200\"><sessionUrl0>rtsp://127.0.0.1:%1"
                              "</sessionUrl0><gamesession>1</gamesession></root>")
            .arg(rtsp_.serverPort())
            .toUtf8();
    }

    // The host half of the five pairing phases, run for real so a pairing that
    // reports success has actually happened. The algorithm mirrors the one
    // test_moonlight_pairing drives directly; here it rides the wire, which is
    // what makes "confirming trust must PERSIST it" observable at all.
    QByteArray pairBody(const FakeRequest& request) {
        const auto param = [&request](const char* name) {
            return request.query.queryItemValue(QString::fromLatin1(name)).toStdString();
        };
        const QString phrase = request.query.queryItemValue(QStringLiteral("phrase"));
        if (pairDeclines) { return pairRefusal(); }
        if (phrase == QLatin1String("getservercert")) {
            const auto salt = util::fromHex(param("salt"));
            const auto clientCert = util::fromHex(param("clientcert"));
            if (!salt || salt->size() != 16 || !clientCert) { return pairRefusal(); }
            std::array<std::uint8_t, 16> saltBytes{};
            std::copy(salt->cbegin(), salt->cend(), saltBytes.begin());
            // The key is the PIN the human types HERE, which is why the fixture
            // is told it rather than shown it.
            aesKey_ = mooncrypto::derivePairingKey(saltBytes, typedPin.toStdString());
            clientCertPem_.assign(clientCert->cbegin(), clientCert->cend());
            const QByteArray pem = certPem_.toUtf8();
            return pairPayload(QStringLiteral("plaincert"),
                               upperHex(mooncrypto::Bytes(pem.cbegin(), pem.cend())));
        }
        if (request.query.hasQueryItem(QStringLiteral("clientchallenge"))) {
            return pairPhase2(param("clientchallenge"));
        }
        if (request.query.hasQueryItem(QStringLiteral("serverchallengeresp"))) {
            return pairPhase3(param("serverchallengeresp"));
        }
        if (request.query.hasQueryItem(QStringLiteral("clientpairingsecret"))) {
            return pairPhase4(param("clientpairingsecret")) ? pairPayload({}, {}) : pairRefusal();
        }
        // Phase 5, the mutual-TLS pairchallenge: reaching it at all is the proof
        // it exists for, since this socket only carries a verified channel.
        return pairPayload({}, {});
    }

    QByteArray pairPhase2(const std::string& clientChallengeHex) {
        const auto encrypted = util::fromHex(clientChallengeHex);
        if (!encrypted) { return pairRefusal(); }
        const auto challenge =
            mooncrypto::aesEcbDecrypt(aesKey_, encrypted->data(), encrypted->size());
        const auto certSig = mooncrypto::certSignature(certPem_.toStdString());
        if (!challenge || !certSig) { return pairRefusal(); }

        mooncrypto::Bytes material = *challenge;
        material.insert(material.end(), certSig->cbegin(), certSig->cend());
        material.insert(material.end(), serverSecret_.cbegin(), serverSecret_.cend());
        const auto hash = mooncrypto::sha256(material.data(), material.size());

        mooncrypto::Bytes response(hash.cbegin(), hash.cend());
        response.insert(response.end(), serverChallenge_.cbegin(), serverChallenge_.cend());
        const auto sealed = mooncrypto::aesEcbEncrypt(aesKey_, response.data(), response.size());
        if (!sealed) { return pairRefusal(); }
        return pairPayload("challengeresponse", upperHex(*sealed));
    }

    QByteArray pairPhase3(const std::string& serverChallengeRespHex) {
        const auto encrypted = util::fromHex(serverChallengeRespHex);
        if (!encrypted) { return pairRefusal(); }
        const auto decrypted =
            mooncrypto::aesEcbDecrypt(aesKey_, encrypted->data(), encrypted->size());
        const auto signature = mooncrypto::rsaSignSha256(
            keyPem_.toStdString(), serverSecret_.data(), serverSecret_.size());
        if (!decrypted || !signature) { return pairRefusal(); }
        clientHash_ = *decrypted;
        mooncrypto::Bytes payload(serverSecret_.cbegin(), serverSecret_.cend());
        payload.insert(payload.end(), signature->cbegin(), signature->cend());
        return pairPayload("pairingsecret", upperHex(payload));
    }

    bool pairPhase4(const std::string& clientPairingSecretHex) const {
        const auto payload = util::fromHex(clientPairingSecretHex);
        if (!payload || payload->size() < 16 + mooncrypto::kRsaSignatureSize) { return false; }
        const auto clientCertSig = mooncrypto::certSignature(clientCertPem_);
        if (!clientCertSig) { return false; }
        mooncrypto::Bytes material(serverChallenge_.cbegin(), serverChallenge_.cend());
        material.insert(material.end(), clientCertSig->cbegin(), clientCertSig->cend());
        material.insert(material.end(), payload->cbegin(), payload->cbegin() + 16);
        const auto expected = mooncrypto::sha256(material.data(), material.size());
        if (clientHash_.size() != expected.size() ||
            !std::equal(expected.cbegin(), expected.cend(), clientHash_.cbegin())) {
            return false;
        }
        return mooncrypto::rsaVerifySha256(clientCertPem_, payload->data(), 16,
                                           payload->data() + 16, mooncrypto::kRsaSignatureSize);
    }

    static QByteArray pairPayload(const QString& tag, const std::string& hex) {
        QString body = QStringLiteral("<root status_code=\"200\"><paired>1</paired>");
        if (!tag.isEmpty()) {
            body += QStringLiteral("<%1>%2</%1>").arg(tag, QString::fromStdString(hex));
        }
        return (body + QStringLiteral("</root>")).toUtf8();
    }

    static QByteArray pairRefusal() {
        return QByteArrayLiteral("<root status_code=\"200\"><paired>0</paired></root>");
    }

    static std::string upperHex(const mooncrypto::Bytes& bytes) {
        std::string hex = util::toHex(bytes);
        std::transform(hex.begin(), hex.end(), hex.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return hex;
    }

    void installRikey(const QString& hex) {
        const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
        if (raw.size() != 16) { return; }
        std::array<std::uint8_t, 16> key{};
        std::copy(raw.cbegin(), raw.cend(), key.begin());
        // The cipher is the one thing both threads touch: written here, on the
        // Qt thread serving /launch, and read by the service thread below.
        std::lock_guard<std::mutex> lock(cipherMtx_);
        cipher_.setKey(key);
        hostSeq_ = 0;
    }

    // ── RTSP: one connection per message, the way a real host answers ───────
    void serveRtsp() {
        QTcpSocket* sock = rtsp_.nextPendingConnection();
        if (sock == nullptr) { return; }
        QObject::connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        auto pending = std::make_shared<QByteArray>();
        QObject::connect(sock, &QTcpSocket::readyRead, sock, [this, sock, pending] {
            pending->append(sock->readAll());
            if (!pending->contains("\r\n\r\n")) { return; }
            if (rtspStalls) { return; }
            const QByteArray head = pending->left(pending->indexOf("\r\n\r\n"));
            sock->write(rtspReply(head));
            sock->flush();
            // The host hangs up on its own once it has answered, which is what
            // frames a reply that carries no Content-length.
            sock->disconnectFromHost();
        });
    }

    QByteArray rtspReply(const QByteArray& head) const {
        if (!rtspAnswers) { return QByteArrayLiteral("RTSP/1.0 500 Internal\r\nCSeq: 1\r\n\r\n"); }
        QByteArray out = QByteArrayLiteral("RTSP/1.0 200 OK\r\nCSeq: ") + cseqOf(head) +
                         QByteArrayLiteral("\r\nSession: DEADBEEFCAFE;timeout = 90\r\n");
        if (head.startsWith("SETUP ")) {
            if (head.contains("streamid=audio")) {
                out += QByteArrayLiteral("Transport: server_port=") +
                       QByteArray::number(audio_.localPort()) +
                       QByteArrayLiteral("\r\nX-SS-Ping-Payload: 0123456789abcdef\r\n");
            } else if (head.contains("streamid=video")) {
                out += QByteArrayLiteral("Transport: server_port=") +
                       QByteArray::number(video_.localPort()) +
                       QByteArrayLiteral("\r\nX-SS-Ping-Payload: fedcba9876543210\r\n");
            } else {
                out += QByteArrayLiteral("Transport: server_port=") +
                       QByteArray::number(controlPort_) +
                       QByteArrayLiteral("\r\nX-SS-Connect-Data: 7\r\n");
            }
        }
        return out + QByteArrayLiteral("\r\n");
    }

    static QByteArray cseqOf(const QByteArray& head) {
        const qsizetype at = head.indexOf("CSeq:");
        if (at < 0) { return QByteArrayLiteral("1"); }
        return head.mid(at + 5, head.indexOf('\r', at) - at - 5).trimmed();
    }

    void drain(QUdpSocket& sock) {
        while (sock.hasPendingDatagrams()) {
            sock.readDatagram(nullptr, 0);
            ++mediaPings_;
        }
    }

    // ── The control stream: a real ENet listener on its own thread ──────────
    bool startControl() {
        if (enet_initialize() != 0) { return false; }
        ENetAddress address{};
        if (enet_address_set_host(&address, "127.0.0.1") != 0) { return false; }
        enet_address_set_port(&address, 0);
        enetHost_ = enet_host_create(address.address.ss_family, &address, 4, 1, 0, 0);
        if (enetHost_ == nullptr) { return false; }
        // ENet has no port getter, and the bound port is only knowable from the
        // address it filled in after the bind.
        const auto* bound = reinterpret_cast<const sockaddr_in*>(&enetHost_->address.address);
        controlPort_ = static_cast<int>(ntohs(bound->sin_port));
        running_.store(true, std::memory_order_relaxed);
        thread_ = std::thread([this] { serviceControl(); });
        return controlPort_ > 0;
    }

    void stopControl() {
        running_.store(false, std::memory_order_relaxed);
        if (thread_.joinable()) { thread_.join(); }
        if (enetHost_ != nullptr) {
            enet_host_destroy(enetHost_);
            enetHost_ = nullptr;
        }
        peer_ = nullptr;
    }

    void serviceControl() {
        while (running_.load(std::memory_order_relaxed)) {
            runRequestedEnding();
            ENetEvent event{};
            if (enet_host_service(enetHost_, &event, 5) <= 0) { continue; }
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                peer_ = event.peer;
                controlConnected_.store(true, std::memory_order_relaxed);
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                absorb(event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                peer_ = nullptr;
                controlConnected_.store(false, std::memory_order_relaxed);
                break;
            default:
                break;
            }
        }
    }

    // Whichever ending a case asked for, run where the ENet host lives.
    void runRequestedEnding() {
        if (peer_ == nullptr) { return; }
        if (endRequested_.exchange(false, std::memory_order_relaxed)) {
            std::uint8_t plaintext[moonwire::kMaxPlaintextSize];
            const std::size_t len = moonwire::encodeTermination(plaintext);
            std::array<std::uint8_t, moonwire::kMaxPlaintextSize + 64> sealed{};
            std::size_t total = 0;
            {
                std::lock_guard<std::mutex> lock(cipherMtx_);
                if (cipher_.hasKey()) {
                    total = cipher_.seal(hostSeq_++, plaintext, len, sealed.data());
                }
            }
            if (total > 0) {
                enet_peer_send(peer_, 0,
                               enet_packet_create(sealed.data(), total, ENET_PACKET_FLAG_RELIABLE));
                enet_host_flush(enetHost_);
            }
        }
        if (dropRequested_.exchange(false, std::memory_order_relaxed)) {
            enet_peer_disconnect_now(peer_, 0);
            peer_ = nullptr;
            controlConnected_.store(false, std::memory_order_relaxed);
        }
    }

    // Opens one sealed control packet and records what it was. The counters are
    // atomics because they are written here, on the ENet service thread, and
    // read by the case on the Qt thread.
    void absorb(const std::uint8_t* data, std::size_t len) {
        std::array<std::uint8_t, 256> plaintext{};
        std::optional<std::size_t> opened;
        {
            std::lock_guard<std::mutex> lock(cipherMtx_);
            if (!cipher_.hasKey()) { return; }
            opened = cipher_.open(data, len, plaintext.data(), plaintext.size());
        }
        if (!opened || *opened < 12) { return; }
        const auto u16At = [&plaintext](std::size_t at) {
            return static_cast<std::uint16_t>(plaintext[at] |
                                              (static_cast<std::uint16_t>(plaintext[at + 1]) << 8));
        };
        if (u16At(0) != moonproto::kPktInputData) { return; }
        // [pkt u16][len u16][data size u32 BE][input type u32 LE][body]
        const std::uint32_t inputType =
            static_cast<std::uint32_t>(u16At(8)) | (static_cast<std::uint32_t>(u16At(10)) << 16);
        if (inputType == moonproto::kInputControllerArrival) {
            arrivals_.fetch_add(1, std::memory_order_relaxed);
        } else if (inputType == moonproto::kInputControllerMulti && *opened >= 18) {
            // The active mask sits two words into the body, and a bit missing
            // from it is how a pad is told to unplug.
            lastMask_.store(static_cast<int>(u16At(16)), std::memory_order_relaxed);
        }
    }

    // The host's own pairing state, filled in as the phases arrive.
    std::array<std::uint8_t, mooncrypto::kAesKeySize> aesKey_{};
    std::array<std::uint8_t, 16> serverSecret_{{0x21, 0x28, 0x2F, 0x36, 0x3D, 0x44, 0x4B, 0x52,
                                                0x59, 0x60, 0x67, 0x6E, 0x75, 0x7C, 0x83, 0x8A}};
    std::array<std::uint8_t, 16> serverChallenge_{{0x87, 0x8E, 0x95, 0x9C, 0xA3, 0xAA, 0xB1, 0xB8,
                                                   0xBF, 0xC6, 0xCD, 0xD4, 0xDB, 0xE2, 0xE9, 0xF0}};
    std::string clientCertPem_;
    mooncrypto::Bytes clientHash_;

    QTcpServer plain_;
    QSslServer tls_;
    QTcpServer rtsp_;
    QUdpSocket video_;
    QUdpSocket audio_;
    bool listening_ = false;
    QString certPem_;
    QString keyPem_;
    QList<FakeRequest> requests_;
    int mediaPings_ = 0;

    // The ENet host and its peer belong to the service thread once it starts.
    ENetHost* enetHost_ = nullptr;
    ENetPeer* peer_ = nullptr;
    int controlPort_ = 0;
    mutable std::mutex cipherMtx_;
    mooncrypto::ControlCipher cipher_;
    std::uint32_t hostSeq_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> endRequested_{false};
    std::atomic<bool> dropRequested_{false};
    std::atomic<bool> controlConnected_{false};
    std::atomic<int> arrivals_{0};
    std::atomic<int> lastMask_{-1};
};

} // namespace dish::test
