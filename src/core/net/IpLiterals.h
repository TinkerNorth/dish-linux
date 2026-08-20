// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Private-IP-literal classification, from the string alone with no DNS. Both
// discovery and the connect guard vet addresses through this, so a hostile beacon
// cannot point a dish at an arbitrary internet host.

#pragma once

#include <string>

namespace dish::net {

// True iff `host` is a private/loopback/link-local/unique-local IPv4 or IPv6
// literal. Surrounding "[...]" brackets (IPv6 URL-authority form) are stripped
// first. Returns false for hostnames, malformed literals, and public addresses.
bool isPrivateHostLiteral(const std::string& host);

} // namespace dish::net
