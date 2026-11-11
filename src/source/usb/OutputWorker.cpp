// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/usb/OutputWorker.h"

#include "core/thread/TrackedMutex.h"

#include <optional>
#include <utility>

namespace dish::source::usb {

namespace {

// A fresh claim's shadow: every handoff still owed, nothing driven yet.
input::usbout::FeedbackState claimFeedbackState(bool ds5VibrationV2) {
    input::usbout::FeedbackState st;
    st.ds5VibrationV2 = ds5VibrationV2;
    return st;
}

} // namespace

OutputWorker::OutputWorker(std::unique_ptr<HidReportWriter> writer,
                           input::usbparse::HidParser parser, bool ds5VibrationV2)
    : writer_(std::move(writer)), parser_(parser), state_(claimFeedbackState(ds5VibrationV2)),
      cadence_(parser, Clock::now()), thread_([this] { run(); }) {}

OutputWorker::~OutputWorker() { stop(); }

void OutputWorker::stop() {
    if (stopping_.exchange(true, std::memory_order_seq_cst)) {
        if (thread_.joinable()) { thread_.join(); }
        return;
    }
    wake_.notify();
    if (thread_.joinable()) { thread_.join(); }
}

void OutputWorker::postRumble(std::uint16_t strong, std::uint16_t weak) {
    mailbox_.postRumble(strong, weak, 0);
    wake_.notify();
}

void OutputWorker::postLightbar(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    mailbox_.postLightbar(r, g, b);
    wake_.notify();
}

void OutputWorker::postPlayerLeds(std::uint8_t ledMask) {
    mailbox_.postPlayerLeds(ledMask);
    wake_.notify();
}

void OutputWorker::postTriggerEffects(
    const std::array<std::uint8_t, input::usbout::kTriggerEffectBlockBytes>& left,
    const std::array<std::uint8_t, input::usbout::kTriggerEffectBlockBytes>& right) {
    mailbox_.postTriggerEffects(left, right);
    wake_.notify();
}

void OutputWorker::postMicLed(std::uint8_t state) {
    mailbox_.postMicLed(state);
    wake_.notify();
}

void OutputWorker::run() {
    const thread::ScopedThreadRole role(thread::ThreadRole::Other);
    while (!stopping_.load(std::memory_order_seq_cst)) {
        sleepUntilWorkOrDue();
        const Clock::time_point now = Clock::now();
        if (const auto taken = mailbox_.take()) { writeTaken(*taken, now); }
        writeDueCadence(now);
    }
}

bool OutputWorker::hasWork() const {
    return stopping_.load(std::memory_order_seq_cst) || mailbox_.pending();
}

// A family with nothing on a clock sleeps until it has work, as it always has; one that owes its
// pad a write sleeps no later than that write is due.
void OutputWorker::sleepUntilWorkOrDue() {
    const std::optional<Clock::time_point> due = cadence_.nextDue();
    if (!due.has_value()) {
        wake_.sleepUnless([this] { return hasWork(); });
        return;
    }
    wake_.sleepUnlessUntil([this] { return hasWork(); }, *due);
}

// The Switch Pro takes a packet only when its low nibble advances.
std::uint8_t OutputWorker::nextSeq() {
    seq_ = static_cast<std::uint8_t>((seq_ + 1) & 0x0FU);
    return seq_;
}

void OutputWorker::countWrite(bool wrote) {
    if (wrote) {
        writes_.fetch_add(1, std::memory_order_release);
        return;
    }
    failedWrites_.fetch_add(1, std::memory_order_release);
}

void OutputWorker::writeReport(const std::uint8_t* report, std::size_t n) {
    countWrite(n != 0 && writer_->write(report, n));
}

void OutputWorker::writeFeatureReport(const std::uint8_t* report, std::size_t n) {
    countWrite(n != 0 && writer_->writeFeature(report, n));
}

// A rumble write, the host's or the cadence's repeat of it, starts the pad's repeat clock over.
void OutputWorker::writeRumble(std::uint16_t strong, std::uint16_t weak, Clock::time_point now) {
    std::array<std::uint8_t, input::usbout::kMaxOutputReportBytes> report{};
    writeReport(report.data(),
                input::usbout::buildRumbleReport(parser_, state_, strong, weak, nextSeq(),
                                                 report.data(), report.size()));
    rumbleStrong_ = strong;
    rumbleWeak_ = weak;
    const bool running = strong != 0 || weak != 0;
    cadence_.rumbleSent(now, running);
}

// The settings the pad must keep receiving, each a whole feature report.
void OutputWorker::writeKeepalive(Clock::time_point now) {
    std::array<std::uint8_t, input::usbparse::kMaxKeepaliveReportBytes> report{};
    for (int i = 0;; ++i) {
        const std::size_t n =
            input::usbparse::buildKeepaliveReport(parser_, i, report.data(), report.size());
        if (n == 0) { break; }
        writeFeatureReport(report.data(), n);
    }
    cadence_.settingsSent(now);
}

void OutputWorker::writeDueCadence(Clock::time_point now) {
    if (cadence_.settingsDue(now)) { writeKeepalive(now); }
    if (cadence_.rumbleDue(now)) { writeRumble(rumbleStrong_, rumbleWeak_, now); }
}

// One report per pending kind, in a fixed order, each from the newest state the mailbox held.
void OutputWorker::writeTaken(const input::OutputMailbox::Taken& taken, Clock::time_point now) {
    namespace output = input::output;
    namespace usbout = input::usbout;
    const input::OutputState& s = taken.state;
    std::array<std::uint8_t, usbout::kMaxOutputReportBytes> report{};
    if ((taken.kinds & output::kRumble) != 0) { writeRumble(s.strong, s.weak, now); }
    if ((taken.kinds & output::kLightbar) != 0) {
        writeReport(report.data(), usbout::buildLightbarReport(parser_, state_, s.r, s.g, s.b,
                                                               report.data(), report.size()));
    }
    if ((taken.kinds & output::kTriggerEffects) != 0) {
        writeReport(report.data(), usbout::buildTriggerEffectsReport(
                                       parser_, state_, s.leftTrigger.data(), s.rightTrigger.data(),
                                       report.data(), report.size()));
    }
    if ((taken.kinds & output::kPlayerLeds) != 0) {
        writeReport(report.data(),
                    usbout::buildPlayerLedsReport(parser_, state_, s.ledMask, nextSeq(),
                                                  report.data(), report.size()));
    }
    if ((taken.kinds & output::kMicLed) != 0) {
        writeReport(report.data(), usbout::buildMicMuteLedReport(parser_, state_, s.micLedState,
                                                                 report.data(), report.size()));
    }
}

} // namespace dish::source::usb
