#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Runs the gates Linux CI runs, in the same order, against the local tree, so a
# green run here means a green run there. Every gate is a script under
# scripts/ that the workflow step calls too, and the configure lines are the
# CMakePresets.json presets the workflow drives.
#
#   scripts/ci-local.sh                     every gate of linux-ci.yml's ci job
#   scripts/ci-local.sh --quick             skip clang-tidy and the Release leg (fastest loop)
#   scripts/ci-local.sh --with-sanitizers   also the ASan+UBSan and TSan legs, lock laws included
#   scripts/ci-local.sh --with-lock-stats   also the lock-statistics build
#   scripts/ci-local.sh --with-coverage     also the lcov build
#   scripts/ci-local.sh --with-sentry       also the build with the Sentry SDK
#   scripts/ci-local.sh --with-package      also the .deb and .rpm lanes, in their
#                                           containers (scripts/package-in-container.sh;
#                                           needs Docker)
#   scripts/ci-local.sh --allow-missing     downgrade a missing tool to a notice
#   scripts/ci-local.sh --compiler clang    set CC/CXX for this run (gcc or clang)
#
# Without --allow-missing a gate whose tool is absent FAILS rather than printing
# a notice and continuing: a "green" run that silently skipped four gates is
# worse than no run at all.
#
# One run covers one compiler. CI's ci job is a gcc/clang matrix (the lint
# steps run on gcc; the clang leg exists because -Wconversion, -Wshadow and
# -Wold-style-cast diverge materially); reproduce the other leg with
# --compiler clang.
#
# The security gates run here through the same shared scripts _security.yml
# runs (check-action-pins.sh, security-allowlist.py, osv-scan.sh) and the same
# gitleaks command, with OSV-Scanner and gitleaks at the versions
# scripts/tools.sh pins (Linux x86_64 and macOS arm64), fetched into
# build/tools/. Python 3.11+ with PyYAML.
#
# CI-only, with the reason:
#   dependency review (security.yml): a GitHub API over the pull request's
#     dependency graph; off until the repository's Dependency graph setting is.
#   CodeQL (codeql.yml): GitHub's analyzer, not installable as a gate.
#   the AppImage and its dish-bin package (appimage-ci.yml): an x86_64 jammy
#     host; locally, scripts/package-in-container.sh arch takes a built AppImage.
#   the Flatpak (flatpak-ci.yml): scripts/build-flatpak.sh, needs flatpak-builder.
#   the APT and DNF repositories (linux-ci.yml's repos job): run them with
#     scripts/package-in-container.sh repos <dir> on the .deb and .rpm that
#     --with-package leaves under dist/container/.
#   measurements.yml: tools/bench, see tools/bench/README.md.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

QUICK=0
PACKAGE=0
LOCK_STATS=0
SANITIZE=0
COVERAGE=0
SENTRY=0
ALLOW_MISSING=0
COMPILER=""
while [ "$#" -gt 0 ]; do
    case "$1" in
    --quick) QUICK=1 ;;
    --with-package) PACKAGE=1 ;;
    --with-lock-stats) LOCK_STATS=1 ;;
    --with-sanitizers) SANITIZE=1 ;;
    --with-coverage) COVERAGE=1 ;;
    --with-sentry) SENTRY=1 ;;
    --allow-missing) ALLOW_MISSING=1 ;;
    --compiler)
        shift
        COMPILER="${1:-}"
        ;;
    --compiler=*) COMPILER="${1#--compiler=}" ;;
    *)
        echo "unknown flag: $1" >&2
        exit 2
        ;;
    esac
    shift
done

case "$COMPILER" in
"") ;;
gcc) export CC=gcc CXX=g++ ;;
clang) export CC=clang CXX=clang++ ;;
*)
    echo "--compiler takes gcc or clang (got '$COMPILER')" >&2
    exit 2
    ;;
esac

step() {
    echo ""
    echo "=== $1 ==="
}

# Returns 0 when the caller should run the gate, 1 when it was skipped by
# permission, and exits when a tool CI gates on is missing.
have() {
    local tool="$1"
    if command -v "$tool" >/dev/null 2>&1; then return 0; fi
    if [ "$ALLOW_MISSING" -eq 1 ]; then
        echo "::notice:: $tool is not installed; CI gates this. Skipping (--allow-missing)."
        return 1
    fi
    echo "$tool is not installed and CI gates it. Install it (scripts/install-deps.sh), or re-run with --allow-missing." >&2
    exit 1
}

