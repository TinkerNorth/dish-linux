// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// One logging category for the whole Moonlight path — pairing, HTTP, the RTSP
// handshake, the control stream and the session coordinator. A mid-handshake
// hang-up reaches the client as a bare socket failure with no reply attached,
// so the step it died on is the only thing that identifies it; the category is
// shared so one filter rule follows a session end to end.
//
// QtInfoMsg floor, matching dish.net: the per-request trace lines are qCDebug
// and stay off until someone asks for them.

#pragma once

#include <QLoggingCategory>

namespace dish::source::moon {

Q_DECLARE_LOGGING_CATEGORY(lcMoon)

} // namespace dish::source::moon
