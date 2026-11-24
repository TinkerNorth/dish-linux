#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# clang-format gate: the exact file set and invocation linux-ci.yml runs. One
# script so the workflow and scripts/ci-local.sh cannot drift on the file set
# or the invocation. Check-only; the pre-commit hook is the autofix path.
#
# CI pins clang-format at DISH_CLANG_FORMAT_VERSION in scripts/tools.sh (PyPI
# via pipx), and scripts/ci-local.sh refuses another version: a different
# clang-format disagrees on layout.
set -euo pipefail
cd "$(dirname "$0")/.."

find src tests tools -type f \( -name '*.cpp' -o -name '*.h' \) \
  -print0 | xargs -0 clang-format --dry-run --Werror
echo "clang-format: OK"
