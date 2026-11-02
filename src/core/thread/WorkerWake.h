// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <atomic>
#include <semaphore>

namespace dish::thread {

// The satellite's WorkerWake: a worker sleeps on a counting semaphore that a producer releases only
// after it sees the worker asleep, so a busy stream costs its producer one load and no system call.
// The worker announces it is going to sleep before it checks for work one last time; a producer
// publishes its work before it looks, so one of the two always sees the other (both sequentially
// consistent).
class WorkerWake {
  public:
    void notify() noexcept {
        if (sleeping_.load(std::memory_order_seq_cst) &&
            sleeping_.exchange(false, std::memory_order_seq_cst)) {
            wake_.release();
        }
    }

    template <typename HasWork> void sleepUnless(HasWork&& hasWork) {
        sleeping_.store(true, std::memory_order_seq_cst);
        if (!hasWork()) { wake_.acquire(); }
        sleeping_.store(false, std::memory_order_seq_cst);
    }

  private:
    std::counting_semaphore<> wake_{0};
    std::atomic<bool> sleeping_{false};
};

} // namespace dish::thread
