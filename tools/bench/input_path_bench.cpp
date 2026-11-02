// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "HotPathRig.h"

#include "bench_support.h"
#include "core/model/Protocol.h"
#include "core/thread/TrackedMutex.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <sodium.h>

#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <exception>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace bench = dish::bench;
namespace dt = dish::thread;
namespace hp = dish::test::hotpath;

using bench::Clock;
using DeviceState = dish::input::GamepadInputProcessor::DeviceState;
using MicSender = dish::net::ConnectionHub::MicAudioSender;

constexpr double kDefaultRateHz = 1000.0;
constexpr double kDefaultSecs = 3.0;
constexpr int kDefaultReps = 3;
constexpr double kDefaultMicHz = 50.0;
constexpr double kPadBRateHz = 250.0;
constexpr double kRumbleHz = 100.0;
constexpr double kLightbarHz = 10.0;
constexpr double kPumpHz = 200.0;
constexpr double kRepublishHz = 10.0;
constexpr double kDevicesHz = 10.0;
constexpr double kInspectHz = 30.0;
constexpr double kCountersHz = 1.0;
constexpr double kToggleHz = 5.0;
constexpr double kSmokeSecs = 0.2;
constexpr double kWarmupMaxSecs = 1.0;
constexpr std::int64_t kStartDelayNs = 50 * bench::kNanosPerMilli;
constexpr std::int64_t kSettleNs = 100 * bench::kNanosPerMilli;
constexpr int kSetupTimeoutMs = 15'000;
constexpr std::size_t kSessions = 4;
constexpr int kPadASession = 0;
constexpr int kPadBSession = 1;
constexpr int kDirectSession = 2;
constexpr int kToggleSession = 3;
constexpr std::size_t kSlots = 4096;
constexpr std::size_t kMsgTypes = 0x20;
constexpr std::size_t kInputPayloadBytes = 13;
constexpr std::size_t kRxOffset = 9;
constexpr std::size_t kMicBytes = 120;
constexpr std::uint8_t kOpusFill = 0xF8;
constexpr std::uint32_t kRumbleBase = 0x4000;
constexpr std::uint32_t kRumbleSteps = 64;
constexpr std::int16_t kRestingLy = -9000;
constexpr std::int16_t kRestingRy = 12000;
constexpr std::uint8_t kRestingTrigger = 40;
constexpr std::uint16_t kRestingButtons = 0x1000;
constexpr int kExitHarness = 1;
constexpr int kExitUsage = 2;
constexpr int kMicroDecimals = 1;
constexpr int kMilliDecimals = 3;
constexpr int kRatioDecimals = 2;

std::int64_t nanosOf(Clock::time_point t) {
    return std::chrono::duration_cast<bench::Nanos>(t.time_since_epoch()).count();
}

std::int64_t nowNs() { return nanosOf(Clock::now()); }

// The spin-wait hint: x86's pause, which Windows spells YieldProcessor; elsewhere a yield.
void spinPause() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

// Sleeps to kTimerLead before the deadline on CLOCK_MONOTONIC, the clock steady_clock reads here,
// then spins.
void waitUntil(std::int64_t dueNs) {
    const std::int64_t wakeNs = dueNs - bench::kTimerLead.count();
    if (wakeNs > nowNs()) {
        timespec wake{};
        wake.tv_sec = static_cast<time_t>(wakeNs / bench::kNanosPerSecond);
        wake.tv_nsec = static_cast<long>(wakeNs % bench::kNanosPerSecond);
        while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, nullptr) == EINTR) {}
    }
    while (nowNs() < dueNs) { spinPause(); }
}

enum class Mode : std::uint8_t { Synthetic, Vigem };

struct Options {
    Mode mode = Mode::Synthetic;
    double rateHz = kDefaultRateHz;
    double secs = kDefaultSecs;
    int reps = kDefaultReps;
    double micHz = kDefaultMicHz;
    std::string json;
    std::string csv;
    std::vector<std::string> scenarios;
    bool smoke = false;
    bool help = false;
};

const char* modeName(Mode mode) { return mode == Mode::Vigem ? "vigem" : "synthetic"; }

void printUsage(std::ostream& out) {
    out << "usage: bench_input_path [options]\n"
           "  --mode synthetic|vigem  synthetic (default) publishes into the processor; vigem\n"
           "                          is Windows-only and reports that it is not available here\n"
           "  --rate <hz>             pad A and the DualSense (default 1000)\n"
           "  --secs <s>              seconds per run (default 3)\n"
           "  --reps <n>              interleaved repetitions after a discarded warm-up; the rep\n"
           "                          whose p99.9 is the median is reported (default 3)\n"
           "  --mic-hz <hz>           S5's mic frames per second (default 50)\n"
           "  --scenarios <list>      comma-separated subset of S1..S7 (default all)\n"
           "  --json <path>           also write the results, settings and machine as JSON\n"
           "  --csv <path>            also write the result rows as CSV\n"
           "  --smoke                 every scenario for 0.2 s, one rep, no warm-up; the exit "
           "code\n"
           "                          is the harness's own accounting, never a latency\n"
           "  --help                  this text\n"
           "Every producer paces to absolute deadlines: clock_nanosleep to 300 us before each\n"
           "deadline, then a spin.\n";
}

template <typename T> bool parseNumber(const std::string& text, T& out) {
    const char* first = text.c_str();
    const char* last = first + text.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

std::vector<std::string> splitList(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t stop = comma == std::string_view::npos ? text.size() : comma;
        if (stop > start) { out.emplace_back(text.substr(start, stop - start)); }
        if (comma == std::string_view::npos) { break; }
        start = comma + 1;
    }
    return out;
}

