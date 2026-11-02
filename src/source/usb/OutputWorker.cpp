// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "source/usb/OutputWorker.h"

#include "core/thread/TrackedMutex.h"

#include <utility>

namespace dish::source::usb {

OutputWorker::OutputWorker(std::unique_ptr<HidReportWriter> writer,
                           input::usbparse::HidParser parser)
    : writer_(std::move(writer)), parser_(parser), thread_([this] { run(); }) {}

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
        wake_.sleepUnless(
            [this] { return stopping_.load(std::memory_order_seq_cst) || mailbox_.pending(); });
        if (const auto taken = mailbox_.take()) { writeTaken(*taken); }
    }
}

// The Switch Pro takes a packet only when its low nibble advances.
std::uint8_t OutputWorker::nextSeq() {
    seq_ = static_cast<std::uint8_t>((seq_ + 1) & 0x0FU);
    return seq_;
}

void OutputWorker::writeReport(const std::uint8_t* report, std::size_t n) {
    if (n != 0 && writer_->write(report, n)) {
        writes_.fetch_add(1, std::memory_order_release);
        return;
    }
    failedWrites_.fetch_add(1, std::memory_order_release);
}

// One report per pending kind, in a fixed order, each from the newest state the mailbox held.
void OutputWorker::writeTaken(const input::OutputMailbox::Taken& taken) {
    namespace output = input::output;
    namespace usbout = input::usbout;
    const input::OutputState& s = taken.state;
    std::array<std::uint8_t, usbout::kMaxOutputReportBytes> report{};
    if ((taken.kinds & output::kRumble) != 0) {
        writeReport(report.data(),
                    usbout::buildRumbleReport(parser_, state_, s.strong, s.weak, nextSeq(),
                                              report.data(), report.size()));
    }
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
