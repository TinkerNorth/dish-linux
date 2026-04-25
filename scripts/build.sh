#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Convenience wrapper around cmake/ninja for local development.
#
# Usage:
#   scripts/build.sh                # release build into ./build
#   scripts/build.sh debug          # debug build into ./build-debug
#   scripts/build.sh release test   # release build then run ctest
#   scripts/build.sh clean          # wipe all build directories

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

config="release"
run_tests=0

for arg in "$@"; do
    case "${arg}" in
        debug|Debug)     config="debug" ;;
        release|Release) config="release" ;;
        test|tests)      run_tests=1 ;;
        clean)
            rm -rf build build-debug build-release build-tidy
            echo "removed build directories"
            exit 0
            ;;
        -h|--help)
            sed -n '1,15p' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *)
            echo "unknown argument: ${arg}" >&2
            exit 1
            ;;
    esac
done

case "${config}" in
    debug)   cmake_type="Debug";   build_dir="build-debug" ;;
    release) cmake_type="Release"; build_dir="build" ;;
esac

generator="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
    generator="Ninja"
fi

cmake -S . -B "${build_dir}" -G "${generator}" \
    -DCMAKE_BUILD_TYPE="${cmake_type}" \
    -DDISH_BUILD_TESTS=ON

cmake --build "${build_dir}" --parallel

if [[ "${run_tests}" -eq 1 ]]; then
    (cd "${build_dir}" && ctest --output-on-failure --parallel)
fi

echo
echo "built ${build_dir}/dish (${cmake_type})"