// Applies one flag; false on an unknown flag, a missing value or a value out of range.
bool applyFlag(Options& o, std::string_view flag, const std::function<const char*()>& value) {
    if (flag == "--help") {
        o.help = true;
        return true;
    }
    if (flag == "--smoke") {
        o.smoke = true;
        return true;
    }
    const char* v = value();
    if (v == nullptr) { return false; }
    const std::string_view text(v);
    if (flag == "--mode") {
        if (text != "synthetic" && text != "vigem") { return false; }
        o.mode = text == "vigem" ? Mode::Vigem : Mode::Synthetic;
        return true;
    }
    if (flag == "--rate") { return parseNumber(std::string(text), o.rateHz) && o.rateHz > 0.0; }
    if (flag == "--secs") { return parseNumber(std::string(text), o.secs) && o.secs > 0.0; }
    if (flag == "--reps") { return parseNumber(std::string(text), o.reps) && o.reps > 0; }
    if (flag == "--mic-hz") { return parseNumber(std::string(text), o.micHz) && o.micHz > 0.0; }
    if (flag == "--scenarios") {
        o.scenarios = splitList(text);
        return !o.scenarios.empty();
    }
    if (flag == "--json") {
        o.json = std::string(text);
        return true;
    }
    if (flag == "--csv") {
        o.csv = std::string(text);
        return true;
    }
    return false;
}

std::optional<Options> parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag(argv[i]);
        const auto value = [&i, argc, argv]() -> const char* {
            return i + 1 < argc ? argv[++i] : nullptr;
        };
        if (!applyFlag(o, flag, value)) {
            std::cerr << "bench_input_path: bad option " << flag << "\n";
            return std::nullopt;
        }
    }
    if (o.smoke) {
        o.secs = kSmokeSecs;
        o.reps = 1;
    }
    return o;
}

// One measured stream: the producer stamps each frame as it publishes and the receiver times its
// datagram against that.
class StreamTap {
  public:
    StreamTap(std::string name, double rateHz)
        : name_(std::move(name)), rateHz_(rateHz), firstArrival_(kSlots, 0) {}

    const std::string& name() const { return name_; }
    double rateHz() const { return rateHz_; }

    void reset(std::size_t expectedFrames) {
        origin_.clear();
        std::fill(firstArrival_.begin(), firstArrival_.end(), 0U);
        for (std::vector<std::int64_t>* v : {&lateness_, &latency_, &arrivals_}) {
            v->clear();
            v->reserve(expectedFrames);
        }
        sent_ = 0;
        duplicates_ = 0;
    }

    void published(std::uint32_t sequence, std::int64_t stampNs, std::int64_t latenessNs) {
        origin_.stamp(sequence, stampNs);
        lateness_.push_back(latenessNs);
        ++sent_;
    }

    void arrived(std::uint32_t sequence, std::int64_t arrivedNs) {
        std::uint32_t& seen = firstArrival_[sequence % kSlots];
        if (seen == sequence + 1) {
            ++duplicates_;
            return;
        }
        seen = sequence + 1;
        arrivals_.push_back(arrivedNs);
        if (const auto origin = origin_.take(sequence)) { latency_.push_back(arrivedNs - *origin); }
    }

    std::uint64_t sent() const { return sent_; }
    std::uint64_t duplicates() const { return duplicates_; }
    const std::vector<std::int64_t>& lateness() const { return lateness_; }
    const std::vector<std::int64_t>& latency() const { return latency_; }
    const std::vector<std::int64_t>& arrivals() const { return arrivals_; }

  private:
    const std::string name_;
    const double rateHz_;
    bench::SequenceStamps<kSlots> origin_;
    std::vector<std::uint32_t> firstArrival_;
    std::vector<std::int64_t> lateness_;
    std::vector<std::int64_t> latency_;
    std::vector<std::int64_t> arrivals_;
    std::uint64_t sent_ = 0;
    std::uint64_t duplicates_ = 0;
};

// What one session's loopback satellite records while a window is open: every datagram by type,
// and the timing of the stream whose reports the session carries.
class SessionTap {
  public:
    void onDatagram(std::uint16_t msgType, const std::uint8_t* body, std::size_t len,
                    Clock::time_point arrived) {
        if (!gate_.enter()) { return; }
        if (msgType < kMsgTypes) { ++datagrams_[msgType]; }
        if (msgType == dish::proto::kMsgInput && stream_ != nullptr && len >= kInputPayloadBytes) {
            const auto low = static_cast<std::uint16_t>(body[kRxOffset]);
            const auto high = static_cast<std::uint16_t>(body[kRxOffset + 1]);
            const auto rx =
                static_cast<std::int16_t>(static_cast<std::uint16_t>(low | (high << 8)));
            if (const auto sequence = bench::decodeSequence(rx)) {
                stream_->arrived(*sequence, nanosOf(arrived));
            }
        }
        gate_.leave();
    }

    void arm(StreamTap* stream) {
        datagrams_.fill(0);
        stream_ = stream;
    }

    void open() { gate_.open(); }
    void close() { gate_.close(); }
    std::uint64_t count(std::uint16_t msgType) const { return datagrams_[msgType]; }

    std::uint64_t reportDatagrams() const {
        return count(dish::proto::kMsgInput) + count(dish::proto::kMsgMotion) +
               count(dish::proto::kMsgTouchpad) + count(dish::proto::kMsgBattery);
    }

  private:
    bench::RecordingGate gate_;
    std::array<std::uint64_t, kMsgTypes> datagrams_{};
    StreamTap* stream_ = nullptr;
};

struct StreamResult {
    std::string producer;
    double rateHz = 0.0;
    std::uint64_t sent = 0;
    bench::LatencySummary latency;
    std::int64_t longestGapNs = 0;
    double datagramsPerReport = 0.0;
    std::uint64_t drops = 0;
    std::uint64_t duplicates = 0;
    bench::PacingVerdict pacing;
};

