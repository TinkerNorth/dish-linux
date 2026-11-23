#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# lcov over the Debug suite: build-cov/, cov.info, and the summary on stdout.
# linux-ci.yml's coverage job and scripts/ci-local.sh --with-coverage both run
# this.
#
# -fprofile-update=atomic: gcov's counters are non-atomic by default, so the
# threaded tests race them and geninfo aborts on a negative count. Timing
# dependent, so it passes until it doesn't.
set -euo pipefail
cd "$(dirname "$0")/.."

# CMAKE_DISABLE_FIND_PACKAGE_Catch2: this lane builds the suite against the
# FetchContent Catch2 tests/CMakeLists.txt pins, the copy a contributor without
# the distribution package gets, so the fallback is not an untested path.
cmake -S . -B build-cov -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DDISH_BUILD_TESTS=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_Catch2=ON \
    -DCMAKE_CXX_FLAGS="--coverage -fprofile-abs-path -fprofile-update=atomic" \
    -DCMAKE_EXE_LINKER_FLAGS="--coverage"
cmake --build build-cov --parallel
(cd build-cov && QT_QPA_PLATFORM=offscreen ctest --output-on-failure --parallel)
lcov --capture --directory build-cov --output-file cov.info \
    --ignore-errors mismatch,gcov,source
lcov --remove cov.info '/usr/*' '*/build-cov/*' '*/tests/*' \
    --output-file cov.info --ignore-errors unused
lcov --summary cov.info 2>&1
