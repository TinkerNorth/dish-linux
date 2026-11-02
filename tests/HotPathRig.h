// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "Network/SatelliteClient.h"

#include "FeatureSettings.h"
#include "Input/GamepadInputProcessor.h"
#include "Input/SDLGamepadBridge.h"
#include "LightbarRouting.h"
#include "Network/ConnectionHub.h"
#include "Network/ConnectionStore.h"
#include "Network/WifiConnection.h"
#include "Network/WifiConnectionManager.h"
#include "core/audio/PadAudioMatcher.h"
#include "core/input/UsbOutputReports.h"
#include "core/input/UsbReportParsers.h"
#include "core/model/Protocol.h"
#include "core/reducer/BindingPresence.h"
#include "core/reducer/FeedbackRouting.h"
#include "core/reducer/RumbleRouting.h"
#include "core/reducer/SlotPathFields.h"
#include "core/reducer/TouchpadRouting.h"
#include "core/thread/TrackedMutex.h"
#include "core/wire/SessionCrypto.h"
#include "repository/MotionPreferenceRepository.h"
#include "repository/RumblePreferenceRepository.h"
#include "source/audio/SpeakerPlayoutEngine.h"
#include "source/store/MotionEnabledStore.h"
#include "source/store/RumbleEnabledStore.h"
#include "source/usb/UsbDeviceGateway.h"
#include "source/usb/UsbGamepadManager.h"

#include "FakeAudioGateway.h"
#include "FakePairingListener.h"
#include "QSettingsFixture.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QHash>
#include <QJsonObject>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <SDL.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dish::test::hotpath {

using Clock = std::chrono::steady_clock;
using SessionKey = std::array<std::uint8_t, wire::kCryptoKeySize>;

inline constexpr std::uint8_t kPairingKeyByte = 0x11;
inline constexpr std::array<std::uint8_t, wire::kSessionSaltSize> kSessionSalt{1, 2, 3, 4,
                                                                               5, 6, 7, 8};
inline constexpr std::array<std::uint8_t, 4> kToken{0, 0, 0, 1};
inline constexpr std::uint32_t kTokenBe = 1;
inline constexpr int kSettleMs = 10'000;
inline constexpr std::size_t kHeaderBytes = 8;
inline constexpr std::size_t kInnerHeaderBytes = 4;
inline constexpr std::size_t kTagBytes = 16;
inline constexpr std::uint16_t kAckEpoch = 1;
inline constexpr std::uint16_t kAckBitmap = 1;

inline QString sessionsPath() { return QStringLiteral("/api/connections"); }
inline QString controllerPath() { return QStringLiteral("/api/connections/conn_1/controllers/0"); }

inline QJsonObject grantJson() {
    return QJsonObject{{QStringLiteral("connectionId"), QStringLiteral("conn_1")},
                       {QStringLiteral("token"), QStringLiteral("00000001")},
                       {QStringLiteral("sessionSalt"), QStringLiteral("0102030405060708")},
                       {QStringLiteral("epoch"), 1},
                       {QStringLiteral("protocolVersion"), proto::kProtocolVersion}};
}

inline QJsonObject controllerAppliedJson() {
    return QJsonObject{{QStringLiteral("epoch"), 1},
                       {QStringLiteral("controller"),
                        QJsonObject{{QStringLiteral("ctrlIdx"), 0},
                                    {QStringLiteral("result"), QStringLiteral("ok")}}}};
}

// The satellite's REST API as the rig scripts it: every session PUT is granted, the controller
// slot is applied, and anything else (a release, a controller DELETE) is acknowledged.
inline PairingAnswer answerRest(const SeenRequest& request) {
    if (request.method == "PUT" && request.path == sessionsPath()) {
        return PairingAnswer{200, grantJson()};
    }
    if (request.method == "PUT" && request.path == controllerPath()) {
        return PairingAnswer{200, controllerAppliedJson()};
    }
    return PairingAnswer{200, QJsonObject{}};
}

// The key both ends derive from the grant above and the pairing key every rig session is paired
// with, exactly as WifiConnectionManager derives it.
inline SessionKey derivedSessionKey() {
    std::array<std::uint8_t, wire::kCryptoKeySize> pairingKey{};
    pairingKey.fill(kPairingKeyByte);
    SessionKey out{};
    wire::deriveSessionKey(pairingKey.data(), kSessionSalt.data(), kTokenBe, out.data());
    return out;
}

inline std::uint16_t readBe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}

inline void writeBe16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v & 0xFFU);
}

inline std::uint32_t readBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

inline void writeBe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    p[3] = static_cast<std::uint8_t>(v & 0xFFU);
}