struct Extras {
    std::uint64_t rumblesSent = 0;
    std::uint64_t lightbarsSent = 0;
    std::uint64_t feedbackRouted = 0;
    std::uint64_t micSent = 0;
    std::uint64_t micReceived = 0;
    std::uint64_t pingsReceived = 0;
    std::uint64_t toggles = 0;
    std::size_t guiReads = 0;
};

struct LockRow {
    std::string lock;
    std::string role;
    std::uint64_t acquisitions = 0;
    std::uint64_t contended = 0;
    std::uint64_t tryFailures = 0;
    double maxWaitUs = 0.0;
    double p99WaitUs = 0.0;
    double maxHoldUs = 0.0;
};

struct ScenarioRun {
    std::string scenario;
    std::vector<StreamResult> streams;
    Extras extras;
    std::vector<LockRow> locks;
    std::vector<std::string> problems;
};

#ifdef DISH_LOCK_STATS
constexpr double kP99 = 0.99;

std::vector<LockRow> lockRows(const dt::lockstats::Snapshot& snap) {
    std::vector<LockRow> rows;
    for (const dt::lockstats::LockStats& lock : snap.locks) {
        for (std::size_t r = 0; r < dt::kThreadRoleCount; ++r) {
            const dt::lockstats::RoleStats& s = lock.roles[r];
            if (!s.touched()) { continue; }
            LockRow row;
            row.lock = lock.name;
            row.role = dt::threadRoleName(static_cast<dt::ThreadRole>(r));
            row.acquisitions = s.acquisitions;
            row.contended = s.contended;
            row.tryFailures = s.tryFailures;
            row.maxWaitUs = bench::nanosToMicros(static_cast<std::int64_t>(s.waitMaxNs));
            row.p99WaitUs = bench::nanosToMicros(static_cast<std::int64_t>(
                dt::lockstats::histogramPercentileNs(s.waitHistogram, kP99)));
            row.maxHoldUs = bench::nanosToMicros(static_cast<std::int64_t>(s.holdMaxNs));
            rows.push_back(std::move(row));
        }
    }
    return rows;
}
#endif

void startLockWindow() {
#ifdef DISH_LOCK_STATS
    dt::lockstats::reset();
#endif
}

std::vector<LockRow> endLockWindow() {
#ifdef DISH_LOCK_STATS
    return lockRows(dt::lockstats::snapshot());
#else
    return {};
#endif
}

StreamResult resultOf(const StreamTap& tap, std::string producer,
                      const std::vector<std::int64_t>& latency, std::uint64_t reportDatagrams) {
    StreamResult r;
    r.producer = std::move(producer);
    r.rateHz = tap.rateHz();
    r.sent = tap.sent();
    r.latency = bench::summarize(latency);
    r.longestGapNs = bench::longestGapNs(tap.arrivals());
    r.datagramsPerReport =
        tap.sent() == 0 ? 0.0
                        : static_cast<double>(reportDatagrams) / static_cast<double>(tap.sent());
    const auto received = static_cast<std::uint64_t>(tap.arrivals().size());
    r.drops = tap.sent() > received ? tap.sent() - received : 0;
    r.duplicates = tap.duplicates();
    r.pacing = bench::judgePacing(tap.lateness());
    return r;
}

struct Scenario {
    const char* id;
    bool padB = false;
    bool gui = false;
    bool feedback = false;
    bool heartbeatAndMic = false;
    bool direct = false;
    bool toggle = false;
};

constexpr std::array kScenarios{
    Scenario{.id = "S1"},
    Scenario{.id = "S2", .padB = true},
    Scenario{.id = "S3", .padB = true, .gui = true},
    Scenario{.id = "S4", .padB = true, .gui = true, .feedback = true},
    Scenario{.id = "S5", .padB = true, .gui = true, .feedback = true, .heartbeatAndMic = true},
    Scenario{.id = "S6", .direct = true},
    Scenario{.id = "S7", .padB = true, .gui = true, .feedback = true, .toggle = true},
};

std::string scenarioLabel(const Scenario& sc, const Options& o) {
    if (sc.heartbeatAndMic && o.micHz != kDefaultMicHz) {
        std::ostringstream label;
        label << sc.id << " (mic " << o.micHz << " Hz)";
        return label.str();
    }
    return sc.id;
}

DeviceState padState(std::uint32_t sequence) {
    DeviceState st;
    st.wButtons = kRestingButtons;
    st.lt = kRestingTrigger;
    st.rx = bench::encodeSequence(sequence);
    st.ly = kRestingLy;
    st.ry = kRestingRy;
    return st;
}

// One producer stream on a paced thread: frame k at its absolute deadline, until the window ends.
struct Paced {
    bench::Cadence cadence;
    std::function<void(std::uint64_t frame, std::int64_t dueNs)> onDue;
};

void runPaced(dt::ThreadRole role, const std::vector<Paced>& streams, std::int64_t startNs,
              std::int64_t endNs) {
    const dt::ScopedThreadRole named(role);
    std::vector<bench::Cadence> cadences;
    cadences.reserve(streams.size());
    for (const Paced& s : streams) { cadences.push_back(s.cadence); }
    for (;;) {
        const std::size_t i = bench::earliest(cadences);
        const std::int64_t due = startNs + cadences[i].dueNs();
        if (due >= endNs) { break; }
        waitUntil(due);
        streams[i].onDue(cadences[i].tick(), due);
        cadences[i].advance();
    }
}

// The GUI thread with nothing else to do: Qt's events at kPumpHz, to absolute deadlines.
void pumpBetween(std::int64_t startNs, std::int64_t endNs) {
    bench::Cadence pump(kPumpHz);
    for (;;) {
        const std::int64_t due = startNs + pump.dueNs();
        if (due >= endNs) { break; }
        waitUntil(due);
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        pump.advance();
    }
}

std::size_t expectedFrames(double rateHz, double secs) {
    return static_cast<std::size_t>(rateHz * secs * 2.0) + 1024U;
}

