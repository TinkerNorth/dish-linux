#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Build a Debian package (.deb) for Dish.
#
# Output: ./dist/dish_<version>_<arch>.deb
# Install with: sudo apt install ./dist/dish_*.deb
#
# The exact configure + cpack path the CI packaging jobs run (linux-ci.yml's
# package job and release.yml's deb job both call this script), via the
# `package` preset in CMakePresets.json. One deliberate difference between a
# local run and CI: CI builds inside a debian:trixie container so
# dpkg-shlibdeps computes Depends against Debian's own Qt sonames; a package
# built on another distro proves the CPack wiring and install layout, but its
# Depends line is that distro's, not Debian's.
#
# Prerequisites: scripts/install-deps.sh plus dpkg-dev.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "build-deb.sh: only supported on Linux (got $(uname -s))." >&2
    exit 1
fi

if ! command -v dpkg-shlibdeps >/dev/null 2>&1; then
    echo "build-deb.sh: dpkg-dev is required (provides dpkg-shlibdeps)." >&2
    echo "             Install with: sudo apt install dpkg-dev" >&2
    exit 1
fi

DIST_DIR="${DIST_DIR:-dist}"
BUILD_DIR="build-package"

echo "[*] Configuring (preset package) in ${BUILD_DIR}/"
# DISH_SENTRY_DSN is empty unless release.yml exported it from the repository
# secret. A hand-run of this script therefore produces a build that cannot
# transmit, and one that does not pay to fetch and build the SDK either.
cmake --preset package -DDISH_SENTRY_DSN="${DISH_SENTRY_DSN:-}"

echo "[*] Building"
cmake --build --preset package --parallel

echo "[*] Packaging (cpack -G DEB)"
mkdir -p "${DIST_DIR}"
cpack --config "${BUILD_DIR}/CPackConfig.cmake" -G DEB -B "${BUILD_DIR}/packages"
cp -f "${BUILD_DIR}"/packages/*.deb "${DIST_DIR}/"

echo ""
echo "[OK] Package built:"
ls -lh "${DIST_DIR}"/dish_*.deb
echo ""
echo "    Install with:  sudo apt install ./${DIST_DIR}/dish_*.deb"
echo "    Remove with:   sudo apt remove dish"
