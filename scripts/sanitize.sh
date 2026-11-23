#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# The suite under a sanitizer, configured, built and run the way linux-ci.yml's
# sanitize and lock-laws-sanitize jobs and scripts/ci-local.sh --with-sanitizers
# all do:
#
#   scripts/sanitize.sh <address+undefined|thread>             DishTests
#   scripts/sanitize.sh <address+undefined|thread> lock-laws   the lock laws, DISH_LOCK_STATS on
#
# The sanitizer is a cache option rather than a preset on purpose: it is a
# matrix value, and a preset per sanitizer would be four presets for one flag.
#
# DishTests only, not everything: the suite covers dish_core, and the Qt Quick
# app target adds only qmlcachegen output, where GCC refuses
# atomic_thread_fence under TSan. Instrumenting it buys no coverage.
#
# Qt and glib are uninstrumented, so their own cross-thread new and delete read
# as a race to TSan; tests/tsan.suppressions says exactly what is excluded and
# why. Serial: two instrumented processes at once is what makes a sanitizer run
# flake on a shared runner.
set -euo pipefail
cd "$(dirname "$0")/.."

san="${1:?usage: $0 <address+undefined|thread> [lock-laws]}"
mode="${2:-suite}"
case "${san}" in
address+undefined | thread) ;;
*)
    echo "unknown sanitizer: ${san}" >&2
    exit 2
    ;;
esac

case "${mode}" in
suite)
    tree="build-san-${san}"
    lock_stats=OFF
    target=DishTests
    filter=()
    ;;
lock-laws)
    tree="build-san-lockstats-${san}"
    lock_stats=ON
    target=DishLockLawTests
    # The tree registers DishTests too but does not build them.
    filter=(-R "^lock law")
    ;;
*)
    echo "unknown mode: ${mode}" >&2
    exit 2
    ;;
esac

cmake -S . -B "${tree}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DDISH_BUILD_TESTS=ON \
    -DDISH_LOCK_STATS="${lock_stats}" \
    -DDISH_SANITIZER="${san}"
cmake --build "${tree}" --parallel --target "${target}"
(
    cd "${tree}"
    QT_QPA_PLATFORM=offscreen \
        ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
        UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
        TSAN_OPTIONS="halt_on_error=1:suppressions=$(pwd)/../tests/tsan.suppressions" \
        ctest --output-on-failure --parallel 1 "${filter[@]+"${filter[@]}"}"
)
