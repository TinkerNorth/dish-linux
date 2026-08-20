#!/usr/bin/env bash
# Runs every gate Linux CI runs, in the same order, against the local tree.
# Mirrors .github/workflows/linux-ci.yml so a green run here means a green run
# there. clang-tidy is skipped with a notice when not installed, since CI
# provides its own.
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
# No display in a bare shell either; the QML tests construct QGuiApplication.
(cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure --parallel)

step "qmllint (QML static analysis)"
QMLLINT="$(command -v qmllint6 || command -v qmllint || true)"
if [ -n "$QMLLINT" ]; then
  # git's * crosses directory levels; 'src/qml/**/*.qml' would miss the
  # top-level Main and AppShell.
  # shellcheck disable=SC2046
  "$QMLLINT" -I build --unqualified info $(git ls-files 'src/qml/*.qml')
else
  echo "::notice:: qmllint not installed; CI gates this."
fi

step "QML literal scanner"
./scripts/qml-lint-literals.sh --mode error

step "Translation catalogues in sync"
if command -v lupdate6 >/dev/null 2>&1 || command -v lupdate >/dev/null 2>&1; then
  ./scripts/check-translations.sh
else
  echo "::notice:: lupdate not installed; CI gates this."
fi

if [ "$TIDY" -eq 1 ]; then
  if command -v clang-tidy >/dev/null 2>&1; then
    step "clang-tidy (src, UI + qml excluded — mirrors CI)"
    # Same build/ the gates above used: CMakeLists exports the compile database
    # globally, so a second tree would only re-derive the same src/ entries.
    NPROC="$(nproc)"
    TIDY_RC=0
    find src -type f \( -name '*.cpp' -o -name '*.h' \) ! -path 'src/UI/*' ! -path 'src/qml/*' -print0 |
      xargs -0 -n1 -P"${NPROC}" clang-tidy -p build --quiet --warnings-as-errors='*' || TIDY_RC=$?
    if [ "$TIDY_RC" -ne 0 ]; then exit "$TIDY_RC"; fi
  else
    echo ""; echo "=== clang-tidy skipped (not installed) ==="
  fi
fi

step "Configure + build (Release)"
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DDISH_BUILD_TESTS=OFF
cmake --build build-release --parallel --target Dish

echo ""
echo "All local CI gates passed."
