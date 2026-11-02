// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "core/thread/TrackedMutex.h"

#include <QDBusMessage>
#include <QString>

#include <atomic>
#include <cstdint>
#include <optional>

namespace dish::source {

enum class PriorityRequest : std::uint8_t {
    None,
    High,
};

// What each thread asks the scheduler for: the three that carry every report and every feedback
// message ask to run ahead of the desktop, and the rest ask for nothing.
constexpr PriorityRequest priorityRequestFor(thread::ThreadRole role) {
    switch (role) {
    case thread::ThreadRole::SdlInput:
    case thread::ThreadRole::UsbReader:
    case thread::ThreadRole::Receive:
        return PriorityRequest::High;
    case thread::ThreadRole::Gui:
    case thread::ThreadRole::Heartbeat:
    case thread::ThreadRole::Playout:
    case thread::ThreadRole::AudioCapture:
    case thread::ThreadRole::MoonlightRx:
    case thread::ThreadRole::Update:
    case thread::ThreadRole::Other:
        break;
    }
    return PriorityRequest::None;
}

// The nice level asked for: inside rtkit's default floor of -15, and above the audio server's.
inline constexpr std::int32_t kHighPriorityNice = -10;

// rtkit's default RTTimeUSecMax. rtkit hands a priority only to a process whose RLIMIT_RTTIME is
// at most this, so a runaway realtime thread is killed instead of starving the machine.
inline constexpr std::uint64_t kRealtimeTimeLimitUsec = 200'000;

// Who grants the priority: rtkit on the system bus, or inside a Flatpak, whose thread ids are its
// own, the realtime portal on the session bus, which maps them to the host's for rtkit.
enum class PriorityBroker : std::uint8_t {
    RealtimeKit,
    Portal,
};

// The D-Bus call a request becomes for `broker`.
QDBusMessage highPriorityRequest(PriorityBroker broker, std::uint64_t pid, std::uint64_t tid,
                                 std::int32_t nice);

// Sends a request and answers with the error a refusal carries, or nothing when it was granted.
class PriorityBus {
  public:
    virtual ~PriorityBus() = default;
    virtual std::optional<QString> send(PriorityBroker broker, const QDBusMessage& request) = 0;

  protected:
    PriorityBus() = default;
    PriorityBus(const PriorityBus&) = default;
    PriorityBus& operator=(const PriorityBus&) = default;
    PriorityBus(PriorityBus&&) = default;
    PriorityBus& operator=(PriorityBus&&) = default;
};

// The real buses, waiting at most 250 ms for an answer: a thread asks once, as it starts.
class DbusPriorityBus final : public PriorityBus {
  public:
    std::optional<QString> send(PriorityBroker broker, const QDBusMessage& request) override;
};

// Caps RLIMIT_RTTIME at kRealtimeTimeLimitUsec, as rtkit requires. False when the system refuses.
bool capRealtimeTime();

// Asks for the calling thread's priority, once, as the thread starts: rtkit or the portal sets the
// nice level of that thread alone, and it ends with the thread. A refusal (no rtkit, no portal, no
// permission) changes nothing and is logged once per requester. Shared by every thread that asks.
class PriorityRequester {
  public:
    PriorityRequester(PriorityBus& bus, PriorityBroker broker) : bus_(bus), broker_(broker) {}

    // True when the role asked for nothing or was granted what it asked for.
    bool request(thread::ThreadRole role) const;

    bool refusalLogged() const { return refusalLogged_.load(std::memory_order_relaxed); }

  private:
    PriorityBus& bus_;
    const PriorityBroker broker_;
    mutable std::atomic<bool> refusalLogged_{false};
};

} // namespace dish::source
