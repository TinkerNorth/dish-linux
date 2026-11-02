// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include "core/thread/TrackedMutex.h"
#include "core/thread/WorkerWake.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace dish::net {

class SatelliteClient;

// Closes the session clients a session lets go of, on a thread of its own. Closing a client joins
// its heartbeat and receive threads, up to a heartbeat chunk and a receive wait of 100 ms each,
// which the GUI thread that tears a session down must not sit through. Shared by a manager and its
// sessions, so it outlives the last session that hands it a client.
class ClientReaper {
  public:
    ClientReaper();
    // Closes whatever is still queued, then joins.
    ~ClientReaper();

    ClientReaper(const ClientReaper&) = delete;
    ClientReaper& operator=(const ClientReaper&) = delete;
    ClientReaper(ClientReaper&&) = delete;
    ClientReaper& operator=(ClientReaper&&) = delete;

    // Any thread; returns at once. The reaper closes the client and drops its reference to it.
    void retire(std::shared_ptr<SatelliteClient> client);

    // Returns once every client retired before the call is closed.
    void drain();

  private:
    void run();

    thread::TrackedMutex mtx_{"wifi.reaper"};
    thread::WorkerWake wake_;
    // Under mtx_.
    std::vector<std::shared_ptr<SatelliteClient>> queue_;
    std::uint64_t retired_ = 0;
    bool stopping_ = false;
    std::atomic<std::uint64_t> closed_{0};
    std::thread thread_;
};

} // namespace dish::net