std::int64_t windowEnd(std::int64_t start, double secs) {
    return start + static_cast<std::int64_t>(secs * static_cast<double>(bench::kNanosPerSecond));
}

bool hasDevice(const QList<dish::input::SDLGamepadBridge::Device>& devices, const QString& id) {
    return std::any_of(
        devices.begin(), devices.end(),
        [&id](const dish::input::SDLGamepadBridge::Device& d) { return d.id == id; });
}

hp::HotPathRig::HandlerFor tapHandlers(std::array<SessionTap, kSessions>& taps) {
    return [&taps](int session) {
        SessionTap* tap = &taps[static_cast<std::size_t>(session)];
        return [tap](std::uint16_t type, const std::uint8_t* body, std::size_t len,
                     Clock::time_point arrived) { tap->onDatagram(type, body, len, arrived); };
    };
}

struct GuiTally {
    std::uint64_t toggles = 0;
    std::size_t reads = 0;
};

class SyntheticBench {
  public:
    explicit SyntheticBench(const Options& options)
        : options_(options), rig_(static_cast<int>(kSessions), tapHandlers(taps_)),
          padA_("A", options.rateHz), padB_("B", kPadBRateHz),
          direct_("DualSense", options.rateHz) {}

    SyntheticBench(const SyntheticBench&) = delete;
    SyntheticBench& operator=(const SyntheticBench&) = delete;
    SyntheticBench(SyntheticBench&&) = delete;
    SyntheticBench& operator=(SyntheticBench&&) = delete;

    // The virtual pads leave while SDL is still up, before the rig stops the bridge.
    ~SyntheticBench() {
        virtualC_.reset();
        virtualB_.reset();
        virtualA_.reset();
    }

    // Brings every session live with its pad bound; empty on success, else what failed.
    std::string setUp() {
        if (!rig_.startBridge()) { return "the SDL thread never initialised SDL"; }
        virtualA_.emplace("Bench pad A");
        virtualB_.emplace("Bench pad B");
        virtualC_.emplace("Bench pad C");
        slotA_ = virtualA_->deviceId();
        slotB_ = virtualB_->deviceId();
        slotC_ = virtualC_->deviceId();
        const bool opened = hp::pumpUntil(
            [this] {
                const auto devices = rig_.bridge().devices();
                return hasDevice(devices, slotA_) && hasDevice(devices, slotB_) &&
                       hasDevice(devices, slotC_);
            },
            kSetupTimeoutMs);
        if (!opened) { return "the bridge never opened the virtual pads"; }
        if (!rig_.claimDualSense()) { return "the USB manager never claimed the DualSense"; }
        if (!rig_.connectAll()) { return "a session never went live"; }
        if (!rig_.bind(slotA_, kPadASession) || !rig_.bind(slotB_, kPadBSession) ||
            !rig_.bind(hp::FakeDualSenseGateway::slotId(), kDirectSession)) {
            return "a slot was never applied";
        }
        for (int s = 0; s < rig_.sessionCount(); ++s) {
            rig_.connection(s)->client()->stopHeartbeat();
        }
        mic_ = rig_.hub().micAudioSenderForSlot(slotA_);
        if (!mic_) { return "pad A has no mic sender"; }
        republish();
        return {};
    }

    ScenarioRun run(const Scenario& sc, double secs) {
        padA_.reset(expectedFrames(options_.rateHz, secs));
        padB_.reset(expectedFrames(kPadBRateHz, secs));
        direct_.reset(expectedFrames(options_.rateHz, secs));
        tap(kPadASession).arm(sc.direct ? nullptr : &padA_);
        tap(kPadBSession).arm(sc.padB ? &padB_ : nullptr);
        tap(kDirectSession).arm(sc.direct ? &direct_ : nullptr);
        tap(kToggleSession).arm(nullptr);
        if (sc.heartbeatAndMic) { rig_.connection(kPadASession)->client()->startHeartbeat(); }
        const std::uint64_t routedBefore = routed();
        std::atomic<std::uint64_t> rumblesSent{0};
        std::atomic<std::uint64_t> lightbarsSent{0};
        std::atomic<std::uint64_t> micSent{0};

        startLockWindow();
        for (SessionTap& t : taps_) { t.open(); }
        const std::int64_t start = nowNs() + kStartDelayNs;
        const std::int64_t end = windowEnd(start, secs);
        std::vector<std::thread> producers;
        if (sc.direct) {
            producers.push_back(usbReaderProducer(start, end));
        } else {
            producers.push_back(sdlInputProducer(sc, start, end));
        }
        if (sc.feedback) {
            producers.push_back(feedbackProducer(rumblesSent, lightbarsSent, start, end));
        }
        if (sc.heartbeatAndMic) { producers.push_back(micProducer(micSent, start, end)); }
        const GuiTally gui = guiLoop(sc, start, end);
        for (std::thread& t : producers) { t.join(); }
        pumpBetween(end, end + kSettleNs);
        for (SessionTap& t : taps_) { t.close(); }

        ScenarioRun out;
        out.scenario = scenarioLabel(sc, options_);
        out.locks = endLockWindow();
        if (sc.heartbeatAndMic) { rig_.connection(kPadASession)->client()->stopHeartbeat(); }
        if (toggleBound_) { toggle(); }
        if (sc.direct) {
            out.streams.push_back(resultOf(direct_, direct_.name(), direct_.latency(),
                                           tap(kDirectSession).reportDatagrams()));
        } else {
            out.streams.push_back(resultOf(padA_, padA_.name(), padA_.latency(),
                                           tap(kPadASession).reportDatagrams()));
            if (sc.padB) {
                out.streams.push_back(resultOf(padB_, padB_.name(), padB_.latency(),
                                               tap(kPadBSession).reportDatagrams()));
            }
        }
        out.extras.rumblesSent = rumblesSent.load();
        out.extras.lightbarsSent = lightbarsSent.load();
        out.extras.feedbackRouted = routed() - routedBefore;
        out.extras.micSent = micSent.load();
        out.extras.micReceived = tap(kPadASession).count(dish::proto::kMsgMicAudio);
        out.extras.pingsReceived = tap(kPadASession).count(dish::proto::kMsgHeartbeat);
        out.extras.toggles = gui.toggles;
        out.extras.guiReads = gui.reads;
        out.problems = accountingProblems(sc, out);
        return out;
    }

