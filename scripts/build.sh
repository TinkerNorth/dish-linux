#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Build Dish on Linux: scripts/build.sh [debug|release|test|clean]
#
# Thin wrapper over the CMake presets in CMakePresets.json, which are the
# single source of configure truth (the same presets linux-ci.yml, codeql.yml
# and release.yml run).
#
#   scripts/build.sh                # release preset -> build-release/
#   scripts/build.sh debug          # debug preset   -> build/
#   scripts/build.sh test           # debug build, then ctest (offscreen)
#   scripts/build.sh clean          # wipe the build directories
#
# Directory note: the debug preset writes to build/ (CI's tree name), not the
# build-debug/ this script used before the presets existed. Ninja is required
# (the presets pin CI's generator); scripts/install-deps.sh installs it.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

config="release"
run_tests=0

for arg in "$@"; do
    case "${arg}" in
        debug|Debug)     config="debug" ;;
        release|Release) config="release" ;;
        # Tests are a debug concern: an assertion is worth more than the
        # optimizer here. (CI also tests Release; ci-local.sh covers that.)
        test|tests)      run_tests=1; config="debug" ;;
        clean)
            rm -rf build build-release build-package build-debug build-tidy build-appimage build-san-*
            echo "removed build directories"
            exit 0
            ;;
        -h|--help)
            sed -n '5,19p' "${BASH_SOURCE[0]}"
            exit 0
            ;;
        *)
            echo "unknown argument: ${arg}" >&2
            exit 1
            ;;
    esac
done

if ! command -v ninja >/dev/null 2>&1; then
    echo "ninja not found; the presets pin CI's Ninja generator. Run scripts/install-deps.sh." >&2
    exit 1
fi

cmake --preset "${config}"
cmake --build --preset "${config}" --parallel

if [[ "${run_tests}" -eq 1 ]]; then
    # QT_QPA_PLATFORM=offscreen comes from the test preset: no display in a
    # bare shell either, and the QML tests construct QGuiApplication.
    ctest --preset "${config}" --parallel
fi

echo
case "${config}" in
    debug)   echo "built build/dish (Debug)" ;;
    release) echo "built build-release/dish (Release)" ;;
esac
