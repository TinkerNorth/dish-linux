#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Every fact the tree states in more than one place, compared against its
# source. version-consistency.yml and scripts/ci-local.sh both run this; it is
# a text gate, not a build.
#
#   the version       project(Dish VERSION) -> AppStream <release>, CHANGELOG heading,
#                     update-policy.json minimum (not newer)
#   the Qt floor      qt_standard_project_setup(REQUIRES) -> find_package, docs/PACKAGING.md,
#                     README.md, THIRD_PARTY.md, licenses.json
#   the Qt pin        scripts/tools.sh DISH_QT_VERSION -> CONTRIBUTING.md, docs/PACKAGING.md
#   the SDL2 floor    pkg_check_modules(sdl2>=) -> THIRD_PARTY.md, licenses.json
#   the SDL2 pin      scripts/tools.sh -> both Flatpak manifests (url and sha256)
#   sentry-native     scripts/tools.sh -> CMakeLists.txt, both Flatpak manifests,
#                     THIRD_PARTY.md, licenses.json
#   Catch2            the tests/CMakeLists.txt fallback pin -> licenses.json
#   the KDE runtime   scripts/tools.sh DISH_KDE_RUNTIME -> both Flatpak manifests,
#                     docs/PACKAGING.md, and no other org.kde.Platform// branch anywhere
#   the Flatpak deps  the Flathub template's modules match the sideload manifest's
#   containers        scripts/tools.sh images -> every workflow `container:` line
#   clang-format      scripts/tools.sh -> .clang-format's header
#   the protocol      kProtocolVersion -> docs/contract.md, CONTRIBUTING.md
#
# Prints every drift it finds, then fails if there was any.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

fail=0
err() {
    echo "::error::$1"
    fail=1
}
# has <file> <fixed string> <message>
has() {
    grep -Fq -- "$2" "$1" || err "$1: $3"
}
# count <file> <fixed string>
count() {
    grep -Fc -- "$2" "$1" || true
}

sideload=packaging/flatpak/com.tinkernorth.Dish.yml
flathub=packaging/flatpak/flathub/com.tinkernorth.Dish.yml
licenses=assets/licenses/licenses.json

license_version() {
    python3 -c 'import json, sys
for entry in json.load(open(sys.argv[1]))["libraries"]:
    if entry["artifact"] == sys.argv[2]:
        print(entry["version"])' "${licenses}" "$1"
}