  private:
    SessionTap& tap(int session) { return taps_[static_cast<std::size_t>(session)]; }
    const SessionTap& tap(int session) const { return taps_[static_cast<std::size_t>(session)]; }

    std::uint64_t routed() const {
        return rig_.glue().rumblesRouted() + rig_.glue().lightbarsRouted();
    }

    std::thread sdlInputProducer(const Scenario& sc, std::int64_t start, std::int64_t end) {
        std::vector<Paced> streams;
        streams.push_back(padStream(padA_, slotA_.toStdString()));
        if (sc.padB) { streams.push_back(padStream(padB_, slotB_.toStdString())); }
        return std::thread(runPaced, dt::ThreadRole::SdlInput, std::move(streams), start, end);
    }

    Paced padStream(StreamTap& stream, std::string deviceId) {
        dish::input::GamepadInputProcessor* processor = &rig_.processor();
        return Paced{bench::Cadence(stream.rateHz()),
                     [&stream, processor, id = std::move(deviceId)](std::uint64_t frame,
                                                                    std::int64_t dueNs) {
                         const std::uint32_t sequence = bench::sequenceOf(frame);
                         const std::int64_t stamp = nowNs();
                         stream.published(sequence, stamp, stamp - dueNs);
                         processor->publish(id, padState(sequence));
                     }};
    }

    std::thread usbReaderProducer(std::int64_t start, std::int64_t end) {
        const hp::FakeDualSenseGateway* gateway = &rig_.usbGateway();
        StreamTap* stream = &direct_;
        std::vector<Paced> streams;
        streams.push_back(Paced{bench::Cadence(direct_.rateHz()),
                                [gateway, stream](std::uint64_t frame, std::int64_t dueNs) {
                                    const std::uint32_t sequence = bench::sequenceOf(frame);
                                    const std::int64_t stamp = nowNs();
                                    stream->published(sequence, stamp, stamp - dueNs);
                                    gateway->deliver(
                                        hp::dualSenseReport(bench::encodeSequence(sequence)));
                                }});
        return std::thread(runPaced, dt::ThreadRole::UsbReader, std::move(streams), start, end);
    }

    // The satellite's side of S4: rumble at 100 Hz and a light-bar colour at 10 Hz to pad A's
    // session. The strength moves each time, as a game's does, so SDL never drops one as a repeat.
    std::thread feedbackProducer(std::atomic<std::uint64_t>& rumbles,
                                 std::atomic<std::uint64_t>& lightbars, std::int64_t start,
                                 std::int64_t end) {
        hp::LoopbackSatellite* satellite = &rig_.satellite(kPadASession);
        std::vector<Paced> streams;
        streams.push_back(Paced{
            bench::Cadence(kRumbleHz), [satellite, &rumbles](std::uint64_t frame, std::int64_t) {
                std::array<std::uint8_t, 7> body{0, 0, 0, 0x20, 0, 0, 0x64};
                hp::writeBe16(body.data() + 1,
                              static_cast<std::uint16_t>(kRumbleBase + (frame % kRumbleSteps)));
                if (satellite->sendToDish(dish::proto::kMsgRumble, body.data(), body.size())) {
                    rumbles.fetch_add(1);
                }
            }});
        streams.push_back(Paced{bench::Cadence(kLightbarHz),
                                [satellite, &lightbars](std::uint64_t frame, std::int64_t) {
                                    const std::array<std::uint8_t, 4> body{
                                        0, static_cast<std::uint8_t>(frame), 0x10, 0x80};
                                    if (satellite->sendToDish(dish::proto::kMsgLightbar,
                                                              body.data(), body.size())) {
                                        lightbars.fetch_add(1);
                                    }
                                }});
        return std::thread(runPaced, dt::ThreadRole::Other, std::move(streams), start, end);
    }

    std::thread micProducer(std::atomic<std::uint64_t>& sent, std::int64_t start,
                            std::int64_t end) {
        const MicSender* mic = &mic_;
        std::vector<Paced> streams;
        streams.push_back(
            Paced{bench::Cadence(options_.micHz), [mic, &sent](std::uint64_t frame, std::int64_t) {
                      std::array<std::uint8_t, kMicBytes> opus{};
                      opus.fill(kOpusFill);
                      if ((*mic)(static_cast<std::uint16_t>(frame), opus.data(), opus.size())) {
                          sent.fetch_add(1);
                      }
                  }});
        return std::thread(runPaced, dt::ThreadRole::AudioCapture, std::move(streams), start, end);
    }

    // The GUI thread, this one: it always runs Qt's loop, as the app's does, and from S3 on it
    // does what the GUI does while a pad plays; S7 also binds and unbinds pad C every 200 ms.
    GuiTally guiLoop(const Scenario& sc, std::int64_t start, std::int64_t end) {
        enum Task : std::uint8_t { Pump, Republish, Devices, Inspect, Counters, Toggle };
        std::vector<bench::Cadence> cadences{
            bench::Cadence(kPumpHz),    bench::Cadence(kRepublishHz), bench::Cadence(kDevicesHz),
            bench::Cadence(kInspectHz), bench::Cadence(kCountersHz),  bench::Cadence(kToggleHz)};
        GuiTally tally;
        for (;;) {
            const std::size_t task = bench::earliest(cadences);
            const std::int64_t due = start + cadences[task].dueNs();
            if (due >= end) { break; }
            waitUntil(due);
            cadences[task].advance();
            if (task == Pump) {
                QCoreApplication::processEvents(QEventLoop::AllEvents);
                continue;
            }
            if (!sc.gui) { continue; }
            if (task == Republish) {
                republish();
            } else if (task == Devices) {
                tally.reads += static_cast<std::size_t>(rig_.bridge().devices().size());
            } else if (task == Inspect) {
                if (rig_.processor().inspect(slotA_.toStdString()).wire.has_value()) {
                    ++tally.reads;
                }
            } else if (task == Counters) {
                for (const QString& slot : boundSlots()) {
                    if (rig_.processor().inputCounters(slot.toStdString()).gamepadEvents > 0) {
                        ++tally.reads;
                    }
                }
            } else if (task == Toggle && sc.toggle) {
                toggle();
                ++tally.toggles;
            }
        }
        return tally;
    }

