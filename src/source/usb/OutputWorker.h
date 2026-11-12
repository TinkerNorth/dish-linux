// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "source/usb/HidReportWriter.h"

#include "core/input/OutputCadence.h"
#include "core/input/OutputMailbox.h"
#include "core/input/UsbOutputReports.h"
#include "core/input/UsbReportParsers.h"
#include "core/thread/WorkerWake.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace dish::source::usb {

// One claimed pad's feedback and settings. A producer posts the newest state of a kind to the
// mailbox and returns at once; the worker's own thread builds each pending kind's OUT report and
// writes it, however long the pad takes, so a slow pad never holds the receive thread or the read
// thread. It also writes what the pad is owed on a clock (input::OutputCadence): the 2026 Steam
// Controller's settings and its running rumble. A Steam Controller's settings are the worker's to
// put on a pad that is there (postPadLink) and to take off again when the claim is handed back
// (stop), so no other thread ever waits on a feature write. Made with the claim, started once the
// claim's writer exists, and stopped before the claim is released.
class OutputWorker {
  public:
    // Not started: what is posted before start() waits in the mailbox for the thread.
    explicit OutputWorker(input::usbparse::HidParser parser);
    // Started at once, for a claim whose writer is at hand.
    OutputWorker(std::unique_ptr<HidReportWriter> writer, input::usbparse::HidParser parser,
                 bool ds5VibrationV2 = false);
    ~OutputWorker();

    OutputWorker(const OutputWorker&) = delete;
    OutputWorker& operator=(const OutputWorker&) = delete;
    OutputWorker(OutputWorker&&) = delete;
    OutputWorker& operator=(OutputWorker&&) = delete;

    // The claim's writer, once; the thread starts here. `ds5VibrationV2` is the DualSense's rumble
    // mode, fixed at the claim (usbout::ds5UsesVibrationV2); every other family ignores it.
    void start(std::unique_ptr<HidReportWriter> writer, bool ds5VibrationV2 = false);

    // Any thread.
    void postRumble(std::uint16_t strong, std::uint16_t weak);
    void postLightbar(std::uint8_t r, std::uint8_t g, std::uint8_t b);
    void postPlayerLeds(std::uint8_t ledMask);
    void postTriggerEffects(
        const std::array<std::uint8_t, input::usbout::kTriggerEffectBlockBytes>& left,
        const std::array<std::uint8_t, input::usbout::kTriggerEffectBlockBytes>& right);
    void postMicLed(std::uint8_t state);
    // Whether a controller is there to hold the pad's settings: a model's own pad from its claim,
    // a receiver's slot from its link events. A 2015 Steam Controller that arrives is quieted; a
    // controller that leaves holds nothing a hand-back would take back.
    void postPadLink(bool linked);

    // Takes the settings the pad still holds off it, then joins the worker; whatever else is still
    // pending is dropped with the claim. Idempotent.
    void stop();

    // Reports the writer took, and ones it gave up on or a family without the surface refused.
    std::uint64_t writes() const { return writes_.load(std::memory_order_acquire); }
    std::uint64_t failedWrites() const { return failedWrites_.load(std::memory_order_acquire); }

  private:
    using Clock = input::OutputCadence::Clock;

    void run();
    bool hasWork() const;
    void sleepUntilWorkOrDue();
    void writeTaken(const input::OutputMailbox::Taken& taken, Clock::time_point now);
    void writeDueCadence(Clock::time_point now);
    void writeRumble(std::uint16_t strong, std::uint16_t weak, Clock::time_point now);
    void writeKeepalive(Clock::time_point now);
    void padLinkMoved(bool linked);
    bool settingsApplied();
    bool quietSteamPad();
    bool writeConfig(input::usbparse::SteamConfig stage);
    void settleLinkBeforeHandBack();
    void handBack();
    void writeReport(const std::uint8_t* report, std::size_t n);
    void writeFeatureReport(const std::uint8_t* report, std::size_t n);
    void countWrite(bool wrote);
    std::uint8_t nextSeq();

    const input::usbparse::HidParser parser_;
    // Set once by start(), before the thread that writes through it exists.
    std::unique_ptr<HidReportWriter> writer_;
    input::OutputMailbox mailbox_;
    thread::WorkerWake wake_;
    std::atomic<bool> stopping_{false};
    // The worker thread's own: the DualSense rumble mode, lamp and light-bar handoff shadow, the
    // Switch Pro's rolling packet counter, and whether the pad holds settings of ours to take back,
    // which belong to the claim rather than to any one write.
    input::usbout::FeedbackState state_{};
    std::uint8_t seq_ = 0;
    bool holdsSettings_ = false;
    // What the pad is owed on a clock, and the rumble a running repeat writes again.
    input::OutputCadence cadence_;
    std::uint16_t rumbleStrong_ = 0;
    std::uint16_t rumbleWeak_ = 0;
    std::atomic<std::uint64_t> writes_{0};
    std::atomic<std::uint64_t> failedWrites_{0};
    std::thread thread_;
};

} // namespace dish::source::usb
