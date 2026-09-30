// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Who a Moonlight host is, as far as this client can tell.
//
// A host is remembered under the uniqueid its /serverinfo reports. A host that
// was found but has never answered has no uniqueid yet, so it is filed under a
// placeholder built from its address until the first answer replaces it.
//
// The one decision here is whether the machine behind a remembered address is
// still the machine that was paired. It is asked in two places - the manager's
// probe and the session's own /serverinfo - and used to be written out in both,
// with the placeholder's prefix spelled as a bare string in three. One copy of
// the rule is what keeps the two answers the same.
//
// Pure, Qt-free and platform-free, like the rest of src/core.

#pragma once

#include <string>
#include <string_view>

namespace dish::moonlight {

// The prefix that marks a placeholder uniqueid.
inline constexpr std::string_view kAddressUuidPrefix = "addr:";

inline std::string placeholderUuidFor(std::string_view address) {
    std::string uuid(kAddressUuidPrefix);
    uuid += address;
    return uuid;
}

inline bool isAddressPlaceholder(std::string_view uuid) {
    return uuid.substr(0, kAddressUuidPrefix.size()) == kAddressUuidPrefix;
}

// True when the host answered with a uniqueid that is not the one remembered for
// it: the machine was reset or replaced, and the stored certificate anchors
// nothing. Each of the three ways to have no evidence is not a change -
// nothing remembered, nothing reported, or a placeholder that was never an
// identity to begin with.
inline bool hostIdentityChanged(std::string_view remembered, std::string_view reported) {
    const bool haveRemembered = !remembered.empty() && !isAddressPlaceholder(remembered);
    const bool haveReported = !reported.empty();
    return haveRemembered && haveReported && reported != remembered;
}

} // namespace dish::moonlight
