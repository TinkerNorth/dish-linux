#!/usr/bin/env bash
# sign-rpm.sh: sign each given .rpm with the repository key, in place, and
# check the signature took.
#
#   GPG_KEY_ID=<fingerprint> sign-rpm.sh <file.rpm> ...
#
# dnf refuses a package from a repository whose .repo sets gpgcheck=1 unless
# the package itself carries a signature by a key the .repo names; a signed
# repomd.xml is not enough ("OpenPGP check for package ... has failed: The
# package is not signed"). release.yml's rpm job signs the .rpm before it is
# hardened and uploaded, so the downloadable file and the repository's copy
# are the same bytes; build-dnf-repo.sh signs anything still unsigned and
# refuses to index a package signed by another key.
#
# Requires: rpmsign (Fedora: rpm-sign; Debian/Ubuntu: rpm), gpg with
# $GPG_KEY_ID's secret key imported and no passphrase.
set -euo pipefail

if [ -z "${GPG_KEY_ID:-}" ]; then
    echo "GPG_KEY_ID must be set (the signing key's fingerprint)." >&2
    exit 2
fi
if [ "$#" -eq 0 ]; then
    echo "Usage: $0 <file.rpm> ..." >&2
    exit 2
fi
for tool in rpmsign rpm gpg; do
    command -v "$tool" >/dev/null 2>&1 || { echo "$tool not found." >&2; exit 2; }
done

# The key id rpm prints is the fingerprint's last 16 hex digits, lowercase.
key_id="$(printf '%s' "${GPG_KEY_ID}" | tr -d ' ' | tr 'A-F' 'a-f')"
key_id="${key_id: -16}"

signer_of() {
    rpm -qp --qf '%|RSAHEADER?{%{RSAHEADER:pgpsig}}:{%|DSAHEADER?{%{DSAHEADER:pgpsig}}:{(none)}|}|' "$1" 2>/dev/null
}

for pkg in "$@"; do
    if signer_of "$pkg" | grep -qi "key id ${key_id}"; then
        echo "= $(basename "$pkg") is already signed by ${key_id}"
        continue
    fi
    rpmsign --addsign \
        --define "_gpg_name ${GPG_KEY_ID}" \
        --define "_gpg_sign_cmd_extra_args --batch --pinentry-mode loopback" \
        "$pkg"
    signer_of "$pkg" | grep -qi "key id ${key_id}" \
        || { echo "::error::$(basename "$pkg") carries no signature by ${key_id} after signing" >&2; exit 1; }
    echo "+ signed $(basename "$pkg"): $(signer_of "$pkg")"
done
