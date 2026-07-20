#!/usr/bin/env bash
# Runs every gate Linux CI runs, in the same order, against the local tree.
# Mirrors .github/workflows/linux-ci.yml so a green run here means a green run
# there. Works on Linux and macOS (Homebrew Qt6/SDL2/libsodium); clang-tidy is
# skipped with a notice when not installed since CI provides its own.
#
#   scripts/ci_local.sh              all gates
#   scripts/ci_local.sh --no-tidy    skip the clang-tidy pass (fastest loop)
set -euo pipefail
cd "$(dirname "$0")/.."

TIDY=1
for arg in "$@"; do
  case "$arg" in
    --no-tidy) TIDY=0 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

step() { echo ""; echo "=== $1 ==="; }

step "clang-format (check only)"
if command -v clang-format >/dev/null 2>&1; then
  # CI pins clang-format 22.1.4; other versions may disagree on braced-init
  # lists. The check still runs — treat surprises against a different local
  # version with suspicion before blaming the code.
  find src tests -type f \( -name '*.cpp' -o -name '*.h' \) -print0 |
    xargs -0 clang-format --dry-run --Werror
else
  echo "::notice:: clang-format not installed; CI pins 22.1.4. Skipping locally."
fi

step "Configure (Debug, tests on)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDISH_BUILD_TESTS=ON

step "Build"
cmake --build build --parallel

step "Run tests"
(cd build && ctest --output-on-failure --parallel)

if [ "$TIDY" -eq 1 ]; then
  if command -v clang-tidy >/dev/null 2>&1; then
    step "clang-tidy (src, UI excluded — mirrors CI)"
    cmake -S . -B build-tidy -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DDISH_BUILD_TESTS=OFF
    cmake --build build-tidy --parallel
    NPROC=$( (command -v nproc >/dev/null && nproc) || sysctl -n hw.ncpu )
    # On macOS, Homebrew's /usr/local/include loses its implicit-system
    # status once another dep adds it with -I, so project warnings leak into
    # SDL headers and tidy false-positives. Linux CI is the authoritative
    # tidy gate; Darwin runs it advisory.
    TIDY_RC=0
    find src -type f \( -name '*.cpp' -o -name '*.h' \) ! -path 'src/UI/*' -print0 |
      xargs -0 -n1 -P"${NPROC}" clang-tidy -p build-tidy --quiet --warnings-as-errors='*' || TIDY_RC=$?
    if [ "$TIDY_RC" -ne 0 ]; then
      if [ "$(uname -s)" = "Darwin" ]; then
        echo "::notice:: clang-tidy reported issues (advisory on macOS — Linux CI gates this)."
      else
        exit "$TIDY_RC"
      fi
    fi
  else
    echo ""; echo "=== clang-tidy skipped (not installed) ==="
  fi
fi

step "Configure + build (Release)"
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DDISH_BUILD_TESTS=OFF
cmake --build build-release --parallel --target Dish

echo ""
echo "All local CI gates passed."
