#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Builds the app with the Sentry SDK the way a release does, against a DSN
# that resolves nowhere, and checks the SDK is linked and the binary carries
# the debug info the symbol upload reads. The SDK is only fetched and built
# when a DSN is present, which no pull request has, so this is the one lane
# that compiles and links the Sentry path before a tag does. linux-ci.yml's
# sentry-sdk job and scripts/ci-local.sh --with-sentry both run it. Needs
# libcurl's development files (sentry-native's transport).
set -euo pipefail
cd "$(dirname "$0")/.."

cmake --preset release -B build-sentry -DDISH_SENTRY_DSN="https://public@o0.ingest.sentry.invalid/0"
cmake --build build-sentry --target Dish --parallel
bin="$(find build-sentry -maxdepth 2 -type f -name dish -perm -u+x -print -quit)"
[ -n "${bin}" ] || { echo "::error::no dish binary built" >&2; exit 1; }
nm "${bin}" | grep -q 'sentry_init' || { echo "::error::sentry_init is not linked" >&2; exit 1; }
readelf -S "${bin}" | grep -q '\.debug_info' || { echo "::error::no debug info for the symbol upload" >&2; exit 1; }
echo "Sentry build: OK (${bin})"