// Runs the Qt loop of the calling (owner) thread until `done` or the timeout.
inline bool pumpUntil(const std::function<bool()>& done, int timeoutMs = kSettleMs) {
    const QDeadlineTimer deadline(timeoutMs);
    while (!done()) {
        if (deadline.hasExpired()) { return false; }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
    return true;
}

struct BoundSocket {
    int fd = -1;
    std::uint16_t port = 0;
};

inline BoundSocket bindLoopbackUdp() {
    BoundSocket bound;
    bound.fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (bound.fd < 0) { return bound; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    socklen_t len = sizeof(addr);
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
        ::bind(bound.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::getsockname(bound.fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(bound.fd);
        return BoundSocket{};
    }
    // Bounds the blocking receive so the loop sees its stop flag.
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 50'000;
    ::setsockopt(bound.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    bound.port = ntohs(addr.sin_port);
    return bound;
}

// The satellite end of one session on 127.0.0.1: decrypts what the dish sends and hands each inner
// message to `handler` on its own receive thread, answers every heartbeat ping with an ack, and
// sends feedback back to the dish's address once the dish has sent anything.
class LoopbackSatellite {
  public:
    using Handler = std::function<void(std::uint16_t msgType, const std::uint8_t* body,
                                       std::size_t len, Clock::time_point arrived)>;

    LoopbackSatellite(const SessionKey& key, Handler handler)
        : key_(key), handler_(std::move(handler)), bound_(bindLoopbackUdp()) {
        running_.store(bound_.fd >= 0);
        if (running_.load()) {
            thread_ = std::thread([this] { receiveLoop(); });
        }
    }

    ~LoopbackSatellite() {
        running_.store(false);
        if (thread_.joinable()) { thread_.join(); }
        if (bound_.fd >= 0) { ::close(bound_.fd); }
    }

    LoopbackSatellite(const LoopbackSatellite&) = delete;
    LoopbackSatellite& operator=(const LoopbackSatellite&) = delete;
    LoopbackSatellite(LoopbackSatellite&&) = delete;
    LoopbackSatellite& operator=(LoopbackSatellite&&) = delete;

    bool ok() const { return bound_.fd >= 0; }
    std::uint16_t port() const { return bound_.port; }
    bool knowsDish() const { return knowsDish_.load(std::memory_order_acquire); }
    std::uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }
    std::uint64_t acksSent() const { return acksSent_.load(std::memory_order_relaxed); }

    // False until the dish has sent something, since that is how the dish's address is learned.
    bool sendToDish(std::uint16_t msgType, const std::uint8_t* body, std::size_t len) {
        if (!knowsDish() || len > proto::kUdpMaxInnerPayloadBytes) { return false; }
        std::array<std::uint8_t, kInnerHeaderBytes + proto::kUdpMaxInnerPayloadBytes> inner{};
        writeBe16(inner.data(), msgType);
        writeBe16(inner.data() + 2, static_cast<std::uint16_t>(len));
        if (len > 0) { std::memcpy(inner.data() + kInnerHeaderBytes, body, len); }
        std::array<std::uint8_t, proto::kUdpDatagramMaxBytes> packet{};
        // One sender at a time keeps the counters on the wire in order, as the dish's replay guard
        // requires; the satellite's own sender is single-threaded the same way.
        const std::lock_guard<std::mutex> lock(sendMtx_);
        const std::uint32_t counter = ++sendCounter_;
        std::memcpy(packet.data(), kToken.data(), kToken.size());
        writeBe32(packet.data() + kToken.size(), counter);
        unsigned long long cipherLen = 0;
        if (!wire::encryptPacket(key_.data(), wire::kDirServerToClient, counter, kTokenBe,
                                 inner.data(), kInnerHeaderBytes + len,
                                 packet.data() + kHeaderBytes, &cipherLen)) {
            return false;
        }
        const ssize_t sent =
            ::sendto(bound_.fd, packet.data(), static_cast<std::size_t>(kHeaderBytes + cipherLen),
                     0, reinterpret_cast<const sockaddr*>(&dish_), sizeof(dish_));
        return sent > 0;
    }

  private:
    void receiveLoop() {
        std::array<std::uint8_t, proto::kUdpDatagramMaxBytes> buf{};
        std::array<std::uint8_t, proto::kUdpDatagramMaxBytes> plain{};
        while (running_.load(std::memory_order_relaxed)) {
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            const ssize_t n = ::recvfrom(bound_.fd, buf.data(), buf.size(), 0,
                                         reinterpret_cast<sockaddr*>(&from), &fromLen);
            const auto arrived = Clock::now();
            if (n <= 0) { continue; }
            if (!knowsDish()) {
                dish_ = from;
                knowsDish_.store(true, std::memory_order_release);
            }
            handleDatagram(buf.data(), static_cast<std::size_t>(n), plain, arrived);
        }
    }

    void handleDatagram(const std::uint8_t* buf, std::size_t n,
                        std::array<std::uint8_t, proto::kUdpDatagramMaxBytes>& plain,
                        Clock::time_point arrived) {
        if (n < kHeaderBytes + kInnerHeaderBytes + kTagBytes ||
            std::memcmp(buf, kToken.data(), kToken.size()) != 0) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint32_t counter = readBe32(buf + kToken.size());
        unsigned long long plainLen = 0;
        if (!wire::decryptPacket(key_.data(), wire::kDirClientToServer, counter, kTokenBe,
                                 buf + kHeaderBytes, n - kHeaderBytes, plain.data(), &plainLen) ||
            plainLen < kInnerHeaderBytes) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::uint16_t msgType = readBe16(plain.data());
        const std::size_t len = readBe16(plain.data() + 2);
        if (kInnerHeaderBytes + len > plainLen) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        handler_(msgType, plain.data() + kInnerHeaderBytes, len, arrived);
        if (msgType == proto::kMsgHeartbeat) { sendAck(); }
    }

    void sendAck() {
        std::array<std::uint8_t, proto::kHeartbeatAckPayloadBytes> ack{};
        ack[0] = 1;
        ack[1] = 1;
        writeBe16(ack.data() + 2, kAckEpoch);
        writeBe16(ack.data() + 4, kAckBitmap);
        if (sendToDish(proto::kMsgHeartbeatAck, ack.data(), ack.size())) {
            acksSent_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const SessionKey key_;
    const Handler handler_;
    const BoundSocket bound_;
    sockaddr_in dish_{};
    std::atomic<bool> knowsDish_{false};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> acksSent_{0};
    std::mutex sendMtx_;
    std::uint32_t sendCounter_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// What UsbGamepadManager drives for one claimed DualSense, with the hidraw read thread played by
// whoever calls deliver(): it keeps the report sink the manager hands to claim().
class FakeDualSenseGateway : public source::usb::UsbDeviceGateway {
  public:
    static constexpr int kVendorId = 0x054C;
    static constexpr int kProductId = 0x0CE6;

    static QString slotId() { return QString::number((kVendorId << 16) | (kProductId & 0xFFFF)); }

    std::vector<source::usb::UsbDeviceInfo> enumerate() override {
        source::usb::UsbDeviceInfo d;
        d.vendorId = kVendorId;
        d.productId = kProductId;
        d.name = "DualSense Wireless Controller";
        d.hasOutEndpoint = true;
        d.hasImu = true;
        return {d};
    }

    source::usb::ClaimResult
    claim(const source::usb::UsbDeviceInfo& /*device*/,
          std::function<void(const source::usb::UsbReport&)> onReport) override {
        onReport_ = std::move(onReport);
        return source::usb::ClaimResult::success(nextSyntheticId_--);
    }

    void releaseClaim(int /*syntheticId*/) override {}
    bool isKnownFastLaneModel(int /*vendorId*/, int /*productId*/) const override { return true; }
    bool isClaimAlive(int /*syntheticId*/) const override { return true; }
    std::int64_t completionCount(int /*syntheticId*/) const override { return 0; }
    std::int64_t decodedCount(int /*syntheticId*/) const override { return 0; }

    bool writeOutputReport(int /*syntheticId*/, const std::uint8_t* /*data*/,
                           std::size_t /*len*/) override {
        writes_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    bool claimed() const { return static_cast<bool>(onReport_); }
    void deliver(const source::usb::UsbReport& report) const { onReport_(report); }
    std::uint64_t writes() const { return writes_.load(std::memory_order_relaxed); }

  private:
    std::function<void(const source::usb::UsbReport&)> onReport_;
    int nextSyntheticId_ = -1000;
    std::atomic<std::uint64_t> writes_{0};
};

// A full USB DualSense report as the decoder leaves it: input, IMU, touchpad and battery valid.
inline source::usb::UsbReport dualSenseReport(std::int16_t rx) {
    source::usb::UsbReport r;
    r.wButtons = 0x1000;
    r.lt = 40;
    r.rt = 80;
    r.lx = 9000;
    r.ly = -9000;
    r.rx = rx;
    r.ry = 12000;
    r.motionValid = true;
    r.gyroX = 10;
    r.gyroY = -20;
    r.gyroZ = 30;
    r.accelX = 100;
    r.accelY = 8192;
    r.accelZ = -100;
    r.touchpadValid = true;
    r.finger0Active = true;
    r.finger0Id = 3;
    r.finger0X = 1000;
    r.finger0Y = 2000;
    r.batteryValid = true;
    r.batteryLevel = 80;
    r.batteryStatus = 1;
    return r;
}

// An SDL virtual game controller: SDL maps it from its descriptor, so the real bridge opens it as
// a pad with rumble and an LED, and every axis move becomes a controller event on the SDL thread.
class VirtualPad {
  public:
    static constexpr Uint16 kVendorId = 0x045E;
    static constexpr Uint16 kProductId = 0x028E;

    explicit VirtualPad(const char* name) : name_(name) {
        SDL_VirtualJoystickDesc desc{};
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
        desc.naxes = SDL_CONTROLLER_AXIS_MAX;
        desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
        desc.vendor_id = kVendorId;
        desc.product_id = kProductId;
        desc.name = name_;
        desc.userdata = this;
        desc.Rumble = &VirtualPad::onRumble;
        desc.SetLED = &VirtualPad::onLed;
        const int index = SDL_JoystickAttachVirtualEx(&desc);
        if (index >= 0) {
            instanceId_ = SDL_JoystickGetDeviceInstanceID(index);
            joystick_ = SDL_JoystickOpen(index);
        }
    }

    ~VirtualPad() {
        if (joystick_ != nullptr) { SDL_JoystickClose(joystick_); }
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (SDL_JoystickGetDeviceInstanceID(i) == instanceId_) {
                SDL_JoystickDetachVirtual(i);
                break;
            }
        }
    }

    VirtualPad(const VirtualPad&) = delete;
    VirtualPad& operator=(const VirtualPad&) = delete;
    VirtualPad(VirtualPad&&) = delete;
    VirtualPad& operator=(VirtualPad&&) = delete;

    bool ok() const { return joystick_ != nullptr; }
    QString deviceId() const { return QStringLiteral("sdl:%1").arg(instanceId_); }
    void setRightX(std::int16_t value) const {
        SDL_JoystickSetVirtualAxis(joystick_, SDL_CONTROLLER_AXIS_RIGHTX, value);
    }
    std::uint64_t rumbles() const { return rumbles_.load(std::memory_order_relaxed); }
    std::uint64_t leds() const { return leds_.load(std::memory_order_relaxed); }

  private:
    static int SDLCALL onRumble(void* self, Uint16 /*low*/, Uint16 /*high*/) {
        static_cast<VirtualPad*>(self)->rumbles_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    static int SDLCALL onLed(void* self, Uint8 /*red*/, Uint8 /*green*/, Uint8 /*blue*/) {
        static_cast<VirtualPad*>(self)->leds_.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    const char* name_;
    SDL_JoystickID instanceId_ = -1;
    SDL_Joystick* joystick_ = nullptr;
    std::atomic<std::uint64_t> rumbles_{0};
    std::atomic<std::uint64_t> leds_{0};
};

// Mirrors AppModel's input routing and its feedback route (src/AppModel.cpp), which the harness
// cannot build: AppModel owns the tray, the real settings and the wake inhibitor. Keep it in step.
class AppGlue {
  public:
    AppGlue(const net::ConnectionHub* hub, input::SDLGamepadBridge* bridge,
            source::usb::UsbGamepadManager* usbManager,
            source::MotionEnabledStore* motionEnabledStore,
            source::RumbleEnabledStore* rumbleEnabledStore, FeatureSettings* featureSettings,
            source::audio::SpeakerPlayoutEngine* speakerEngine)
        : hub_(hub), bridge_(bridge), usbManager_(usbManager),
          motionEnabledStore_(*motionEnabledStore), rumbleEnabledStore_(*rumbleEnabledStore),
          featureSettings_(featureSettings), speakerEngine_(*speakerEngine) {}

    // AppModel's constructor, the four processor senders. A slot bound to a Moonlight host has no
    // satellite sender and falls through to the Moonlight tables, under the same lock.
    void installSenders(input::GamepadInputProcessor& processor) {
        processor.setReportSender([this](const std::string& did, std::uint16_t buttons,
                                         std::uint8_t lt, std::uint8_t rt, std::int16_t lx,
                                         std::int16_t ly, std::int16_t rx, std::int16_t ry) {
            net::ConnectionHub::ReportSender sender;
            {
                std::lock_guard<thread::TrackedMutex> lock(routingMtx_);
                const QString key = QString::fromStdString(did);
                sender = routing_.value(key);
                if (!sender) { sender = moonlightRouting_.value(key); }
            }
            if (sender) { sender(buttons, lt, rt, lx, ly, rx, ry); }
        });

        processor.setMotionSender([this](const std::string& did, std::int16_t gx, std::int16_t gy,
                                         std::int16_t gz, std::int16_t ax, std::int16_t ay,
                                         std::int16_t az, std::uint32_t dtUs) {
            net::ConnectionHub::MotionSender sender;
            {
                std::lock_guard<thread::TrackedMutex> lock(routingMtx_);
                const QString key = QString::fromStdString(did);
                sender = motionRouting_.value(key);
                if (!sender) { sender = moonlightMotionRouting_.value(key); }
            }
            if (sender) { sender(gx, gy, gz, ax, ay, az, dtUs); }
        });

        processor.setBatterySender(
            [this](const std::string& did, std::uint8_t level, std::uint8_t status) {
                net::ConnectionHub::BatterySender sender;
                {
                    std::lock_guard<thread::TrackedMutex> lock(routingMtx_);
                    const QString key = QString::fromStdString(did);
                    sender = batteryRouting_.value(key);
                    if (!sender) { sender = moonlightBatteryRouting_.value(key); }
                }
                if (sender) { sender(level, status); }
            });

        processor.setTouchpadSender(
            [this](const std::string& did, const input::GamepadInputProcessor::TouchpadSample& s) {
                net::ConnectionHub::TouchpadSender sender;
                {
                    std::lock_guard<thread::TrackedMutex> lock(routingMtx_);
                    const QString key = QString::fromStdString(did);
                    sender = touchpadRouting_.value(key);
                    if (!sender) { sender = moonlightTouchRouting_.value(key); }
                }
                if (sender) {
                    const auto nowMs = static_cast<std::uint32_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
                    const auto fwd = reducer::assembleTouchpadForward(
                        s.finger0Active, s.finger0Id, s.finger0X, s.finger0Y, s.finger1Active,
                        s.finger1Id, s.finger1X, s.finger1Y, s.buttonPressed, nowMs);
                    sender(fwd.finger0Active, fwd.finger0Id, fwd.finger0X, fwd.finger0Y,
                           fwd.finger1Active, fwd.finger1Id, fwd.finger1X, fwd.finger1Y,
                           fwd.buttonPressed, fwd.eventTimeMs);
                }
            });
    }

    // AppModel::republishRouting over the slots on screen.
    void republishRouting(const QStringList& slotList) {
        QHash<QString, net::ConnectionHub::ReportSender> nextRouting;
        QHash<QString, net::ConnectionHub::MotionSender> nextMotion;
        QHash<QString, net::ConnectionHub::BatterySender> nextBattery;
        QHash<QString, net::ConnectionHub::TouchpadSender> nextTouchpad;
        for (const auto& slotId : slotList) {
            if (auto sender = hub_->reportSenderForSlot(slotId)) {
                nextRouting.insert(slotId, sender);
            }
            const bool motionAllowed = motionEnabledStore_.isEnabled(slotId.toStdString());
            if (auto sender = hub_->motionSenderForSlot(slotId); sender && motionAllowed) {
                nextMotion.insert(slotId, sender);
            }
            if (auto sender = hub_->batterySenderForSlot(slotId)) {
                nextBattery.insert(slotId, sender);
            }
            if (auto sender = hub_->touchpadSenderForSlot(slotId)) {
                nextTouchpad.insert(slotId, sender);
            }
        }
        {
            std::lock_guard<thread::TrackedMutex> lock(routingMtx_);
            routing_ = std::move(nextRouting);
            motionRouting_ = std::move(nextMotion);
            batteryRouting_ = std::move(nextBattery);
            touchpadRouting_ = std::move(nextTouchpad);
        }
    }

    // AppModel::installFeedbackHandlers, for the kinds the rig's satellites send.
    void installFeedbackHandlers(net::WifiConnection& conn, const QString& id) {
        conn.setRumbleHandler(
            [this, id](const net::SatelliteClient::RumbleMessage& rm) { onRumbleMessage(id, rm); });
        conn.setLightbarHandler([this, id](const net::SatelliteClient::LightbarMessage& lm) {
            onLightbarMessage(id, lm);
        });
        conn.setSpeakerAudioHandler(
            [this, id](const net::SatelliteClient::SpeakerAudioMessage& sm) {
                onPlayoutMessage(id, sm, source::audio::PlayoutLane::Speaker);
            });
    }

    std::uint64_t rumblesRouted() const { return rumblesRouted_.load(std::memory_order_relaxed); }
    std::uint64_t lightbarsRouted() const {
        return lightbarsRouted_.load(std::memory_order_relaxed);
    }

  private:
    struct SlotHardware {
        bool usbDirect = false;
        bool hasMotion = false;
        bool hasLightbar = false;
        bool hasTouchpad = false;
        bool hasRumble = false;
        bool hasTriggerEffects = false;
        bool hasPlayerLeds = false;
        bool hasMicLed = false;
        bool sdlEffects = false;
    };

    void onRumbleMessage(const QString& id, const net::SatelliteClient::RumbleMessage& rm) {
        const QString deviceId = boundSlotForConnection(id);
        if (deviceId.isEmpty()) { return; }
        deliverRumble(deviceId, rm.strongMagnitude, rm.weakMagnitude, rm.durationMs);
    }

    void onLightbarMessage(const QString& id, const net::SatelliteClient::LightbarMessage& lm) {
        const auto color =
            lightbarColorFromLightbarMessage(lm, featureSettings_->lightbarFollowGame());
        if (!color) { return; }
        const QString deviceId = boundSlotForConnection(id);
        if (deviceId.isEmpty()) { return; }
        actuateLightbar(deviceId, color->r, color->g, color->b);
    }

    void onPlayoutMessage(const QString& id, const net::SatelliteClient::SpeakerAudioMessage& sm,
                          source::audio::PlayoutLane lane) {
        speakerEngine_.deliver(id.toStdString(), sm.controllerIndex, lane, sm.seq, sm.opus,
                               sm.opusLen);
    }

    QString boundSlotForConnection(const QString& connectionId) const {
        const auto bindings = hub_->bindings();
        for (auto it = bindings.cbegin(); it != bindings.cend(); ++it) {
            if (it.value() == connectionId) { return it.key(); }
        }
        return {};
    }

    void deliverRumble(const QString& slotId, std::uint16_t strong, std::uint16_t weak,
                       std::uint16_t durationMs) {
        const reducer::RumbleCommand fromHost{strong, weak, durationMs};
        const bool userRumbleOn = rumbleEnabledStore_.isEnabled(slotId);
        const auto command = reducer::rumbleTheUserAllows(fromHost, userRumbleOn);
        actuateRumble(slotId, command.strong, command.weak, command.durationMs);
    }

    void actuateRumble(const QString& slotId, std::uint16_t strong, std::uint16_t weak,
                       std::uint16_t durationMs) {
        const auto in = feedbackInputs(slotId);
        switch (reducer::resolveFeedbackTarget(in, reducer::FeedbackKind::Rumble)) {
        case reducer::FeedbackTarget::Standard:
            bridge_->applyRumble(slotId, strong, weak, durationMs);
            rumblesRouted_.fetch_add(1, std::memory_order_relaxed);
            return;
        case reducer::FeedbackTarget::DirectUsb: {
            const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
            if (vp.has_value() && usbManager_ != nullptr) {
                usbManager_->applyRumble(vp->first, vp->second, strong, weak);
                rumblesRouted_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        case reducer::FeedbackTarget::None:
            return;
        }
    }

    void actuateLightbar(const QString& slotId, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        const auto in = feedbackInputs(slotId);
        switch (reducer::resolveFeedbackTarget(in, reducer::FeedbackKind::Lightbar)) {
        case reducer::FeedbackTarget::Standard:
            bridge_->applyLightbar(slotId, r, g, b);
            lightbarsRouted_.fetch_add(1, std::memory_order_relaxed);
            return;
        case reducer::FeedbackTarget::DirectUsb: {
            const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
            if (vp.has_value() && usbManager_ != nullptr) {
                usbManager_->applyLightbar(vp->first, vp->second, r, g, b);
                lightbarsRouted_.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }
        case reducer::FeedbackTarget::None:
            return;
        }
    }

    static void applyOutputActuators(SlotHardware& hw, input::usbparse::HidParser parser) {
        hw.hasTriggerEffects = input::usbout::parserHasTriggerEffects(parser);
        hw.hasPlayerLeds = input::usbout::parserHasPlayerLeds(parser);
        hw.hasMicLed = input::usbout::parserHasMicMuteLed(parser);
    }

    static SlotHardware syntheticHardware(int vendorId, int productId) {
        const auto parser = input::usbparse::parserForDevice(vendorId, productId);
        SlotHardware hw;
        hw.usbDirect = true;
        hw.hasMotion = input::usbparse::parserHasImu(parser);
        hw.hasTouchpad = input::usbparse::parserHasTouchpad(parser);
        hw.hasRumble = input::usbparse::parserHasRumble(parser);
        hw.hasLightbar = input::usbout::parserHasLightbar(parser);
        applyOutputActuators(hw, parser);
        return hw;
    }

    SlotHardware sdlHardware(const QString& slotId) const {
        SlotHardware hw;
        for (const auto& d : bridge_->devices()) {
            if (d.id != slotId) { continue; }
            hw.hasMotion = d.motionCapable;
            hw.hasLightbar = d.hasLightbar;
            hw.hasTouchpad = d.hasTouchpad;
            hw.hasRumble = d.hasRumble;
            const auto parser = input::usbparse::parserForDevice(d.vendorId, d.productId);
            applyOutputActuators(hw, parser);
            hw.sdlEffects = parser == input::usbparse::HidParser::DualSense && d.hasLightbar;
            return hw;
        }
        return hw;
    }

    SlotHardware slotHardware(const QString& slotId) const {
        const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
        if (vp.has_value() && reducer::isPadIdentity(vp)) {
            return syntheticHardware(vp->first, vp->second);
        }
        return sdlHardware(slotId);
    }

    reducer::SlotFeedbackInputs feedbackInputs(const QString& slotId) const {
        const SlotHardware hw = slotHardware(slotId);
        reducer::SlotFeedbackInputs in;
        in.usbDirect = hw.usbDirect;
        in.padRumble = hw.hasRumble;
        in.padLightbar = hw.hasLightbar;
        in.padTriggerEffects = hw.hasTriggerEffects;
        in.padPlayerLeds = hw.hasPlayerLeds;
        in.padMicLed = hw.hasMicLed;
        in.standardEffects = hw.sdlEffects;
        if (hw.usbDirect) {
            const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString());
            in.directClaimLive = vp.has_value() && usbManager_ != nullptr &&
                                 usbManager_->isDirectClaimed(vp->first, vp->second);
        }
        const auto route = audioRouteForSlot(slotId);
        in.padMicRoute = route.microphone;
        in.padSpeakerRoute = route.speaker;
        in.padHapticRoute = route.haptics;
        return in;
    }

    audio::PadAudioRoute audioRouteForSlot(const QString& slotId) const {
        int vendorId = 0;
        int productId = 0;
        if (const auto vp = reducer::parseSyntheticSlotId(slotId.toStdString()); vp.has_value()) {
            vendorId = vp->first;
            productId = vp->second;
        } else {
            bool found = false;
            for (const auto& d : bridge_->devices()) {
                if (d.id != slotId) { continue; }
                if (d.bluetooth) { return {}; }
                vendorId = d.vendorId;
                productId = d.productId;
                found = true;
                break;
            }
            if (!found) { return {}; }
        }
        if (vendorId == 0 && productId == 0) { return {}; }
        std::lock_guard<thread::TrackedMutex> lock(audioRoutesMtx_);
        const auto it = padAudioRoutes_.find(audio::padAudioKey(vendorId, productId));
        if (it == padAudioRoutes_.end()) { return {}; }
        return it->second;
    }

    const net::ConnectionHub* hub_;
    input::SDLGamepadBridge* bridge_;
    source::usb::UsbGamepadManager* usbManager_;
    source::MotionEnabledStore& motionEnabledStore_;
    source::RumbleEnabledStore& rumbleEnabledStore_;
    FeatureSettings* featureSettings_;
    source::audio::SpeakerPlayoutEngine& speakerEngine_;

    mutable thread::TrackedMutex audioRoutesMtx_{"app.audioRoutes"};
    std::map<int, audio::PadAudioRoute> padAudioRoutes_;
    mutable thread::TrackedMutex routingMtx_{"app.routing"};
    QHash<QString, net::ConnectionHub::ReportSender> routing_;
    QHash<QString, net::ConnectionHub::MotionSender> motionRouting_;
    QHash<QString, net::ConnectionHub::BatterySender> batteryRouting_;
    QHash<QString, net::ConnectionHub::TouchpadSender> touchpadRouting_;
    // AppModel's Moonlight tables: the rig binds no slot to a host, so they stay empty, and a
    // satellite slot's report never reads them.
    QHash<QString, net::ConnectionHub::ReportSender> moonlightRouting_;
    QHash<QString, net::ConnectionHub::MotionSender> moonlightMotionRouting_;
    QHash<QString, net::ConnectionHub::BatterySender> moonlightBatteryRouting_;
    QHash<QString, net::ConnectionHub::TouchpadSender> moonlightTouchRouting_;
    std::atomic<std::uint64_t> rumblesRouted_{0};
    std::atomic<std::uint64_t> lightbarsRouted_{0};
};

// The objects AppModel owns for the input and feedback paths, wired as AppModel wires them, over
// in-memory settings, the satellite's REST API on a loopback TLS listener and one loopback
// satellite per session.
class HotPathRig {
  public:
    using HandlerFor = std::function<LoopbackSatellite::Handler(int session)>;

    HotPathRig(int sessions, const HandlerFor& handlerFor)
        : settings_(makeSharedSettings()), sessionKey_(derivedSessionKey()), motionRepo_(settings_),
          rumbleRepo_(settings_), motionEnabledStore_(&motionRepo_),
          rumbleEnabledStore_(&rumbleRepo_), featureSettings_(settingsFile(settings_)),
          speakerEngine_(&audioGateway_), bridge_(&processor_),
          usbManager_(&usbGateway_, &processor_, nullptr, nullptr),
          store_(std::make_unique<net::ConnectionStore>(settingsFile(settings_))),
          manager_(std::make_unique<net::WifiConnectionManager>(store_.get())),
          hub_(std::make_unique<net::ConnectionHub>(manager_.get(), store_.get())),
          glue_(hub_.get(), &bridge_, &usbManager_, &motionEnabledStore_, &rumbleEnabledStore_,
                &featureSettings_, &speakerEngine_) {
        rest_.respond = answerRest;
        for (int i = 0; i < sessions; ++i) {
            satellites_.push_back(std::make_unique<LoopbackSatellite>(sessionKey_, handlerFor(i)));
            models::DiscoveredServer s;
            s.machineId = QStringLiteral("hotpath-%1").arg(i);
            s.ip = QStringLiteral("127.0.0.1");
            s.name = QStringLiteral("Hot path %1").arg(i);
            s.udpPort = satellites_.back()->port();
            s.pairPort = rest_.port();
            s.httpPort = rest_.port();
            store_->setSharedKey(
                QString(static_cast<qsizetype>(wire::kCryptoKeySize * 2), QLatin1Char('1')),
                net::WifiConnection::idFor(s));
            servers_.push_back(s);
        }
        glue_.installSenders(processor_);
    }

    // The input thread stops first and the manager next, as in ~AppModel: no SDL report reaches
    // the glue while it goes, and the sessions' receive threads read the hub.
    ~HotPathRig() {
        bridge_.stop();
        glue_.republishRouting({});
        manager_.reset();
        hub_.reset();
    }

    HotPathRig(const HotPathRig&) = delete;
    HotPathRig& operator=(const HotPathRig&) = delete;
    HotPathRig(HotPathRig&&) = delete;
    HotPathRig& operator=(HotPathRig&&) = delete;

    // Connects every session through the manager's real session PUT and waits until each is live
    // with the rig's feedback handlers installed.
    bool connectAll() {
        for (const auto& s : servers_) {
            manager_->connectTo(s, net::ConnectIntent::UserInitiated);
        }
        const bool live = pumpUntil([this] {
            for (int i = 0; i < sessionCount(); ++i) {
                const auto* conn = connection(i);
                if (conn == nullptr || conn->state() != net::SessionState::Live) { return false; }
            }
            return true;
        });
        if (!live) { return false; }
        for (int i = 0; i < sessionCount(); ++i) {
            glue_.installFeedbackHandlers(*connection(i), connectionId(i));
        }
        return true;
    }

    // Binds a slot through the hub and waits for the satellite to have applied it.
    bool bind(const QString& slotId, int session) {
        hub_->bind(slotId, connectionId(session));
        return pumpUntil([this, session] { return connection(session)->registeredBitmap() == 1; });
    }

    void unbind(const QString& slotId) { hub_->unbind(slotId); }

    // SDL_TicksInit marks the tick counter started before it reads the frequency, so two threads'
    // first SDL calls can divide by zero: it starts here, before the SDL thread exists.
    bool startBridge() {
        SDL_GetTicks64();
        bridge_.start();
        return pumpUntil([] { return SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0; });
    }

    bool claimDualSense() {
        usbManager_.reconcile();
        return usbGateway_.claimed() &&
               usbManager_.isDirectClaimed(FakeDualSenseGateway::kVendorId,
                                           FakeDualSenseGateway::kProductId);
    }

    int sessionCount() const { return static_cast<int>(servers_.size()); }
    QString connectionId(int session) const {
        return net::WifiConnection::idFor(servers_[static_cast<std::size_t>(session)]);
    }
    net::WifiConnection* connection(int session) const {
        return manager_->get(connectionId(session));
    }
    LoopbackSatellite& satellite(int session) const {
        return *satellites_[static_cast<std::size_t>(session)];
    }

    input::GamepadInputProcessor& processor() { return processor_; }
    input::SDLGamepadBridge& bridge() { return bridge_; }
    AppGlue& glue() { return glue_; }
    const AppGlue& glue() const { return glue_; }
    net::ConnectionHub& hub() { return *hub_; }
    const FakeDualSenseGateway& usbGateway() const { return usbGateway_; }

  private:
    static std::unique_ptr<QSettings> settingsFile(const std::shared_ptr<QSettings>& shared) {
        return std::make_unique<QSettings>(shared->fileName(), QSettings::IniFormat);
    }

    std::shared_ptr<QSettings> settings_;
    SessionKey sessionKey_;
    repository::MotionPreferenceRepository motionRepo_;
    repository::RumblePreferenceRepository rumbleRepo_;
    source::MotionEnabledStore motionEnabledStore_;
    source::RumbleEnabledStore rumbleEnabledStore_;
    FeatureSettings featureSettings_;
    FakeAudioGateway audioGateway_;
    source::audio::SpeakerPlayoutEngine speakerEngine_;
    input::GamepadInputProcessor processor_;
    input::SDLGamepadBridge bridge_;
    FakeDualSenseGateway usbGateway_;
    source::usb::UsbGamepadManager usbManager_;
    FakePairingListener rest_;
    std::unique_ptr<net::ConnectionStore> store_;
    std::unique_ptr<net::WifiConnectionManager> manager_;
    std::unique_ptr<net::ConnectionHub> hub_;
    AppGlue glue_;
    std::vector<std::unique_ptr<LoopbackSatellite>> satellites_;
    std::vector<models::DiscoveredServer> servers_;
};

} // namespace dish::test::hotpath
