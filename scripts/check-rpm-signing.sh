#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# The pull-request rehearsal of release.yml's "Sign the .rpm" step, in the
# rpm lane's Fedora container:
#
#   scripts/check-rpm-signing.sh <file.rpm>
#
# Signs a copy of the package through packaging/repo/sign-rpm.sh with a
# throwaway key, imports that key into rpm's keyring the way dnf imports the
# key a .repo file names, and requires rpmkeys to report the signature OK. The
# package under test is left unsigned; the key dies with the run.
set -euo pipefail
cd "$(dirname "$0")/.."

rpm_file="${1:?usage: $0 <file.rpm>}"
work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

GNUPGHOME="${work}/gnupg"
mkdir -m 700 "${GNUPGHOME}"
export GNUPGHOME
gpg --batch --pinentry-mode loopback --passphrase '' \
    --quick-gen-key 'Dish CI throwaway <ci@tinkernorth.invalid>' ed25519 sign 1d
GPG_KEY_ID="$(gpg --list-keys --with-colons | awk -F: '/^fpr/ { print $10; exit }')"
export GPG_KEY_ID

cp "${rpm_file}" "${work}/"
signed="${work}/$(basename "${rpm_file}")"
packaging/repo/sign-rpm.sh "${signed}"

gpg --batch --armor --export "${GPG_KEY_ID}" > "${work}/key.asc"
rpmkeys --import "${work}/key.asc"
result="$(rpmkeys --checksig "${signed}")"
echo "${result}"
case "${result}" in
*"signatures OK"*) ;;
*) echo "::error::rpmkeys does not accept the signature: ${result}" >&2; exit 1 ;;
esac
echo "::notice::the .rpm signs with an ed25519 key and rpm verifies it"
