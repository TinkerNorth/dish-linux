#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# clang-tidy over src/ outside src/UI/ and src/qml/, every finding an error:
# the sweep linux-ci.yml and scripts/ci-local.sh both run.
#
#   scripts/check-tidy.sh           (after `cmake --build --preset debug`)
#
# Reads the debug tree rather than configuring a second one: CMakeLists exports
# the compile database globally, so build/ already has entries for every src/
# file, and its mocs are generated. --warnings-as-errors keeps the gate here
# without forking the fleet-canonical .clang-tidy (WarningsAsErrors: '').
# src/UI/ and src/qml/ are outside it because moc and qmltyperegistrar output
# compiled into them is generated code no source change can fix (see
# CONTRIBUTING.md, clang-tidy).
set -euo pipefail
cd "$(dirname "$0")/.."

find src -type f \( -name '*.cpp' -o -name '*.h' \) \
    ! -path 'src/UI/*' \
    ! -path 'src/qml/*' \
    -print0 | xargs -0 -n1 -P"$(nproc)" \
    clang-tidy -p build --quiet --warnings-as-errors='*'
echo "clang-tidy: OK"
