// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// A UDP socket that closes when it goes out of scope.
//
// For sockets whose lifetime IS a scope: a discovery scan opens one, uses it for
// a few seconds and is done with it, and every early return in between had to
// repeat a close() that nothing enforced. SatelliteClient's socket is not one of
// these - it lives as long as the client does and is closed on a state change,
// not on a return - so it keeps its own descriptor.
//
// Non-copyable and non-movable: one owner, one close. The dish-windows twin of
// this header is the same class over a Winsock SOCKET.

#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dish::net {

class ScopedSocket {
  public:
    ScopedSocket() : fd_(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {}
    ~ScopedSocket() {
        if (fd_ >= 0) { ::close(fd_); }
    }
    ScopedSocket(const ScopedSocket&) = delete;
    ScopedSocket& operator=(const ScopedSocket&) = delete;
    ScopedSocket(ScopedSocket&&) = delete;
    ScopedSocket& operator=(ScopedSocket&&) = delete;

    bool valid() const { return fd_ >= 0; }
    int get() const { return fd_; }

  private:
    int fd_;
};

} // namespace dish::net
