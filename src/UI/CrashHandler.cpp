// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "UI/CrashHandler.h"

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace dish::crash {

namespace {

constexpr int kMaxFrames = 64;
constexpr std::size_t kPathMax = 512;
constexpr std::size_t kAltStackSize = 64 * 1024;

// Static so SA_ONSTACK has somewhere to run when the fault is a stack overflow.
char g_altStack[kAltStackSize];

// Resolved once at install() time: the handler must not call getenv or build a
// path, neither of which is async-signal-safe.
char g_logPath[kPathMax] = {};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_inHandler{false};

void appendPath(char* dst, std::size_t cap, const char* part) {
    const std::size_t used = std::strlen(dst);
    const std::size_t room = cap - used - 1;
    std::strncat(dst + used, part, room);
}

// $XDG_STATE_HOME/dish, else $HOME/.local/state/dish. Empty when neither is
// set, which leaves the handler writing to stderr only.
void resolveLogPath() {
    const char* stateHome = std::getenv("XDG_STATE_HOME");
    if (stateHome != nullptr && stateHome[0] == '/') {
        std::strncpy(g_logPath, stateHome, kPathMax - 1);
    } else {
        const char* home = std::getenv("HOME");
        if (home == nullptr || home[0] != '/') { return; }
        std::strncpy(g_logPath, home, kPathMax - 1);
        appendPath(g_logPath, kPathMax, "/.local/state");
    }
    appendPath(g_logPath, kPathMax, "/dish");
    ::mkdir(g_logPath, 0700);
    appendPath(g_logPath, kPathMax, "/crash.log");
}

void writeAll(int fd, const char* text) {
    const std::size_t len = std::strlen(text);
    std::size_t written = 0;
    while (written < len) {
        const ssize_t n = ::write(fd, text + written, len - written);
        if (n <= 0) { return; }
        written += static_cast<std::size_t>(n);
    }
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV\n";
    case SIGABRT:
        return "SIGABRT\n";
    case SIGBUS:
        return "SIGBUS\n";
    case SIGFPE:
        return "SIGFPE\n";
    case SIGILL:
        return "SIGILL\n";
    default:
        return "signal\n";
    }
}

extern "C" void handler(int sig) {
    // A fault inside the handler would otherwise loop until the stack is gone.
    if (g_inHandler.exchange(true)) { ::_exit(128 + sig); }

    void* frames[kMaxFrames];
    const int depth = ::backtrace(frames, kMaxFrames);

    writeAll(STDERR_FILENO, "dish crashed: ");
    writeAll(STDERR_FILENO, signalName(sig));
    ::backtrace_symbols_fd(frames, depth, STDERR_FILENO);

    if (g_logPath[0] != '\0') {
        const int fd = ::open(g_logPath, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
        if (fd >= 0) {
            writeAll(fd, "--- dish crash: ");
            writeAll(fd, signalName(sig));
            ::backtrace_symbols_fd(frames, depth, fd);
            ::close(fd);
        }
    }

    // Re-raise on the default disposition so the shell's exit status and any
    // core-dump collector still see the real signal.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

} // namespace

void install() {
    if (g_installed.exchange(true)) { return; }
    resolveLogPath();

    stack_t altStack{};
    altStack.ss_sp = g_altStack;
    altStack.ss_size = sizeof(g_altStack);
    ::sigaltstack(&altStack, nullptr);

    struct sigaction sa{};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    // SA_ONSTACK so a stack-overflow SIGSEGV still has room to run the handler.
    sa.sa_flags = SA_RESTART | SA_ONSTACK;
    for (const int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
        ::sigaction(sig, &sa, nullptr);
    }
}

} // namespace dish::crash
