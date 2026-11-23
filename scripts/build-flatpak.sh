#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Builds the sideload Flatpak from packaging/flatpak/com.tinkernorth.Dish.yml
# and exports it as a single-file bundle:
#
#   scripts/build-flatpak.sh               dist/Dish-<CMake version>-x86_64.flatpak
#   scripts/build-flatpak.sh <version>     dist/Dish-<version>-x86_64.flatpak
#
# Needs flatpak and flatpak-builder (and elfutils, whose eu-strip finishes each
# module). The KDE runtime and Sdk branch comes from scripts/tools.sh and is
# installed per user from Flathub if missing. flatpak-ci.yml and release.yml's
# flatpak job both run this, so a pull request builds the bundle a tag ships.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

version="${1:-}"
if [ -z "${version}" ]; then
    version="$(awk '/^project\(Dish/ { inproj = 1 }
                    inproj && /^[[:space:]]*VERSION[[:space:]]/ { print $2; exit }' CMakeLists.txt)"
fi
arch="$(uname -m)"

flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user -y --noninteractive flathub \
    "org.kde.Platform//${DISH_KDE_RUNTIME}" "org.kde.Sdk//${DISH_KDE_RUNTIME}"

# --disable-rofiles-fuse: a CI runner or container has no FUSE.
flatpak-builder --user --force-clean --disable-rofiles-fuse \
    --repo=build-flatpak/repo build-flatpak/build \
    packaging/flatpak/com.tinkernorth.Dish.yml

mkdir -p dist
flatpak build-bundle build-flatpak/repo "dist/Dish-${version}-${arch}.flatpak" com.tinkernorth.Dish
ls -l "dist/Dish-${version}-${arch}.flatpak"
