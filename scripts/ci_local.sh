#!/usr/bin/env bash
# Runs the gates Linux CI runs, in the same order, against the local tree, so a
# green run here means a green run there. Mirrors .github/workflows/linux-ci.yml.
#
#   scripts/ci_local.sh                    every gate
#   scripts/ci_local.sh --no-tidy          skip clang-tidy (fastest loop)
#   scripts/ci_local.sh --with-package     also build and lint the .deb
#   scripts/ci_local.sh --allow-missing    downgrade a missing tool to a notice
#
# Without --allow-missing a gate whose tool is absent FAILS rather than printing
# a notice and continuing: a "green" run that silently skipped four gates is
# worse than no run at all.
set -euo pipefail
cd "$(dirname "$0")/.."

TIDY=1
PACKAGE=0
ALLOW_MISSING=0
for arg in "$@"; do
  case "$arg" in
    --no-tidy) TIDY=0 ;;
    --with-package) PACKAGE=1 ;;
    --allow-missing|--allow-missing-tools) ALLOW_MISSING=1 ;;
    *) echo "unknown flag: $arg" >&2; exit 2 ;;
  esac
done

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
  echo "$tool is not installed and CI gates it. Install it, or re-run with --allow-missing." >&2
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
  find src tests -type f \( -name '*.cpp' -o -name '*.h' \) -print0 |
    xargs -0 clang-format --dry-run --Werror
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

step "Configure (Debug, tests on)"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDISH_BUILD_TESTS=ON

step "Build"
cmake --build build --parallel

step "Run tests (Debug)"
# No display in a bare shell either; the QML tests construct QGuiApplication.
(cd build && QT_QPA_PLATFORM=offscreen ctest --output-on-failure --parallel)

step "qmllint (QML static analysis)"
if have qmllint; then
  # git's * crosses directory levels; 'src/qml/**/*.qml' would miss the
  # top-level Main and AppShell.
  # shellcheck disable=SC2046
  qmllint -I build --unqualified info $(git ls-files 'src/qml/*.qml')
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
    find src -type f \( -name '*.cpp' -o -name '*.h' \) ! -path 'src/UI/*' ! -path 'src/qml/*' -print0 |
      xargs -0 -n1 -P"$(nproc)" clang-tidy -p build --quiet --warnings-as-errors='*'
  fi
fi

step "Configure + build + test (Release)"
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DDISH_BUILD_TESTS=ON
cmake --build build-release --parallel
(cd build-release && QT_QPA_PLATFORM=offscreen ctest --output-on-failure --parallel)

step "Hardening flags reached the binary"
readelf -lW  build-release/dish | grep -q 'GNU_RELRO'         || { echo "no RELRO" >&2; exit 1; }
readelf -dW  build-release/dish | grep -qE 'BIND_NOW|FLAGS.*NOW' || { echo "no full RELRO" >&2; exit 1; }
readelf -lW  build-release/dish | grep -qE 'GNU_STACK.*RW '   || { echo "executable stack" >&2; exit 1; }
readelf -hW  build-release/dish | grep -q 'Type:.*DYN'        || { echo "not PIE" >&2; exit 1; }

if [ "$PACKAGE" -eq 1 ]; then
  step "Packaging metadata"
  if have desktop-file-validate; then desktop-file-validate packaging/dish.desktop; fi
  if have appstreamcli; then
    appstreamcli validate --no-net packaging/com.tinkernorth.Dish.metainfo.xml
  fi

  step "Build and lint the .deb"
  # A package built against a Qt below the 6.7 floor is not what ships; CI does
  # this in a debian:trixie container. Locally it still proves the CPack wiring,
  # the install layout and the lintian tags.
  if have dpkg-deb && have cpack; then
    cmake -S . -B build-package -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DDISH_BUILD_TESTS=OFF
    cmake --build build-package --parallel
    rm -rf build-package/packages
    cpack --config build-package/CPackConfig.cmake -G DEB -B build-package/packages
    if have lintian; then
      lintian --fail-on error --tag-display-limit 0 build-package/packages/*.deb
    fi
    dpkg-deb -c build-package/packages/*.deb | grep -E 'udev/rules.d|metainfo|applications|copyright'
  fi
fi

echo ""
echo "All local CI gates passed."
