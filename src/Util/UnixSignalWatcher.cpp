// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "Util/UnixSignalWatcher.h"

#include <QPointer>

#include <cerrno>
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
    // SA_RESETHAND has already put the default disposition back, so a second
    // one still kills: a graceful shutdown that wedges would otherwise leave
    // the user nothing but SIGKILL.
    const auto byte = static_cast<unsigned char>(number);
    // The byte either lands or the pipe is gone, and nothing a handler may call
    // could report either. Only an interruption is worth retrying, and errno
    // is restored so the interrupted code sees its own.
    const int savedErrno = errno;
    while (::write(fd, &byte, 1) < 0 && errno == EINTR) {}
    errno = savedErrno;
}

void detachHandlers() {
    // The default disposition back on every number this watcher took over.
    // sigaction refuses only a number it does not know, and each of these was
    // accepted when it was installed, so there is no result to act on.
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    for (std::size_t i = 0; i < g_watchedCount; ++i) { ::sigaction(g_watched[i], &dfl, nullptr); }
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
    // seen there as an EINTR failure. SA_RESETHAND so the kernel restores the
    // default disposition on the way into the handler: the first signal asks
    // for a graceful shutdown, a second one kills. SA_RESETHAND is the top bit,
    // which glibc spells as an unsigned literal while sa_flags is an int; the
    // cast carries the bit pattern across.
    sa.sa_flags = static_cast<int>(SA_RESTART | SA_RESETHAND);
    for (const int number : numbers) {
        if (::sigaction(number, &sa, nullptr) == 0) { g_watched[g_watchedCount++] = number; }
    }

    auto watcher = std::make_unique<UnixSignalWatcher>(fds[0]);
    watcher->writeFd_ = fds[1];
    watcher->restoresHandlers_ = true;
    return watcher;
}

} // namespace dish::util
