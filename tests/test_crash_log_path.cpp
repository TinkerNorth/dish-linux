// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The XDG ladder runs at install() time and the append runs inside the signal
// handler, so both are checked here against explicit inputs rather than against
// the process environment.

#include "UI/CrashHandler.h"

#include <catch2/catch_test_macros.hpp>

#include <csignal>
#include <cstddef>
#include <cstring>
#include <string>

using dish::crash::appendPath;
using dish::crash::kPathMax;
using dish::crash::logPathFor;
using dish::crash::signalName;

namespace {

std::string resolved(const char* xdgStateHome, const char* home) {
    char path[kPathMax] = {};
    logPathFor(path, kPathMax, xdgStateHome, home);
    return path;
}

// Appends into `cap` bytes of a larger buffer and fails if anything past the
// cap moved, which is the overrun the fixed buffer has to rule out.
std::string appended(const char* start, std::size_t cap, const char* part) {
    char buf[32];
    std::memset(buf, '#', sizeof(buf));
    std::strncpy(buf, start, cap - 1);
    buf[cap - 1] = '\0';
    appendPath(buf, cap, part);
    for (std::size_t i = cap; i < sizeof(buf); ++i) { REQUIRE(buf[i] == '#'); }
    return buf;
}

} // namespace

TEST_CASE("an absolute XDG_STATE_HOME wins over HOME", "[crash][logpath]") {
    REQUIRE(resolved("/var/state", "/home/dish") == "/var/state/dish/crash.log");
}

TEST_CASE("a relative XDG_STATE_HOME is ignored in favour of HOME", "[crash][logpath]") {
    // A relative setting is unusable from a handler that has no say over the
    // working directory it would resolve against.
    REQUIRE(resolved("state", "/home/dish") == "/home/dish/.local/state/dish/crash.log");
    REQUIRE(resolved("./state", "/home/dish") == "/home/dish/.local/state/dish/crash.log");
    REQUIRE(resolved("", "/home/dish") == "/home/dish/.local/state/dish/crash.log");
}

TEST_CASE("neither variable set leaves the path empty", "[crash][logpath]") {
    // The handler then writes to stderr only.
    REQUIRE(resolved(nullptr, nullptr).empty());
    REQUIRE(resolved("state", nullptr).empty());
    REQUIRE(resolved(nullptr, "relative/home").empty());
}

TEST_CASE("a path too long for the buffer is truncated, never overrun", "[crash][logpath]") {
    char path[16];
    std::memset(path, '#', sizeof(path));
    logPathFor(path, sizeof(path), "/a/very/long/state/home", nullptr);
    REQUIRE(std::strlen(path) == sizeof(path) - 1);
    REQUIRE(std::string(path) == "/a/very/long/st");
}

TEST_CASE("signalName names every handled signal", "[crash][logpath]") {
    // The trailing newline is part of the value: it goes straight to an fd.
    REQUIRE(std::string(signalName(SIGSEGV)) == "SIGSEGV\n");
    REQUIRE(std::string(signalName(SIGABRT)) == "SIGABRT\n");
    REQUIRE(std::string(signalName(SIGBUS)) == "SIGBUS\n");
    REQUIRE(std::string(signalName(SIGFPE)) == "SIGFPE\n");
    REQUIRE(std::string(signalName(SIGILL)) == "SIGILL\n");
}

TEST_CASE("signalName falls back for a signal it was not armed for", "[crash][logpath]") {
    REQUIRE(std::string(signalName(SIGTERM)) == "signal\n");
    REQUIRE(std::string(signalName(SIGINT)) == "signal\n");
    REQUIRE(std::string(signalName(0)) == "signal\n");
}

TEST_CASE("appendPath fills the buffer exactly at one below the cap", "[crash][append]") {
    // cap bytes hold cap-1 characters plus the terminator.
    REQUIRE(appended("", 8, "abcdefg") == "abcdefg");
}

TEST_CASE("appendPath truncates at the cap and one past it", "[crash][append]") {
    REQUIRE(appended("", 8, "abcdefgh") == "abcdefg");
    REQUIRE(appended("", 8, "abcdefghi") == "abcdefg");
}

TEST_CASE("appendPath truncates against what the buffer already holds", "[crash][append]") {
    REQUIRE(appended("/ho", 8, "/dish") == "/ho/dis");
    REQUIRE(appended("/ho", 8, "/di") == "/ho/di");
}

TEST_CASE("appendPath adds nothing once the buffer is full", "[crash][append]") {
    REQUIRE(appended("abcdefg", 8, "/dish") == "abcdefg");
    REQUIRE(appended("", 1, "/dish").empty());
}
