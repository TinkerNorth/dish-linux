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
# Coverage is ENFORCED, not reported. An untranslated string does not fail a
# build, it just silently ships in English to someone who does not read it, and
# nothing else in the suite can see that. Every catalogue must be complete.
#
# The source catalogue (dish_en.ts) is seeded from its own source strings first
# — see scripts/seed-source-language.py for why that is not busywork — so the
# count means the same thing in all six and the gate is one rule, not five
# languages plus an exception.

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

# Before 6.9, lupdate resolves a class whose definition and member bodies live
# in different files to a bare class name, dropping the namespace. It would
# rewrite the dish::net::WifiConnectionManager context to WifiConnectionManager
# — not what moc hands tr() at run time, so the entry could never be looked up.
# Since this script rewrites the catalogues in place, refuse rather than let a
# distro lupdate quietly do that. CI pins the version in
# .github/actions/setup-qt.
lupdate_version="$("$lupdate" -version 2>/dev/null || true)"
if [[ ! "$lupdate_version" =~ ([0-9]+)\.([0-9]+) ]]; then
    echo "could not read a version out of '$lupdate -version'." >&2
    exit 1
fi
lupdate_major="${BASH_REMATCH[1]}"
lupdate_minor="${BASH_REMATCH[2]}"
if (( lupdate_major < 6 || (lupdate_major == 6 && lupdate_minor < 9) )); then
    echo "lupdate ${lupdate_major}.${lupdate_minor} is too old; this gate needs 6.9 or newer." >&2
    echo "It would drop the namespace off a context and rewrite catalogues that" >&2
    echo "no longer match what tr() looks up. Point LUPDATE at a newer Qt." >&2
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

# The source catalogue answers for itself; every other language needs words.
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 not found; scripts/seed-source-language.py needs it." >&2
    exit 1
fi
python3 scripts/seed-source-language.py translations/dish_en.ts

if ! git diff --quiet -- translations/; then
    echo
    echo "Translation catalogues are out of date. Run:"
    echo "    scripts/check-translations.sh && git add translations/"
    echo
    git --no-pager diff --stat -- translations/
    exit 1
fi

echo
incomplete=0
for ts in translations/dish_*.ts; do
    total=$(grep -c '<message' "$ts" || true)
    unfinished=$(grep -c 'type="unfinished"' "$ts" || true)
    printf '%-28s %s/%s translated\n' "$(basename "$ts")" "$((total - unfinished))" "$total"
    [[ "$unfinished" -eq 0 ]] || incomplete=1
done

if [[ "$incomplete" -ne 0 ]]; then
    echo
    echo "Untranslated strings remain. Every catalogue must be complete before"
    echo "merge: a missing translation is not a blank, it is English shown to"
    echo "someone who does not read English."
    echo
    echo "Sources still needing words (the union across catalogues):"
    # -B4 spans the <source> above each unfinished <translation>. Written to a
    # variable rather than piped into head, so a short read cannot SIGPIPE the
    # script out from under `set -o pipefail`.
    missing="$(grep -h -B4 'type="unfinished"' translations/dish_*.ts |
        sed -n 's,.*<source>\(.*\)</source>.*,\1,p' | sort -u || true)"
    printf '%s\n' "$missing" | sed -n '1,30p' | sed 's,^,  ,'
    total_missing="$(printf '%s\n' "$missing" | grep -c . || true)"
    if [[ "$total_missing" -gt 30 ]]; then
        echo "  … and $((total_missing - 30)) more"
    fi
    exit 1
fi

echo
echo "Translation catalogues are in sync, and every catalogue is complete."