    void toggle() {
        if (toggleBound_) {
            rig_.unbind(slotC_);
        } else {
            rig_.hub().bind(slotC_, rig_.connectionId(kToggleSession));
        }
        toggleBound_ = !toggleBound_;
        republish();
    }

    QStringList boundSlots() const {
        QStringList slotIds{slotA_, slotB_, hp::FakeDualSenseGateway::slotId()};
        if (toggleBound_) { slotIds.append(slotC_); }
        return slotIds;
    }

    void republish() { rig_.glue().republishRouting(boundSlots()); }

    std::vector<std::string> accountingProblems(const Scenario& sc, const ScenarioRun& run) const {
        std::vector<std::string> problems;
        const auto note = [&problems, &run](const std::string& what) {
            problems.push_back(run.scenario + ": " + what);
        };
        for (const StreamResult& s : run.streams) {
            if (s.sent == 0) { note(s.producer + " sent nothing"); }
            if (s.latency.frames == 0) { note(s.producer + "'s reports never arrived"); }
            if (options_.smoke && !s.pacing.ok) { note(s.producer + " missed its deadlines"); }
        }
        if (sc.gui && run.extras.guiReads == 0) { note("the GUI reads never ran"); }
        if (sc.feedback && run.extras.feedbackRouted == 0) {
            note("no rumble or light-bar reached the SDL queue");
        }
        if (sc.heartbeatAndMic && run.extras.micReceived == 0) { note("no mic frame arrived"); }
        if (sc.direct && (tap(kDirectSession).count(dish::proto::kMsgMotion) == 0 ||
                          tap(kDirectSession).count(dish::proto::kMsgTouchpad) == 0)) {
            note("the DualSense's motion or touchpad never arrived");
        }
        if (sc.toggle && run.extras.toggles == 0) { note("no binding was toggled"); }
        for (int s = 0; s < rig_.sessionCount(); ++s) {
            if (rig_.satellite(s).rejected() != 0) {
                note("a satellite could not read a datagram");
            }
        }
        return problems;
    }

    const Options& options_;
    std::array<SessionTap, kSessions> taps_;
    hp::HotPathRig rig_;
    std::optional<hp::VirtualPad> virtualA_;
    std::optional<hp::VirtualPad> virtualB_;
    std::optional<hp::VirtualPad> virtualC_;
    QString slotA_;
    QString slotB_;
    QString slotC_;
    StreamTap padA_;
    StreamTap padB_;
    StreamTap direct_;
    MicSender mic_;
    bool toggleBound_ = false;
};

struct Report {
    std::vector<ScenarioRun> runs;
    std::vector<std::string> problems;
    std::vector<std::string> notes;
    bool skipped = false;
};

void collect(Report& report, const std::vector<ScenarioRun>& reps) {
    std::vector<std::int64_t> p999s;
    p999s.reserve(reps.size());
    for (const ScenarioRun& run : reps) {
        p999s.push_back(run.streams.empty() ? 0 : run.streams.front().latency.p999Ns);
        report.problems.insert(report.problems.end(), run.problems.begin(), run.problems.end());
    }
    if (!reps.empty()) { report.runs.push_back(reps[bench::medianRep(p999s)]); }
}

Report runSynthetic(const Options& o) {
    Report report;
    SyntheticBench bench(o);
    if (const std::string failed = bench.setUp(); !failed.empty()) {
        report.problems.push_back("setup: " + failed);
        return report;
    }
    std::vector<const Scenario*> chosen;
    for (const Scenario& sc : kScenarios) {
        if (o.scenarios.empty() ||
            std::find(o.scenarios.begin(), o.scenarios.end(), sc.id) != o.scenarios.end()) {
            chosen.push_back(&sc);
        }
    }
    if (chosen.empty()) {
        report.problems.push_back("no scenario matched --scenarios");
        return report;
    }
    if (!o.smoke) {
        for (const Scenario* sc : chosen) { bench.run(*sc, std::min(o.secs, kWarmupMaxSecs)); }
    }
    std::vector<std::vector<ScenarioRun>> reps(chosen.size());
    for (int rep = 0; rep < o.reps; ++rep) {
        for (std::size_t i = 0; i < chosen.size(); ++i) {
            reps[i].push_back(bench.run(*chosen[i], o.secs));
        }
    }
    for (const auto& scenarioReps : reps) { collect(report, scenarioReps); }
    return report;
}

// ViGEm is a Windows bus driver. The mode stays, so the same commands run on both systems.
Report runVigem() {
    Report report;
    report.skipped = true;
    report.notes.push_back("vigem mode skipped: not available on this platform");
    return report;
}

// The value of the first `key: value` line of /proc/cpuinfo that names `key`.
std::string cpuName() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (!line.starts_with("model name")) { continue; }
        const std::size_t value = line.find_first_not_of(" \t", line.find(':') + 1);
        return value == std::string::npos ? "unknown" : line.substr(value);
    }
    return "unknown";
}

