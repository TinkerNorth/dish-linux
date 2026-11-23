#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Install the Linux build toolchain for Dish: scripts/install-deps.sh [--ci-qt]
#
# Default: the distro packages (apt; the README "Build from source" list plus
# the lint tools CI gates with, and clang-format at the version scripts/tools.sh
# pins, through pipx, as every CI lane in the fleet does).
#
# --ci-qt additionally installs the exact Qt CI builds against
# (DISH_QT_VERSION through aqtinstall, by scripts/install-qt.sh, the script
# .github/actions/setup-qt runs) into ~/Qt and prints the exports to use it.
# The tradeoff, so you can choose deliberately:
#
#   * Distro Qt (default): integrates with your package manager, but Debian
#     13 ships 6.8 and Ubuntu 24.04 only 6.4 (below the 6.7 floor). The
#     translation gate is the sharp edge: lupdate only resolves a class
#     defined across a header/source pair back to its namespace from 6.9 on,
#     so scripts/check-translations.sh can report spurious diffs under an
#     older lupdate that CI's Qt does not produce.
#   * CI Qt (--ci-qt): byte-for-byte what linux-ci.yml uses, so every gate
#     agrees with CI; ~1.5 GB under ~/Qt, and you export CMAKE_PREFIX_PATH /
#     QT_ROOT_DIR / LD_LIBRARY_PATH yourself (printed at the end).
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

CI_QT=0
for arg in "$@"; do
  case "$arg" in
    --ci-qt) CI_QT=1 ;;
    -h|--help) sed -n '5,25p' scripts/install-deps.sh; exit 0 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

step() { echo ""; echo "=== $1 ==="; }

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "install-deps.sh: only supported on Linux (got $(uname -s))." >&2
    exit 1
fi
if ! command -v apt-get >/dev/null 2>&1; then
    echo "This script drives apt (Debian/Ubuntu, what CI runs). On another distro install:" >&2
    echo "  gcc/clang, cmake, ninja, pkg-config, Qt 6.7+ (base, declarative, svg, tools/linguist)," >&2
    echo "  libsodium, SDL2, OpenSSL, Opus and DBus development headers, catch2, clang-tidy," >&2
    echo "  librsvg2 tools, and clang-format ${DISH_CLANG_FORMAT_VERSION} (pipx install clang-format==${DISH_CLANG_FORMAT_VERSION})." >&2
    exit 1
fi

step "apt packages (the README list + the lint tools CI gates with)"
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config \
    qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-svg-dev \
    qt6-tools-dev qt6-l10n-tools \
    libsodium-dev libsdl2-dev libssl-dev libopus-dev libdbus-1-dev catch2 \
    librsvg2-bin clang-tidy pipx

step "clang-format ${DISH_CLANG_FORMAT_VERSION} (pipx; the pin every CI lane uses)"
pipx install --force "clang-format==${DISH_CLANG_FORMAT_VERSION}"
pipx ensurepath
if ! command -v clang-format >/dev/null 2>&1; then
    echo "[NOTE] clang-format installed via pipx; open a new shell (pipx ensurepath) to pick it up."
fi

if [ "$CI_QT" -eq 1 ]; then
    step "Qt ${DISH_QT_VERSION} via aqtinstall ${DISH_AQTINSTALL_VERSION} (what .github/actions/setup-qt installs)"
    root="$(scripts/install-qt.sh "$HOME/Qt" | tail -n1)"
    echo ""
    echo "[OK] Qt ${DISH_QT_VERSION} at ${root}. To build against it, export (e.g. in ~/.bashrc):"
    echo "  export CMAKE_PREFIX_PATH=\"${root}\""
    echo "  export QT_ROOT_DIR=\"${root}\""
    echo "  export LD_LIBRARY_PATH=\"${root}/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\""
    echo "  export PATH=\"${root}/bin:\$PATH\""
fi

echo ""
echo "=== Done ==="
echo ""
echo "Next steps:"
echo "  1. Build:   scripts/build.sh"
echo "  2. Test:    scripts/build.sh test"
echo "  3. CI parity before pushing:  scripts/ci-local.sh"