# --- the version -------------------------------------------------------------
ver=$(awk '/^project\(Dish/ { inproj = 1 }
           inproj && /^[[:space:]]*VERSION[[:space:]]/ { print $2; exit }' CMakeLists.txt)
if ! grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' <<<"${ver}"; then
    echo "::error::could not read MAJOR.MINOR.PATCH from project(Dish VERSION ...) (got '${ver}')"
    exit 1
fi
echo "version ${ver}"
# AppStream: GNOME Software and KDE Discover show this as the version, so a
# stale entry tells users the wrong build shipped.
grep -Eq "<release version=\"${ver}\"" packaging/com.tinkernorth.Dish.metainfo.xml \
    || err "packaging/com.tinkernorth.Dish.metainfo.xml has no <release version=\"${ver}\">"
# A released version needs its own heading; an unreleased tree keeps the
# Unreleased one.
grep -Eq "^## \[(${ver}|Unreleased)\]" CHANGELOG.md \
    || err "CHANGELOG.md has neither a [${ver}] nor an [Unreleased] heading"
# UpdateManifest rejects a manifest whose minimum is newer than its version,
# which would fail every client's check rather than the old ones only.
min=$(python3 -c 'import json;print(json.load(open("packaging/update-policy.json"))["minimumSupportedVersion"])')
newest=$(printf '%s\n%s\n' "${ver}" "${min}" | sort -V | tail -1)
[ "${newest}" = "${ver}" ] \
    || err "packaging/update-policy.json minimumSupportedVersion (${min}) is newer than ${ver}"

# --- the Qt floor and the Qt pin ---------------------------------------------
floor=$(sed -nE 's/.*qt_standard_project_setup\(REQUIRES ([0-9]+\.[0-9]+)\).*/\1/p' CMakeLists.txt)
if [ -z "${floor}" ]; then
    err "could not read the Qt floor from qt_standard_project_setup(REQUIRES ...)"
else
    echo "Qt floor ${floor}, Qt pin ${DISH_QT_VERSION}"
    [ "$(count CMakeLists.txt "find_package(Qt6 ${floor} ")" = 2 ] \
        || err "CMakeLists.txt find_package(Qt6 ...) calls do not both use the ${floor} floor"
    has docs/PACKAGING.md "Qt ${floor}" "does not name the Qt ${floor} floor"
    has README.md "Qt ${floor}" "does not name the Qt ${floor} floor"
    has THIRD_PARTY.md ">= ${floor}" "does not name the Qt >= ${floor} floor"
    [ "$(license_version qtbase)" = "${floor}+" ] || err "${licenses}: Qt version is not \"${floor}+\""
fi
has CONTRIBUTING.md "Qt ${DISH_QT_VERSION}" "does not name the Qt ${DISH_QT_VERSION} CI builds with"
has docs/PACKAGING.md "Qt ${DISH_QT_VERSION}" "does not name the Qt ${DISH_QT_VERSION} CI builds with"

# --- SDL2 --------------------------------------------------------------------
sdl_floor=$(sed -nE 's/.*pkg_check_modules\(SDL2 +REQUIRED IMPORTED_TARGET sdl2>=([0-9]+\.[0-9]+)\).*/\1/p' CMakeLists.txt)
if [ -z "${sdl_floor}" ]; then
    err "CMakeLists.txt: pkg_check_modules(SDL2 ...) states no sdl2>= floor"
else
    echo "SDL2 floor ${sdl_floor}, SDL2 pin ${DISH_SDL2_VERSION}"
    grep -Eq "^\| \[SDL2\]\(#sdl2\) \| >= ${sdl_floor} \|" THIRD_PARTY.md \
        || err "THIRD_PARTY.md: the SDL2 row does not give the >= ${sdl_floor} floor"
    [ "$(license_version SDL2)" = "${sdl_floor}+" ] || err "${licenses}: SDL2 version is not \"${sdl_floor}+\""
fi
for manifest in "${sideload}" "${flathub}"; do
    has "${manifest}" "release-${DISH_SDL2_VERSION}/SDL2-${DISH_SDL2_VERSION}.tar.gz" \
        "the sdl2 module is not SDL2 ${DISH_SDL2_VERSION}"
    has "${manifest}" "sha256: ${DISH_SDL2_SHA256}" "the sdl2 module's sha256 is not the pinned one"
done
has THIRD_PARTY.md "SDL2 ${DISH_SDL2_VERSION}" "does not name the SDL2 ${DISH_SDL2_VERSION} the AppImage and Flatpak bundle"

# --- sentry-native and sentry-cli --------------------------------------------
sentry_url="sentry-native/releases/download/${DISH_SENTRY_NATIVE_VERSION}/sentry-native.zip"
echo "sentry-native ${DISH_SENTRY_NATIVE_VERSION}, sentry-cli ${DISH_SENTRY_CLI_VERSION}"
has CMakeLists.txt "${sentry_url}" "FetchContent_Declare(sentry) is not ${DISH_SENTRY_NATIVE_VERSION}"
has CMakeLists.txt "URL_HASH SHA256=${DISH_SENTRY_NATIVE_RELEASE_ZIP_SHA256}" "FetchContent_Declare(sentry)'s hash is not the pinned one"
for manifest in "${sideload}" "${flathub}"; do
    has "${manifest}" "${sentry_url}" "the sentry-native source is not ${DISH_SENTRY_NATIVE_VERSION}"
    has "${manifest}" "sha256: ${DISH_SENTRY_NATIVE_RELEASE_ZIP_SHA256}" "the sentry-native sha256 is not the pinned one"
done
grep -Eq "^\| \[sentry-native\]\(#sentry-native\) \| ${DISH_SENTRY_NATIVE_VERSION} \|" THIRD_PARTY.md \
    || err "THIRD_PARTY.md: the sentry-native row is not ${DISH_SENTRY_NATIVE_VERSION}"
[ "$(license_version sentry-native)" = "${DISH_SENTRY_NATIVE_VERSION}" ] \
    || err "${licenses}: sentry-native version is not ${DISH_SENTRY_NATIVE_VERSION}"
if grep -rnE 'sentry-cli/releases/download/' .github scripts | grep -v 'DISH_SENTRY_CLI_VERSION'; then
    err "a sentry-cli download outside scripts/sentry-cli.sh names its own version"
fi

# --- Catch2: the FetchContent fallback and the Licenses screen ---------------
catch2=$(sed -nE 's/^[[:space:]]+GIT_TAG[[:space:]]+[0-9a-f]{40}\) # v([0-9.]+)$/\1/p' tests/CMakeLists.txt)
if [ -z "${catch2}" ]; then
    err "tests/CMakeLists.txt: the Catch2 fallback is not pinned to a commit with a '# vX.Y.Z' comment"
else
    echo "Catch2 fallback ${catch2}"
    [ "$(license_version Catch2)" = "${catch2}" ] || err "${licenses}: Catch2 version is not ${catch2}"
    has THIRD_PARTY.md "Catch2 v${catch2}" "does not name the Catch2 v${catch2} fallback"
fi

# --- the KDE runtime ---------------------------------------------------------
echo "KDE runtime ${DISH_KDE_RUNTIME}"
for manifest in "${sideload}" "${flathub}"; do
    has "${manifest}" "runtime-version: '${DISH_KDE_RUNTIME}'" "runtime-version is not '${DISH_KDE_RUNTIME}'"
done
has docs/PACKAGING.md "org.kde.Platform//${DISH_KDE_RUNTIME}" "does not name org.kde.Platform//${DISH_KDE_RUNTIME}"
# One value: any other branch written anywhere outside the release history is drift.
others=$(git grep -nE "org\.kde\.(Platform|Sdk)//[0-9]" -- ':!CHANGELOG.md' \
    | grep -vE "org\.kde\.(Platform|Sdk)//${DISH_KDE_RUNTIME//./\\.}([^0-9]|$)" || true)
[ -z "${others}" ] || err "another KDE runtime branch is named:
${others}"
stale=$(git grep -nE "runtime-version: '[0-9]" -- packaging | grep -vF "runtime-version: '${DISH_KDE_RUNTIME}'" || true)
[ -z "${stale}" ] || err "a manifest names another runtime-version:
${stale}"

# --- the Flathub template keeps the sideload manifest's dependencies ---------
for pin in \
    "$(grep -E '^\s+url: https://github.com/jedisct1/libsodium/' "${sideload}")" \
    "$(grep -A1 -E '^\s+url: https://github.com/jedisct1/libsodium/' "${sideload}" | tail -1)"
do
    has "${flathub}" "${pin#"${pin%%[![:space:]]*}"}" "differs from ${sideload} on libsodium"
done

# --- containers --------------------------------------------------------------
echo "containers ${DISH_DEB_IMAGE} ${DISH_RPM_IMAGE} ${DISH_ARCH_IMAGE}"
while IFS= read -r line; do
    image="${line##*container: }"
    case "${image}" in
    "${DISH_DEB_IMAGE}" | "${DISH_RPM_IMAGE}" | "${DISH_ARCH_IMAGE}") ;;
    *) err "${line%%:*}: container ${image} is not one scripts/tools.sh pins" ;;
    esac
done < <(grep -nH -E '^\s+container:' .github/workflows/*.yml)

# --- clang-format ------------------------------------------------------------
has .clang-format "clang-format ${DISH_CLANG_FORMAT_VERSION}" "does not name clang-format ${DISH_CLANG_FORMAT_VERSION}"

# --- the protocol ------------------------------------------------------------
protocol=$(sed -nE 's/^inline constexpr int kProtocolVersion = ([0-9]+);.*/\1/p' src/core/model/Protocol.h)
if [ -z "${protocol}" ]; then
    err "could not read kProtocolVersion from src/core/model/Protocol.h"
else
    echo "protocol ${protocol}"
    has docs/contract.md "protocol version ${protocol}" "does not name protocol version ${protocol}"
    has CONTRIBUTING.md "protocol version ${protocol}" "does not name protocol version ${protocol}"
fi

if [ "${fail}" -ne 0 ]; then
    echo "::error::Drift between a source and its copies. scripts/check-version-consistency.sh names each source; CONTRIBUTING.md, 'Cutting a release', has the list."
    exit 1
fi
echo "All version mirrors and pins agree."