// The distribution's own name for itself, from os-release.
std::string osName() {
    std::ifstream osRelease("/etc/os-release");
    const std::string key = "PRETTY_NAME=";
    std::string line;
    while (std::getline(osRelease, line)) {
        if (!line.starts_with(key)) { continue; }
        std::string value = line.substr(key.size());
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        return value;
    }
    return "unknown";
}

std::string kernelRelease() {
    utsname name{};
    if (::uname(&name) != 0) { return "unknown"; }
    return std::string(name.sysname) + " " + name.release;
}

const char* buildType() {
#ifdef NDEBUG
    return "Release";
#else
    return "Debug";
#endif
}

bool lockStatsBuild() {
#ifdef DISH_LOCK_STATS
    return true;
#else
    return false;
#endif
}

std::string fixed(double value, int decimals) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
}

std::vector<std::string> rowCells(const Options& o, const ScenarioRun& run, const StreamResult& s) {
    return {modeName(o.mode),
            run.scenario,
            s.producer,
            fixed(s.rateHz, 0),
            std::to_string(s.latency.frames),
            fixed(bench::nanosToMicros(s.latency.p50Ns), kMicroDecimals),
            fixed(bench::nanosToMicros(s.latency.p99Ns), kMicroDecimals),
            fixed(bench::nanosToMicros(s.latency.p999Ns), kMicroDecimals),
            fixed(bench::nanosToMicros(s.latency.maxNs), kMicroDecimals),
            std::to_string(s.latency.overOneMs),
            fixed(bench::nanosToMillis(s.longestGapNs), kMilliDecimals),
            fixed(s.datagramsPerReport, kRatioDecimals),
            std::to_string(s.drops)};
}

void printTable(std::ostream& out, const std::vector<std::string>& headers,
                const std::vector<std::vector<std::string>>& rows, std::size_t textColumns) {
    out << "|";
    for (const std::string& h : headers) { out << " " << h << " |"; }
    out << "\n|";
    for (std::size_t i = 0; i < headers.size(); ++i) {
        out << (i < textColumns ? " --- |" : " ---: |");
    }
    out << "\n";
    for (const auto& row : rows) {
        out << "|";
        for (const std::string& cell : row) { out << " " << cell << " |"; }
        out << "\n";
    }
}

std::vector<std::string> rowHeaders() {
    return {"mode",           "scenario",      "producer", "rate Hz", "frames",
            "p50 us",         "p99 us",        "p99.9 us", "max us",  "over 1 ms",
            "longest gap ms", "dgrams/report", "drops"};
}

void printReport(std::ostream& out, const Options& o, const Report& report) {
    std::vector<std::vector<std::string>> rows;
    for (const ScenarioRun& run : report.runs) {
        for (const StreamResult& s : run.streams) { rows.push_back(rowCells(o, run, s)); }
    }
    out << "\n";
    printTable(out, rowHeaders(), rows, 3);
    std::vector<std::vector<std::string>> extras;
    for (const ScenarioRun& run : report.runs) {
        const Extras& e = run.extras;
        extras.push_back({run.scenario, std::to_string(e.rumblesSent),
                          std::to_string(e.lightbarsSent), std::to_string(e.feedbackRouted),
                          std::to_string(e.micSent), std::to_string(e.micReceived),
                          std::to_string(e.pingsReceived), std::to_string(e.toggles)});
    }
    out << "\n";
    printTable(out,
               {"scenario", "rumbles sent", "light-bars sent", "queued for SDL", "mic sent",
                "mic received", "pings", "toggles"},
               extras, 1);
    if (lockStatsBuild()) {
        std::vector<std::vector<std::string>> locks;
        for (const ScenarioRun& run : report.runs) {
            for (const LockRow& l : run.locks) {
                locks.push_back({modeName(o.mode), run.scenario, l.lock, l.role,
                                 std::to_string(l.acquisitions), std::to_string(l.contended),
                                 std::to_string(l.tryFailures), fixed(l.maxWaitUs, kMicroDecimals),
                                 fixed(l.p99WaitUs, kMicroDecimals),
                                 fixed(l.maxHoldUs, kMicroDecimals)});
            }
        }
        out << "\n";
        printTable(out,
                   {"mode", "scenario", "lock", "role", "acquisitions", "contended", "try fails",
                    "max wait us", "p99 wait us", "max hold us"},
                   locks, 4);
    }
    for (const std::string& note : report.notes) { out << "\nnote: " << note << "\n"; }
    for (const std::string& problem : report.problems) { out << "PROBLEM: " << problem << "\n"; }
}

QJsonArray toJsonArray(const std::vector<std::string>& values) {
    QJsonArray out;
    for (const std::string& v : values) { out.append(QString::fromStdString(v)); }
    return out;
}

