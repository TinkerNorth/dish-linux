// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Util/UnixSignalWatcher.h"

#include <QPointer>

#include <csignal>
#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstddef>

namespace dish::util {
namespace {

constexpr std::size_t kMaxWatched = 8;

std::atomic<int> g_writeFd{-1};
std::atomic<bool> g_installed{false};
std::array<int, kMaxWatched> g_watched{};
std::size_t g_watchedCount = 0;

void handler(int number) {
    const int fd = g_writeFd.load(std::memory_order_relaxed);
    if (fd < 0) { return; }
    // A second one has to still kill: a graceful shutdown that wedges would
    // otherwise leave the user nothing but SIGKILL.
    (void)::signal(number, SIG_DFL);
    const auto byte = static_cast<unsigned char>(number);
    const ssize_t written = ::write(fd, &byte, 1);
    (void)written; // Nothing a handler is allowed to call could report it.
}

void detachHandlers() {
    for (std::size_t i = 0; i < g_watchedCount; ++i) { (void)::signal(g_watched[i], SIG_DFL); }
    g_watchedCount = 0;
    g_writeFd.store(-1, std::memory_order_relaxed);
    g_installed.store(false, std::memory_order_release);
}

} // namespace

UnixSignalWatcher::UnixSignalWatcher(int readFd, QObject* parent)
    : QObject(parent), readFd_(readFd),
      notifier_(std::make_unique<QSocketNotifier>(readFd, QSocketNotifier::Read)) {
    connect(notifier_.get(), &QSocketNotifier::activated, this, &UnixSignalWatcher::drain);
}

UnixSignalWatcher::~UnixSignalWatcher() {
    // Before the fds go, so no handler can be mid-write against a closed one.
    if (restoresHandlers_) { detachHandlers(); }
    notifier_.reset();
    if (readFd_ >= 0) { ::close(readFd_); }
    if (writeFd_ >= 0) { ::close(writeFd_); }
}

void UnixSignalWatcher::drain() {
    std::array<unsigned char, 16> buf{};
    const ssize_t count = ::read(readFd_, buf.data(), buf.size());
    if (count <= 0) { return; }
    // The subscriber's whole job is to shut the app down, and one of the ways
    // it can do that is to delete this object out from under the loop.
    QPointer<UnixSignalWatcher> alive(this);
    const auto received = static_cast<std::size_t>(count);
    for (std::size_t i = 0; i < received && !alive.isNull(); ++i) {
        Q_EMIT signalled(static_cast<int>(buf[i]));
    }
}

std::unique_ptr<UnixSignalWatcher> installSignalWatcher(std::initializer_list<int> numbers) {
    if (numbers.size() > kMaxWatched) { return nullptr; }
    if (g_installed.exchange(true, std::memory_order_acq_rel)) { return nullptr; }

    int fds[2] = {-1, -1};
    // Non-blocking on both ends: a handler that blocked on a full pipe would
    // hang whatever thread the kernel happened to deliver the signal on.
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        g_installed.store(false, std::memory_order_release);
        return nullptr;
    }
    g_writeFd.store(fds[1], std::memory_order_relaxed);

    struct sigaction sa{};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART so a signal during a read on the HID or socket threads is not
    // seen there as an EINTR failure.
    sa.sa_flags = SA_RESTART;
    for (const int number : numbers) {
        if (::sigaction(number, &sa, nullptr) == 0) { g_watched[g_watchedCount++] = number; }
    }

    auto watcher = std::make_unique<UnixSignalWatcher>(fds[0]);
    watcher->writeFd_ = fds[1];
    watcher->restoresHandlers_ = true;
    return watcher;
}

} // namespace dish::util
