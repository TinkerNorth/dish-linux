#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Builds the dish-bin pacman package from packaging/aur/PKGBUILD, as root in
# an archlinux:base-devel container:
#
#   scripts/build-arch-package.sh <version>              sources from the published release
#   scripts/build-arch-package.sh <version> <AppImage>   sources from this tree and that AppImage
#
# Output: dist/arch/dish-bin-<version>-1-x86_64.pkg.tar.zst
#
# release.yml's arch-publish job runs the first form after the release is
# public, so the repository package is built from, and checksummed against, the
# published bytes. linux-ci.yml's pull-request lane runs the second form with
# the AppImage it just built, so the PKGBUILD has been built before any tag
# needs it. Both forms run the same makepkg: the second only places the source
# files where makepkg looks before it downloads anything.
#
# The PKGBUILD in the tree keeps pkgver=0.1.0 and zero checksums on purpose:
# this script writes the version and `updpkgsums` writes the checksums.
set -euo pipefail
cd "$(dirname "$0")/.."

version="${1:?usage: $0 <version> [<AppImage>]}"
appimage="${2:-}"

# makepkg refuses to run as root, and the container runs as root: build as a
# throwaway user.
id builder >/dev/null 2>&1 || useradd -m builder
builddir=/home/builder/dish-bin
rm -rf "${builddir}"
mkdir -p "${builddir}"
cp packaging/aur/PKGBUILD packaging/aur/dish-bin.install "${builddir}/"
sed -i "s/^pkgver=.*/pkgver=${version}/; s/^pkgrel=.*/pkgrel=1/" "${builddir}/PKGBUILD"

if [ -n "${appimage}" ]; then
    # The names the PKGBUILD's source array gives each file, so makepkg finds
    # them in place and fetches nothing.
    cp "${appimage}" "${builddir}/Dish-${version}-x86_64.AppImage"
    cp packaging/dish.desktop "${builddir}/com.tinkernorth.Dish.desktop"
    cp packaging/dish.svg "${builddir}/com.tinkernorth.Dish.svg"
    cp packaging/com.tinkernorth.Dish.metainfo.xml "${builddir}/com.tinkernorth.Dish.metainfo.xml"
    cp packaging/udev/70-dish-hidraw.rules "${builddir}/70-dish-hidraw.rules"
fi

chown -R builder:builder "${builddir}"
# -d skips dependency checks: a -bin package() only copies files, and fuse2 and
# hicolor-icon-theme are runtime dependencies.
su builder -c "cd '${builddir}' && updpkgsums && PACKAGER='dish-release-bot <releases@tinkernorth.invalid>' makepkg -d --noconfirm"

mkdir -p dist/arch
cp "${builddir}"/*.pkg.tar.zst dist/arch/
ls -l dist/arch/
