#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Fails a build that hard-codes a design value in a QML page. A page that writes
# `#4FE3FF`, `radius: 8` or `font.pixelSize: 11` has forked the design system
# silently: it still renders, it just stops following the palette and the scale.
#
# Scope:
#   * src/qml/kit/**    SKIPPED. The kit is the layer that TURNS tokens into
#                       pixels; a token defined in terms of itself is not one.
#   * src/qml/wizard/** and src/qml/shared/**  ERRORS in error mode. They were
#                       written against the finished token surface.
#   * everything else outside the kit  WARNS. Those files predate the token
#                       surface; promoting them to error is the recorded
#                       follow-up.
#
# Only errors set the exit code, so CI runs in error mode while the warnings
# stay informational.
#
# Usage: scripts/qml-lint-literals.sh [--mode error|warn]

set -euo pipefail

mode="error"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode)
            mode="${2:-}"
            shift 2
            ;;
        --mode=*)
            mode="${1#*=}"
            shift
            ;;
        -h|--help)
            sed -n '3,22p' "$0"
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [[ "$mode" != "error" && "$mode" != "warn" ]]; then
    echo "--mode must be error or warn" >&2
    exit 2
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

# git ls-files, not find: it settles tracked-ness and keeps build-tree copies of
# a page out of the scan.
mapfile -t files < <(git ls-files 'src/qml/*.qml')

# Deliberately narrow: these match a literal ASSIGNED to a design property, not
# any number, so `width: parent.width - 8` stays quiet.
rule_names=(
    'raw colour literal'
    'Qt.rgba() colour'
    'hard-coded font.pixelSize'
    'hard-coded radius'
    'hard-coded spacing metric'
    'hard-coded font.family'
    'hand-rolled disabled opacity'
)
rule_patterns=(
    '#[0-9A-Fa-f]{3,8}\b'
    'Qt\.rgba\('
    'font\.pixelSize[[:space:]]*:[[:space:]]*[0-9]'
    'radius[[:space:]]*:[[:space:]]*[0-9]'
    '(^|[^A-Za-z0-9_.])(spacing|padding|leftPadding|rightPadding|topPadding|bottomPadding|margins|leftMargin|rightMargin|topMargin|bottomMargin)[[:space:]]*:[[:space:]]*[0-9]'
    'font\.family[[:space:]]*:[[:space:]]*"'
    'opacity[[:space:]]*:[[:space:]]*0\.4'
)

error_count=0
warn_count=0

for file in "${files[@]}"; do
    # Exempt by design, not by omission: see the kit note in the header.
    [[ "$file" == src/qml/kit/* ]] && continue
    # A tracked path can be missing from the worktree (unstaged deletion,
    # half-applied rebase); a lint scan is not the place to fail on that.
    [[ -f "$file" ]] || continue

    strict=0
    if [[ "$mode" == "error" ]]; then
        case "$file" in
            src/qml/wizard/*|src/qml/shared/*) strict=1 ;;
        esac
    fi

    line_number=0
    while IFS= read -r line; do
        line_number=$((line_number + 1))
        # A token table written in a header comment must not fail a build.
        [[ "$line" =~ ^[[:space:]]*(//|\*|/\*) ]] && continue

        for i in "${!rule_patterns[@]}"; do
            if [[ "$line" =~ ${rule_patterns[$i]} ]]; then
                message="$file:$line_number: ${rule_names[$i]} -- use a Theme/Tokens name"
                if [[ $strict -eq 1 ]]; then
                    echo "ERROR $message"
                    error_count=$((error_count + 1))
                else
                    echo "warn  $message"
                    warn_count=$((warn_count + 1))
                fi
            fi
        done
    done < "$file"
done

echo
echo "qml-lint-literals: $error_count error(s), $warn_count warning(s) over ${#files[@]} tracked QML file(s)."
if [[ $error_count -gt 0 ]]; then
    echo "The wizard and shared page-model directories may not hard-code a design value."
    exit 1
fi
exit 0
