#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Runs the gates Linux CI runs, in the same order, against the local tree, so a
# green run here means a green run there. Mirrors .github/workflows/linux-ci.yml
# via the same CMakePresets.json presets the workflow drives.
#
#   scripts/ci-local.sh                    every ci-job gate
#   scripts/ci-local.sh --no-tidy          skip clang-tidy (fastest loop)
#   scripts/ci-local.sh --with-package     also build and lint the .deb
#   scripts/ci-local.sh --with-sanitizers  also run the ASan/UBSan and TSan legs
#   scripts/ci-local.sh --allow-missing    downgrade a missing tool to a notice
#   scripts/ci-local.sh --compiler clang   set CC/CXX for this run
#
# Without --allow-missing a gate whose tool is absent FAILS rather than printing
# a notice and continuing: a "green" run that silently skipped four gates is
# worse than no run at all.
#
# One run covers one compiler. CI's ci job is a gcc/clang matrix (the lint
# steps run on gcc; the clang leg exists because -Wconversion, -Wshadow and
# -Wold-style-cast diverge materially); reproduce the other leg with
# --compiler clang. The optional sanitizer and package legs run after the core
# ci-job mirror, in the order CI's parallel jobs are listed.
set -euo pipefail
cd "$(dirname "$0")/.."

TIDY=1
PACKAGE=0
SANITIZE=0
ALLOW_MISSING=0
COMPILER=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --no-tidy) TIDY=0 ;;
    --with-package) PACKAGE=1 ;;
    --with-sanitizers) SANITIZE=1 ;;
    --allow-missing|--allow-missing-tools) ALLOW_MISSING=1 ;;
    --compiler)
      shift
      COMPILER="${1:-}"
      ;;
    --compiler=*) COMPILER="${1#--compiler=}" ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
  shift
done

case "$COMPILER" in
  "") ;;
  gcc)   export CC=gcc   CXX=g++     ;;
  clang) export CC=clang CXX=clang++ ;;
  *) echo "--compiler takes gcc or clang (got '$COMPILER')" >&2; exit 2 ;;
esac

step() { echo ""; echo "=== $1 ==="; }

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

step "clang-format (check only)"
if have clang-format; then
  # CI pins 22.1.4; Ubuntu's 18 disagrees on braced-init lists, which is why the
  # pin exists. Treat a surprise from another version with suspicion.
  want=22.1.4
  got="$(clang-format --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
  if [ "$got" != "$want" ]; then
    echo "::notice:: clang-format $got, CI pins $want — disagreements are the version, not the code."
  fi
  bash scripts/check-format.sh
fi

