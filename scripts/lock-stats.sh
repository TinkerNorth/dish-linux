#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# The DISH_LOCK_STATS build: the lock laws (tests/test_lock_laws.cpp), the
# statistics half of tests/test_tracked_mutex.cpp, and the whole suite in that
# build, so counting locks is proven to change nothing else. linux-ci.yml's
# lock-stats job and scripts/ci-local.sh --with-lock-stats both run this.
# A failing law prints the set it observed; its list is corrected from that.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset debug -B build-lockstats -DDISH_LOCK_STATS=ON
cmake --build build-lockstats --parallel --target DishTests DishLockLawTests
(cd build-lockstats && QT_QPA_PLATFORM=offscreen ctest --output-on-failure --parallel)