step "Version mirrors and pins"
scripts/check-version-consistency.sh

step "clang-format (check only)"
if have clang-format; then
    # A formatter of another version disagrees on layout, and a green run with
    # it proves nothing about CI's verdict, so a mismatch fails.
    got="$(clang-format --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
    if [ "$got" != "$DISH_CLANG_FORMAT_VERSION" ]; then
        echo "clang-format $got, CI pins $DISH_CLANG_FORMAT_VERSION: pipx install clang-format==$DISH_CLANG_FORMAT_VERSION" >&2
        exit 1
    fi
    scripts/check-format.sh
fi

step "Action pin lint (40-char SHA required)"
# The shared script _security.yml runs, over the same directories.
sh scripts/check-action-pins.sh

step "Vulnerability allowlist expiry check"
python3 scripts/security-allowlist.py check

step "OSV-Scanner (vendored + manifest deps)"
# The pinned binaries from scripts/tools.sh, with the arguments security.yml
# passes _security.yml.
tools_dir="build/tools"
if fetch_security_tools "${tools_dir}"; then
    OSV_SCANNER="${tools_dir}/osv-scanner" \
        bash scripts/osv-scan.sh --lockfile-script scripts/osv-lockfile.sh -- .

    step "Secret scan (gitleaks)"
    "${tools_dir}/gitleaks" git --config=.gitleaks.toml --redact --exit-code=1 .
elif [ "$ALLOW_MISSING" -eq 1 ]; then
    echo "::notice:: no pinned OSV-Scanner or gitleaks for this platform; CI gates both. Skipping (--allow-missing)."
else
    echo "No pinned OSV-Scanner or gitleaks for this platform, and CI gates both. Re-run with --allow-missing." >&2
    exit 1
fi

step "Configure (Debug, preset debug)"
cmake --preset debug

step "Build"
cmake --build --preset debug --parallel

step "Run tests (Debug)"
# QT_QPA_PLATFORM=offscreen comes from the test preset: no display in a bare
# shell either, and the QML tests construct QGuiApplication.
ctest --preset debug --parallel

step "qmllint (QML static analysis)"
if have qmllint; then scripts/check-qml.sh; fi

step "QML literal scanner"
scripts/qml-lint-literals.sh --mode error

step "Translation catalogues in sync"
if have lupdate; then scripts/check-translations.sh; fi

if [ "$QUICK" -eq 0 ]; then
    step "clang-tidy (src, UI + qml excluded)"
    if have clang-tidy; then scripts/check-tidy.sh; fi

    step "Configure + build + test (Release)"
    # -O3 plus LTO is a different compiler: undefined behaviour that Debug
    # tolerates surfaces here, and the suite must pass against the
    # configuration that actually ships. The input-path bench builds here too,
    # for its --smoke.
    cmake --preset release -DDISH_BUILD_BENCH=ON
    cmake --build --preset release --parallel
    ctest --preset release --parallel

    step "Measurements report tests"
    DISH_BENCH_JSON=build-release/tools/bench/bench_input_path_smoke.json \
        python3 -m unittest discover -s tests -p 'test_measurements_report.py'

    step "Hardening flags reached the binary"
    scripts/check-hardening.sh build-release/dish
fi

if [ "$SANITIZE" -eq 1 ]; then
    for san in address+undefined thread; do
        step "Sanitizer: ${san}"
        scripts/sanitize.sh "${san}"
        step "Lock laws under sanitizer: ${san}"
        scripts/sanitize.sh "${san}" lock-laws
    done
fi

if [ "$LOCK_STATS" -eq 1 ]; then
    step "Lock statistics and the lock laws"
    scripts/lock-stats.sh
fi

if [ "$PACKAGE" -eq 1 ]; then
    if have docker; then
        step ".deb in ${DISH_DEB_IMAGE}: build, lint, install, launch"
        scripts/package-in-container.sh deb
        step ".rpm in ${DISH_RPM_IMAGE}: build, install, launch"
        scripts/package-in-container.sh rpm
    fi
fi

if [ "$COVERAGE" -eq 1 ]; then
    if have lcov; then
        step "Coverage"
        scripts/coverage.sh
    fi
fi

if [ "$SENTRY" -eq 1 ]; then
    step "Build with the Sentry SDK (dummy DSN)"
    scripts/check-sentry-build.sh
fi

echo ""
echo "All local CI gates passed."