step "Action pin lint (40-char SHA required)"
# The same awk _security.yml runs, over the same two directories.
fail=0
while IFS= read -r -d '' file; do
  awk '
    /^[[:space:]]*#/ { next }
    { sub(/[[:space:]]+#.*$/, "", $0) }
    /^[[:space:]]*-?[[:space:]]*uses:[[:space:]]+[^[:space:]]+/ {
      line = $0
      sub(/^[[:space:]]*-?[[:space:]]*uses:[[:space:]]+/, "", line)
      sub(/[[:space:]]+$/, "", line)
      if (line ~ /^\.\//) { next }
      if (line ~ /^docker:\/\/[^@]+@sha256:[0-9a-f]{64}$/) { next }
      if (line !~ /@[0-9a-f]{40}$/) { printf "%s: %s\n", FILENAME, line; exit 2 }
      if (line ~ /@0{40}$/) { printf "%s: %s (all-zero placeholder)\n", FILENAME, line; exit 2 }
    }
  ' "$file" || fail=1
done < <(find .github/workflows .github/actions -type f \( -name '*.yml' -o -name '*.yaml' \) -print0)
[ "$fail" -eq 0 ] || { echo "unpinned action reference" >&2; exit 1; }
echo "action pins: OK"

step "Configure (Debug, preset debug)"
cmake --preset debug

step "Build"
cmake --build --preset debug --parallel

step "Run tests (Debug)"
# QT_QPA_PLATFORM=offscreen comes from the test preset: no display in a bare
# shell either, and the QML tests construct QGuiApplication.
ctest --preset debug --parallel

step "qmllint (QML static analysis)"
if have qmllint; then
  # git's * crosses directory levels; 'src/qml/**/*.qml' would miss the
  # top-level Main and AppShell. -I "${QT_ROOT_DIR}/qml" matches CI (setup-qt
  # exports QT_ROOT_DIR); without it a distro qmllint may resolve Qt's own
  # modules differently than CI's.
  qt_qml_args=()
  if [ -n "${QT_ROOT_DIR:-}" ] && [ -d "${QT_ROOT_DIR}/qml" ]; then
    qt_qml_args=(-I "${QT_ROOT_DIR}/qml")
  fi
  # shellcheck disable=SC2046
  qmllint -I build "${qt_qml_args[@]+"${qt_qml_args[@]}"}" --unqualified info \
    $(git ls-files 'src/qml/*.qml')
fi

step "QML literal scanner"
./scripts/qml-lint-literals.sh --mode error

step "Translation catalogues in sync"
if have lupdate; then ./scripts/check-translations.sh; fi

if [ "$TIDY" -eq 1 ]; then
  step "clang-tidy (src, UI + qml excluded — mirrors CI)"
  if have clang-tidy; then
    # Same build/ the gates above used: CMakeLists exports the compile database
    # globally, so a second tree would only re-derive the same src/ entries.
    # --warnings-as-errors keeps the sweep-clean check set gated without
    # forking the fleet-canonical .clang-tidy (WarningsAsErrors: '').
    find src -type f \( -name '*.cpp' -o -name '*.h' \) \
      ! -path 'src/UI/*' \
      ! -path 'src/qml/*' \
      -print0 | xargs -0 -n1 -P"$(nproc)" \
        clang-tidy -p build --quiet --warnings-as-errors='*'
  fi
fi

step "Configure + build + test (Release)"
# -O3 plus LTO is a different compiler: undefined behaviour that Debug
# tolerates surfaces here, and the suite must pass against the configuration
# that actually ships.
cmake --preset release
cmake --build --preset release --parallel
ctest --preset release --parallel

step "Hardening flags reached the binary"
readelf -lW  build-release/dish | grep -q 'GNU_RELRO'         || { echo "no RELRO" >&2; exit 1; }
readelf -dW  build-release/dish | grep -qE 'BIND_NOW|FLAGS.*NOW' || { echo "no full RELRO" >&2; exit 1; }
readelf -lW  build-release/dish | grep -qE 'GNU_STACK.*RW '   || { echo "executable stack" >&2; exit 1; }
readelf -hW  build-release/dish | grep -q 'Type:.*DYN'        || { echo "not PIE" >&2; exit 1; }

if [ "$SANITIZE" -eq 1 ]; then
  for san in address+undefined thread; do
    step "Sanitizer: ${san}"
    # The sanitizer flag is matrix-injected in CI and deliberately not a
    # preset; keep these flags in step with linux-ci.yml's sanitize job.
    cmake -S . -B "build-san-${san}" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DDISH_BUILD_TESTS=ON -DDISH_SANITIZER="${san}"
    # DishTests only: the Qt Quick app target adds qmlcachegen output where GCC
    # refuses atomic_thread_fence under TSan, and the suite never runs it.
    cmake --build "build-san-${san}" --parallel --target DishTests
    ( cd "build-san-${san}" && \
      QT_QPA_PLATFORM=offscreen \
      ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
      UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
      TSAN_OPTIONS="halt_on_error=1:suppressions=$(pwd)/../tests/tsan.suppressions" \
      ctest --output-on-failure --parallel 1 )
  done
fi

if [ "$PACKAGE" -eq 1 ]; then
  step "Packaging metadata"
  if have desktop-file-validate; then desktop-file-validate packaging/dish.desktop; fi
  if have appstreamcli; then
    appstreamcli validate --no-net packaging/com.tinkernorth.Dish.metainfo.xml
  fi

  step "Build and lint the .deb"
  # CI does this in a debian:trixie container against Debian's Qt; locally it
  # still proves the CPack wiring, the install layout and the lintian tags.
  if have dpkg-deb && have cpack; then
    rm -rf build-package/packages
    bash scripts/build-deb.sh
    if have lintian; then
      lintian --fail-on error --tag-display-limit 0 build-package/packages/*.deb
    fi
    dpkg-deb -c build-package/packages/*.deb | grep -E 'udev/rules.d|metainfo|applications|copyright'
  fi
fi

echo ""
echo "All local CI gates passed."
