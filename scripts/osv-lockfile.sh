#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Prints an OSV-Scanner custom lockfile (the JSON shape of `osv-scanner --format
# json`) naming every component this repository builds from source by the
# upstream commit its pin resolves to, so OSV-Scanner matches the advisories
# that carry git ranges:
#
#   bash scripts/osv-lockfile.sh > osv.json
#   bash scripts/osv-scan.sh --lockfile-script scripts/osv-lockfile.sh .
#
# Nothing here has a manifest OSV-Scanner reads (CMake FetchContent, a
# Flatpak module, a vendored directory), which is why a plain scan of the tree
# finds no packages. The components, each from the file that pins it:
#
#   ENet (cgutman fork)  DISH_ENET_COMMIT in scripts/tools.sh   vendored into every build
#   SDL2                 DISH_SDL2_VERSION                       bundled in the AppImage and the Flatpak
#   libsodium            DISH_LIBSODIUM_VERSION                  bundled in the Flatpak
#   sentry-native        DISH_SENTRY_NATIVE_VERSION              linked into every release build
#   Catch2               tests/CMakeLists.txt's GIT_TAG          the test suite's fallback
#
# Qt, Opus, OpenSSL and libcurl come from the distribution, the KDE runtime or
# the official Qt binaries, not from a pin in this tree, so they are the
# release scan's job (Grype over the packages), not this one's.
#
# A pin that does not resolve to a commit in its upstream fails the script, so
# no component drops out of the scan unnoticed.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

fail() {
    echo "osv-lockfile: $*" >&2
    exit 1
}

# Fetches just that commit object (no trees, no blobs): GitHub answers "not our
# ref" for a SHA the repository does not have.
commit_in_upstream() {
    local url="$1" sha="$2" scratch found=false
    scratch=$(mktemp -d)
    git -C "${scratch}" init -q
    if git -C "${scratch}" fetch -q --depth=1 --filter=tree:0 "${url}" "${sha}" 2>/dev/null; then
        found=true
    fi
    rm -rf "${scratch}"
    [ "${found}" = true ]
}

# A tag's commit is the peeled ^{} line when the tag is annotated, else the
# tag's own line; a 40-character pin must exist in the upstream.
resolve_commit() {
    local url="$1" pin="$2" refs peeled direct
    if [[ "${pin}" =~ ^[0-9a-f]{40}$ ]]; then
        if commit_in_upstream "${url}" "${pin}"; then echo "${pin}"; fi
        return
    fi
    refs=$(git ls-remote --tags "${url}")
    peeled=$(awk -v ref="refs/tags/${pin}^{}" '$2 == ref { print $1 }' <<<"${refs}")
    direct=$(awk -v ref="refs/tags/${pin}" '$2 == ref { print $1 }' <<<"${refs}")
    echo "${peeled:-${direct}}"
}

catch2_commit=$(sed -nE 's/^[[:space:]]+GIT_TAG[[:space:]]+([0-9a-f]{40})\).*/\1/p' tests/CMakeLists.txt)
[ -n "${catch2_commit}" ] || fail "tests/CMakeLists.txt pins Catch2 to no commit"

pins=(
    "https://github.com/cgutman/enet ${DISH_ENET_COMMIT}"
    "https://github.com/libsdl-org/SDL release-${DISH_SDL2_VERSION}"
    "https://github.com/jedisct1/libsodium ${DISH_LIBSODIUM_VERSION}-RELEASE"
    "https://github.com/getsentry/sentry-native ${DISH_SENTRY_NATIVE_VERSION}"
    "https://github.com/catchorg/Catch2 ${catch2_commit}"
)

entries=()
for line in "${pins[@]}"; do
    url="${line%% *}"
    pin="${line#* }"
    commit=$(resolve_commit "${url}" "${pin}")
    [[ "${commit}" =~ ^[0-9a-f]{40}$ ]] || fail "pin '${pin}' does not resolve in ${url}"
    entries+=("{\"package\":{\"name\":\"${url}\",\"commit\":\"${commit}\"}}")
done
joined=$(IFS=,; echo "${entries[*]}")
printf '{"results":[{"packages":[%s]}]}\n' "${joined}"
