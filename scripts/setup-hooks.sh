#!/usr/bin/env bash
# Point this repo's git hooks at the tracked .githooks/ directory so the
# pre-commit lint/format checks run for every contributor after a single
# one-time setup. Idempotent — safe to re-run.
set -euo pipefail

cd "$(dirname "$0")/.."

if [ ! -d .git ]; then
    echo "✗ not a git repository (run 'git init' first, or run from inside the repo)"
    exit 1
fi

git config core.hooksPath .githooks
chmod +x .githooks/*

echo "✓ core.hooksPath → .githooks"
echo
echo "Recommended tooling (install once):"
echo "  Debian/Ubuntu:  sudo apt install clang-format clang-tidy"
echo "  Fedora:         sudo dnf install clang-tools-extra"
echo "  Arch:           sudo pacman -S clang"
echo
echo "Note: clang-tidy needs build-debug/compile_commands.json — generate it with:"
echo "  scripts/build.sh debug"
