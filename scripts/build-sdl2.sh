#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Builds the pinned SDL2 (DISH_SDL2_VERSION in scripts/tools.sh) from its
# release tarball into a prefix, for the AppImage host, whose distribution SDL
# is below the 2.24 floor in CMakeLists.txt:
#
#   scripts/build-sdl2.sh <prefix> [<work dir>]
#
# The same feature set as the Flatpak manifests' sdl2 module: gamepads plus the
# audio subsystem controller audio opens the pads' own endpoints through.
# libpulse-dev is what lets its pulse backend configure; SDL dlopen's the
# host's libpulse at run time, so nothing extra is bundled. Pulse is the one
# audio backend this build carries, so ALSA is switched off by name: left at
# its default, SDL's configure looks for ALSA headers the host never installs
# and warns that it cannot find them, for a library that comes out the same
# either way. linuxdeploy bundles the resulting library into the AppImage.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/tools.sh
source "${repo_root}/scripts/tools.sh"

prefix="${1:?usage: $0 <prefix> [<work dir>]}"
work="${2:-$(mktemp -d)}"
mkdir -p "${work}"

fetch_pinned \
    "https://github.com/libsdl-org/SDL/releases/download/release-${DISH_SDL2_VERSION}/SDL2-${DISH_SDL2_VERSION}.tar.gz" \
    "${work}/sdl2.tar.gz" "${DISH_SDL2_SHA256}"
tar xzf "${work}/sdl2.tar.gz" -C "${work}"
cmake -S "${work}/SDL2-${DISH_SDL2_VERSION}" -B "${work}/sdl-build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${prefix}" \
    -DSDL_STATIC=OFF -DSDL_SHARED=ON -DSDL_TEST=OFF \
    -DSDL_RENDER=OFF -DSDL_AUDIO=ON -DSDL_ALSA=OFF \
    -DSDL_HIDAPI=ON -DSDL_JOYSTICK=ON -DSDL_HAPTIC=ON
cmake --build "${work}/sdl-build" --parallel
cmake --install "${work}/sdl-build"
