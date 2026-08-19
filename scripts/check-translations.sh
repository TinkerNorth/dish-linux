#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Fails a build whose translation catalogues no longer match the source. A stale
# .ts is not a compile error, so without this check a build stays green while
# every new string ships in English to non-English users.
#
# Re-runs lupdate with the same flags CMake uses, then asks git whether anything
# changed. A dirty tree means a string was added, edited or deleted without
# refreshing the catalogues, and the fix is the line the failure prints.
#
# NOTE: this REWRITES translations/*.ts in your working tree. It refuses to run
# if they are already dirty, so nothing of yours is lost, but expect modified
# files afterwards.
#
# Coverage is reported, never enforced: translating a string is a separate act
# from extracting it, and a gate that waits for the words would just get routed
# around.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

lupdate="${LUPDATE:-}"
if [[ -z "$lupdate" ]]; then
    for candidate in lupdate-qt6 lupdate6 lupdate; do
        if command -v "$candidate" >/dev/null 2>&1; then
            lupdate="$candidate"
            break
        fi
    done
fi
if [[ -z "$lupdate" ]]; then
    echo "lupdate not found. Install qt6-l10n-tools, or set LUPDATE." >&2
    exit 1
fi

if ! git diff --quiet -- translations/; then
    echo "translations/ is already dirty; commit or stash first." >&2
    exit 1
fi

# Keep identical to the CMake invocation. On drift the gate fails on formatting
# rather than content, and people learn to ignore it.
"$lupdate" -locations none -no-obsolete \
    -recursive src \
    -ts translations/dish_en.ts translations/dish_bs.ts translations/dish_de.ts \
       translations/dish_es.ts translations/dish_fr.ts translations/dish_pt_BR.ts

if ! git diff --quiet -- translations/; then
    echo
    echo "Translation catalogues are out of date. Run:"
    echo "    scripts/check-translations.sh && git add translations/"
    echo
    git --no-pager diff --stat -- translations/
    exit 1
fi

# Reported, never enforced.
echo
for ts in translations/dish_*.ts; do
    total=$(grep -c '<message' "$ts" || true)
    unfinished=$(grep -c 'type="unfinished"' "$ts" || true)
    printf '%-28s %s/%s translated\n' "$(basename "$ts")" "$((total - unfinished))" "$total"
done

echo
echo "Translation catalogues are in sync."
