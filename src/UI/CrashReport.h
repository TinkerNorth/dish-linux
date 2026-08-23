// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// CrashReport — turns the backtrace CrashHandler left in
// $XDG_STATE_HOME/dish/crash.log into something a user can read, check, and
// choose to send.
//
// Nothing here transmits. The whole point of the local-report design is that the
// user sees the exact text before it goes anywhere, and that the "send" step is
// their browser opening a prefilled issue — not a background upload.
//
// `redact` is the part that earns its keep. A backtrace picks up the home
// directory, the LAN addresses Dish was talking to, and anything key-shaped that
// happened to be on the stack. A user pasting that into a public issue should not
// be publishing their pairing key, so the scrub happens before they ever see the
// text, not as advice in a doc they will not read.

#pragma once

#include <QString>
#include <QUrl>

namespace dish::crash {

// GitHub rejects a URL much past this, and browsers vary below it. A report that
// would overflow is truncated with a marker telling the user to attach the file.
inline constexpr int kMaxIssueUrlBytes = 6000;

// Absolute path of the crash log, following the same $XDG_STATE_HOME ladder the
// signal handler uses. Empty when neither XDG_STATE_HOME nor HOME is absolute.
QString crashLogPath();

// Whether a crash log exists and is non-empty.
bool hasCrashLog();

// The log's contents, or an empty string when there is none.
QString readCrashLog();

// Deletes the crash log. Called when the user dismisses the report, so the
// prompt does not follow them around after they have dealt with it.
bool discardCrashLog();

// Scrubs the identifying material out of a raw backtrace:
//   - the home directory collapses to `~`
//   - IPv4 literals become `[ip]`, and `*.local` hostnames become `[host].local`
//   - hex runs of 32 chars or more become `[redacted]`, which is what a pairing
//     key, a session key or a device id looks like on a stack
// `home` is a parameter rather than read from the environment so the tests can
// drive it without touching the real one.
QString redact(const QString& raw, const QString& home);

// The full report body: environment header plus the redacted backtrace. This is
// the exact text shown to the user and the exact text the issue is prefilled
// with — there is no second, richer version sent behind their back.
QString buildReport(const QString& rawLog, const QString& home);

// A prefilled "new issue" URL for the report. Truncates the body when it would
// exceed kMaxIssueUrlBytes rather than producing a URL the browser will drop.
QUrl issueUrl(const QString& report);

} // namespace dish::crash
