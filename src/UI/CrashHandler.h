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

#pragma once

namespace dish::crash {

// Idempotent; call early in main().
void install();

} // namespace dish::crash
