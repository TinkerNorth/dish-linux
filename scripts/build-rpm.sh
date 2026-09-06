#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Build an RPM package for Dish. Same rails as scripts/build-deb.sh (the
# `package` preset plus cpack), with the generator swapped: release.yml's rpm
# job calls this inside a fedora container so rpmbuild's soname scanner
# writes the Requires against Fedora's packages; a local run on another
# distro proves the CPack wiring and layout only.
#
# Output: ./dist/dish-<version>.<arch>.rpm
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "build-rpm.sh: only supported on Linux (got $(uname -s))." >&2
    exit 1
fi

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "build-rpm.sh: rpm-build is required (provides rpmbuild)." >&2
    echo "             Install with: sudo dnf install rpm-build (or apt install rpm)" >&2
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

echo "[*] Packaging (cpack -G RPM)"
mkdir -p "${DIST_DIR}"
cpack --config "${BUILD_DIR}/CPackConfig.cmake" -G RPM -B "${BUILD_DIR}/packages"
cp -f "${BUILD_DIR}"/packages/*.rpm "${DIST_DIR}/"

echo ""
echo "[OK] Package built:"
ls -lh "${DIST_DIR}"/dish-*.rpm
