#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Asserts the linker applied full RELRO, BIND_NOW, a non-executable stack and
# PIE to a built binary:
#
#   scripts/check-hardening.sh [binary]        (default build-release/dish)
#
# Ubuntu patches PIE, partial RELRO and stack protection into its gcc defaults,
# so these pass by accident on a CI runner and would silently stop passing
# anywhere else. linux-ci.yml and scripts/ci-local.sh both run this.
set -euo pipefail
cd "$(dirname "$0")/.."

bin="${1:-build-release/dish}"
fail() {
    echo "::error::${bin}: $1" >&2
    exit 1
}
readelf -lW "${bin}" | grep -q 'GNU_RELRO' || fail "no RELRO"
readelf -dW "${bin}" | grep -qE 'BIND_NOW|FLAGS.*NOW' || fail "no full RELRO"
readelf -lW "${bin}" | grep -qE 'GNU_STACK.*RW ' || fail "executable stack"
readelf -hW "${bin}" | grep -q 'Type:.*DYN' || fail "not PIE"
echo "hardening: OK (${bin})"
