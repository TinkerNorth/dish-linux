// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Arms handlers for the fatal signals that carry a usable stack and writes a
// backtrace to $XDG_STATE_HOME/dish/crash.log. Nothing is transmitted anywhere
// — the file is the user's to send.
//
// The handler must never recurse or allocate, so it uses only
// async-signal-safe calls and re-raises on the default disposition afterwards
// so the shell and any core-dump collector still see the real signal.
//
// The path ladder, the signal names and the fixed-buffer append are inline here
// rather than in the .cpp: the .cpp is compiled into the exe, which the tests
// cannot link, and all three are pure enough to check against explicit inputs.

#pragma once

#include <csignal>
#include <cstddef>
#include <cstring>

namespace dish::crash {

// The resolved path lives in a fixed buffer; the handler may not allocate.
inline constexpr std::size_t kPathMax = 512;

// Truncates rather than overruns: the caller is a signal handler, where an
// overrun would be a crash inside the crash handler.
inline void appendPath(char* dst, std::size_t cap, const char* part) {
    const std::size_t used = std::strlen(dst);
    if (used + 1 >= cap) { return; }
    std::strncat(dst + used, part, cap - used - 1);
}

// $XDG_STATE_HOME/dish, else $HOME/.local/state/dish. Empty when neither is an
// absolute path, which leaves the handler writing to stderr only.
inline void logDirFor(char* dst, std::size_t cap, const char* xdgStateHome, const char* home) {
    if (cap == 0) { return; }
    dst[0] = '\0';
    if (xdgStateHome != nullptr && xdgStateHome[0] == '/') {
        std::strncpy(dst, xdgStateHome, cap - 1);
        dst[cap - 1] = '\0';
    } else {
        // A relative setting is unusable: by the time the handler runs, nothing
        // guarantees the working directory it was written against.
        if (home == nullptr || home[0] != '/') { return; }
        std::strncpy(dst, home, cap - 1);
        dst[cap - 1] = '\0';
        appendPath(dst, cap, "/.local/state");
    }
    appendPath(dst, cap, "/dish");
}

// The crash log inside that directory; empty when the directory is.
inline void logPathFor(char* dst, std::size_t cap, const char* xdgStateHome, const char* home) {
    logDirFor(dst, cap, xdgStateHome, home);
    if (cap == 0 || dst[0] == '\0') { return; }
    appendPath(dst, cap, "/crash.log");
}

// Trailing newline included: the handler writes the result straight to an fd.
inline const char* signalName(int sig) {
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

// Idempotent; call early in main().
void install();

} // namespace dish::crash
