// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Whether the machine behind a remembered address is still the one that was
// paired. A false "changed" tells the user their host was replaced and makes
// them pair it again for nothing; a false "same" lets a stranger's certificate
// be judged against a pin it was never meant to match. Both the manager's probe
// and the session's own /serverinfo ask it, so both depend on these cases.

#include "core/moonlight/MoonlightHostIdentity.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using dish::moonlight::hostIdentityChanged;
using dish::moonlight::isAddressPlaceholder;
using dish::moonlight::kAddressUuidPrefix;
using dish::moonlight::placeholderUuidFor;

TEST_CASE("host identity: the same uniqueid is the same host", "[moonlight][identity]") {
    CHECK_FALSE(hostIdentityChanged("7b5d0738-cbb5", "7b5d0738-cbb5"));
}

TEST_CASE("host identity: a different uniqueid is a replaced host", "[moonlight][identity]") {
    CHECK(hostIdentityChanged("7b5d0738-cbb5", "0c11aa19-4e2f"));
}

TEST_CASE("host identity: nothing remembered is no evidence of a change", "[moonlight][identity]") {
    // A host this client has never paired has no identity to have changed from.
    CHECK_FALSE(hostIdentityChanged("", "0c11aa19-4e2f"));
}

TEST_CASE("host identity: an answer that names no uniqueid proves nothing",
          "[moonlight][identity]") {
    // Some GameStream servers omit the field; that is not a different machine.
    CHECK_FALSE(hostIdentityChanged("7b5d0738-cbb5", ""));
}

TEST_CASE("host identity: a placeholder was never an identity", "[moonlight][identity]") {
    // A host found but never answered is filed under its address. Its first
    // real uniqueid replaces the placeholder; it does not contradict it.
    const std::string placeholder = placeholderUuidFor("192.0.2.40");
    CHECK_FALSE(hostIdentityChanged(placeholder, "0c11aa19-4e2f"));
}

TEST_CASE("host identity: placeholders are recognised by the prefix they are built with",
          "[moonlight][identity]") {
    const std::string placeholder = placeholderUuidFor("192.0.2.40");
    CHECK(placeholder == std::string(kAddressUuidPrefix) + "192.0.2.40");
    CHECK(isAddressPlaceholder(placeholder));
    // A real uniqueid, however it happens to start, is not one.
    CHECK_FALSE(isAddressPlaceholder("7b5d0738-cbb5"));
    CHECK_FALSE(isAddressPlaceholder("add"));
    CHECK_FALSE(isAddressPlaceholder(""));
}
