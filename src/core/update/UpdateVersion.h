// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The updater's ONE version grammar and ordering: strict MAJOR.MINOR.PATCH
// parsed to an int triple, compared as a tuple. No 'v' prefix, no prerelease
// tags, no build metadata — the release permalink never points at those, and a
// looser grammar is how "0.10.0-rc1" ends up ordered before "0.9.0". Kept apart
// from the installer's VersionCompare on purpose (section 16.1): the app must
// not link installer code to decide whether to stage.

#pragma once

#include <QString>

#include <optional>
#include <tuple>

namespace dish::update {

struct UpdateVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    // Strict `^\d+\.\d+\.\d+$`. Rejects anything else: empty parts, signs,
    // spaces, prefixes, suffixes, or a component that overflows int.
    static std::optional<UpdateVersion> parse(const QString& text);

    bool operator==(const UpdateVersion& o) const {
        return std::tie(major, minor, patch) == std::tie(o.major, o.minor, o.patch);
    }
    bool operator!=(const UpdateVersion& o) const { return !(*this == o); }
    bool operator<(const UpdateVersion& o) const {
        return std::tie(major, minor, patch) < std::tie(o.major, o.minor, o.patch);
    }
    bool operator<=(const UpdateVersion& o) const { return !(o < *this); }
    bool operator>(const UpdateVersion& o) const { return o < *this; }
    bool operator>=(const UpdateVersion& o) const { return !(*this < o); }
};

bool isValidVersion(const QString& text);

// -1 / 0 / +1 when both sides parse; nullopt when either is malformed, so a
// call site must decide what an unparsable version means for it (the reducer
// treats it as "not newer").
std::optional<int> compareVersions(const QString& a, const QString& b);

// True iff BOTH parse and candidate > baseline. The single predicate behind
// "stage only if strictly greater" and "apply only if staged > current":
// malformed input can never rank above a real version.
bool isStrictlyNewer(const QString& candidate, const QString& baseline);

} // namespace dish::update