QJsonObject toJson(const Options& o, const Report& report) {
    QJsonArray rows;
    QJsonArray extras;
    QJsonArray locks;
    for (const ScenarioRun& run : report.runs) {
        const QString scenario = QString::fromStdString(run.scenario);
        for (const StreamResult& s : run.streams) {
            rows.append(QJsonObject{
                {"mode", modeName(o.mode)},
                {"scenario", scenario},
                {"producer", QString::fromStdString(s.producer)},
                {"rateHz", s.rateHz},
                {"sent", static_cast<qint64>(s.sent)},
                {"frames", static_cast<qint64>(s.latency.frames)},
                {"p50Us", bench::nanosToMicros(s.latency.p50Ns)},
                {"p99Us", bench::nanosToMicros(s.latency.p99Ns)},
                {"p999Us", bench::nanosToMicros(s.latency.p999Ns)},
                {"maxUs", bench::nanosToMicros(s.latency.maxNs)},
                {"overOneMs", static_cast<qint64>(s.latency.overOneMs)},
                {"longestGapMs", bench::nanosToMillis(s.longestGapNs)},
                {"datagramsPerReport", s.datagramsPerReport},
                {"drops", static_cast<qint64>(s.drops)},
                {"duplicates", static_cast<qint64>(s.duplicates)},
                {"pacingEarly", static_cast<qint64>(s.pacing.early)},
                {"pacingMedianLatenessUs", bench::nanosToMicros(s.pacing.medianLatenessNs)}});
        }
        const Extras& e = run.extras;
        extras.append(QJsonObject{{"scenario", scenario},
                                  {"rumblesSent", static_cast<qint64>(e.rumblesSent)},
                                  {"lightbarsSent", static_cast<qint64>(e.lightbarsSent)},
                                  {"queuedForSdl", static_cast<qint64>(e.feedbackRouted)},
                                  {"micSent", static_cast<qint64>(e.micSent)},
                                  {"micReceived", static_cast<qint64>(e.micReceived)},
                                  {"pingsReceived", static_cast<qint64>(e.pingsReceived)},
                                  {"toggles", static_cast<qint64>(e.toggles)}});
        for (const LockRow& l : run.locks) {
            locks.append(QJsonObject{{"scenario", scenario},
                                     {"lock", QString::fromStdString(l.lock)},
                                     {"role", QString::fromStdString(l.role)},
                                     {"acquisitions", static_cast<qint64>(l.acquisitions)},
                                     {"contended", static_cast<qint64>(l.contended)},
                                     {"tryFailures", static_cast<qint64>(l.tryFailures)},
                                     {"maxWaitUs", l.maxWaitUs},
                                     {"p99WaitUs", l.p99WaitUs},
                                     {"maxHoldUs", l.maxHoldUs}});
        }
    }
    QJsonObject settings{{"mode", modeName(o.mode)},
                         {"rateHz", o.rateHz},
                         {"secs", o.secs},
                         {"reps", o.reps},
                         {"micHz", o.micHz},
                         {"smoke", o.smoke},
                         {"scenarios", toJsonArray(o.scenarios)}};
    QJsonObject machine{{"cpu", QString::fromStdString(cpuName())},
                        {"logicalCpus", static_cast<int>(std::thread::hardware_concurrency())},
                        {"os", QString::fromStdString(osName())},
                        {"kernel", QString::fromStdString(kernelRelease())},
                        {"build", buildType()},
                        {"lockStats", lockStatsBuild()}};
    return QJsonObject{{"bench", "input_path"},
                       {"settings", settings},
                       {"machine", machine},
                       {"rows", rows},
                       {"extras", extras},
                       {"locks", locks},
                       {"skipped", report.skipped},
                       {"notes", toJsonArray(report.notes)},
                       {"problems", toJsonArray(report.problems)}};
}

bool writeJson(const std::string& path, const QJsonObject& json) {
    QFile file(QString::fromStdString(path));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) { return false; }
    const QByteArray bytes = QJsonDocument(json).toJson(QJsonDocument::Indented);
    return file.write(bytes) == bytes.size();
}

bool writeCsv(const std::string& path, const Options& o, const Report& report) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) { return false; }
    const std::vector<std::string> headers = rowHeaders();
    for (std::size_t i = 0; i < headers.size(); ++i) { out << (i == 0 ? "" : ",") << headers[i]; }
    out << "\n";
    for (const ScenarioRun& run : report.runs) {
        for (const StreamResult& s : run.streams) {
            const auto cells = rowCells(o, run, s);
            for (std::size_t i = 0; i < cells.size(); ++i) {
                out << (i == 0 ? "" : ",") << cells[i];
            }
            out << "\n";
        }
    }
    return static_cast<bool>(out);
}

void printBanner(std::ostream& out, const Options& o) {
    out << "bench_input_path: mode " << modeName(o.mode) << ", rate " << o.rateHz << " Hz, "
        << o.secs << " s x " << o.reps << " reps, mic " << o.micHz << " Hz"
        << (o.smoke ? ", smoke" : "") << "\n"
        << "machine: " << cpuName() << ", " << std::thread::hardware_concurrency()
        << " logical CPUs, " << osName() << " (" << kernelRelease() << "), " << buildType()
        << " build" << (lockStatsBuild() ? ", lock statistics on" : "") << "\n";
}

} // namespace

namespace {

int runBench(int argc, char** argv) {
    const std::optional<Options> parsed = parseOptions(argc, argv);
    if (!parsed.has_value()) {
        printUsage(std::cerr);
        return kExitUsage;
    }
    const Options& options = *parsed;
    if (options.help) {
        printUsage(std::cout);
        return 0;
    }
    if (sodium_init() < 0) {
        std::cerr << "bench_input_path: libsodium failed to initialise\n";
        return kExitHarness;
    }
    QCoreApplication app(argc, argv);
    dt::setThreadRole(dt::ThreadRole::Gui);
    printBanner(std::cout, options);
    const Report report = options.mode == Mode::Vigem ? runVigem() : runSynthetic(options);
    printReport(std::cout, options, report);
    bool written = true;
    if (!options.json.empty()) { written = writeJson(options.json, toJson(options, report)); }
    if (!options.csv.empty()) { written = writeCsv(options.csv, options, report) && written; }
    if (!written) { std::cerr << "bench_input_path: could not write the results\n"; }
    return report.problems.empty() && written ? 0 : kExitHarness;
}

// Straight to the descriptor: a stream can throw, and nothing may leave main.
void writeStderr(const char* text) noexcept {
    std::size_t left = std::strlen(text);
    while (left > 0) {
        const ssize_t written = ::write(STDERR_FILENO, text, left);
        if (written <= 0) { return; }
        text += written;
        left -= static_cast<std::size_t>(written);
    }
}

} // namespace

// Anything the harness throws ends the run as a harness failure, with what it said.
int main(int argc, char** argv) {
    try {
        return runBench(argc, argv);
    } catch (const std::exception& e) {
        writeStderr("bench_input_path: ");
        writeStderr(e.what());
        writeStderr("\n");
    } catch (...) { writeStderr("bench_input_path: an unknown exception ended the run\n"); }
    return kExitHarness;
}
