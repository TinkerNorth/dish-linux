#!/usr/bin/env bash
# Thin forwarder kept for muscle memory: the CI-parity runner now lives at
# scripts/ci-local.sh (the fleet-wide name; satellite and dish-windows carry
# the same contract). Same flags: --no-tidy --with-package --with-sanitizers
# --allow-missing --compiler gcc|clang.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/ci-local.sh" "$@"
