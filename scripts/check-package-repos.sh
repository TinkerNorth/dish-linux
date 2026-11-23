#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# The pull-request rehearsal of release.yml's apt-publish and rpm-publish
# jobs:
#
#   scripts/check-package-repos.sh <staging dir>
#
# <staging dir> holds a dish_<version>_<arch>.deb and a dish-<version>.<arch>.rpm,
# the way the release bundle does. Builds both repositories through
# packaging/repo/build-apt-repo.sh and build-dnf-repo.sh (the scripts the
# publish jobs run) into dist/pages/, signed with a throwaway key that dies
# with the run, then reads them back the way a client does: apt fetches and
# verifies the signed index and downloads the package, and the DNF metadata's
# signature and checksums are checked against the package. Nothing is
# published. scripts/install-from-repos.sh then installs from the tree this
# writes, as a user would.
#
# Needs apt-utils, apt, createrepo_c and gpg (Debian/Ubuntu), and root or sudo
# for apt. linux-ci.yml's repos job runs it on the runner the publish jobs run
# on; locally, scripts/package-in-container.sh repos runs it in Debian.
set -euo pipefail
cd "$(dirname "$0")/.."

staging="$(cd "${1:?usage: $0 <staging dir>}" && pwd)"
sudo=""
[ "$(id -u)" = 0 ] || sudo="sudo"

fail() {
    echo "::error::$1" >&2
    exit 1
}

shopt -s nullglob
debs=("${staging}"/dish_*_*.deb)
rpms=("${staging}"/dish-[0-9]*.rpm)
[ "${#debs[@]}" -eq 1 ] || fail "expected one dish_*.deb in ${staging}, found ${#debs[@]}"
[ "${#rpms[@]}" -eq 1 ] || fail "expected one dish-<version>.rpm in ${staging}, found ${#rpms[@]}"
deb="${debs[0]}"
rpm="${rpms[0]}"
deb_arch="$(dpkg-deb -f "${deb}" Architecture)"
rpm_arch="${rpm%.rpm}"
rpm_arch="${rpm_arch##*.}"

GNUPGHOME="$(mktemp -d)"
export GNUPGHOME
gpg --batch --pinentry-mode loopback --passphrase '' \
    --quick-gen-key 'Dish CI throwaway <ci@tinkernorth.invalid>' ed25519 sign 1d
GPG_KEY_ID="$(gpg --list-keys --with-colons | awk -F: '/^fpr/ { print $10; exit }')"
export GPG_KEY_ID

pages="$(pwd)/dist/pages"
rm -rf "${pages}"
mkdir -p "${pages}"
gpg --batch --yes --armor --export "${GPG_KEY_ID}" > "${pages}/gpg.key"

echo "=== APT repository ==="
DISH_APT_ARCH="${deb_arch}" packaging/repo/build-apt-repo.sh "${pages}" "${staging}"

echo "=== DNF repository ==="
# file:///pages: the path scripts/install-from-repos.sh mounts this tree at, so
# the .repo file it installs resolves inside its Fedora container.
DISH_RPM_ARCH="${rpm_arch}" REPO_BASE_URL="file:///pages" \
    packaging/repo/build-dnf-repo.sh "${pages}" "${staging}"

echo "=== apt reads the repository back ==="
work="$(mktemp -d)"
gpg --batch --dearmor < "${pages}/gpg.key" > "${work}/dish.gpg"
echo "deb [arch=${deb_arch} signed-by=${work}/dish.gpg] file://${pages}/debian stable main" > "${work}/dish.list"
mkdir -p "${work}/lists/partial" "${work}/archives/partial" "${work}/download"
# apt fetches as its unprivileged _apt user, as it does into /var/lib/apt: the
# directories it writes are that user's, and the rest is readable, or apt
# falls back to fetching as root and says so.
chmod 755 "${work}"
if id _apt >/dev/null 2>&1; then
    ${sudo} chown -R _apt "${work}/lists" "${work}/archives" "${work}/download"
fi
apt_opts=(
    -o "Dir::Etc::SourceList=${work}/dish.list"
    -o "Dir::Etc::SourceParts=/nonexistent"
    -o "Dir::State::Lists=${work}/lists"
    -o "Dir::Cache::Archives=${work}/archives"
    -o "APT::Architecture=${deb_arch}"
)
# An unsigned or mis-signed InRelease is an error here, not a warning.
${sudo} apt-get "${apt_opts[@]}" -o APT::Get::List-Cleanup=0 \
    -o Acquire::AllowInsecureRepositories=false update 2>&1 | tee "${work}/update.log"
if grep -qE '^(W|E):' "${work}/update.log"; then
    fail "apt reported a problem reading the repository"
fi
version="$(dpkg-deb -f "${deb}" Version)"
apt-cache "${apt_opts[@]}" policy dish | tee "${work}/policy.log"
grep -q "Candidate: ${version}" "${work}/policy.log" || fail "apt does not offer dish ${version} from the repository"
(cd "${work}/download" && ${sudo} apt-get "${apt_opts[@]}" download "dish=${version}") 2>&1 | tee "${work}/download.log"
if grep -qE '^(W|E):' "${work}/download.log"; then
    fail "apt reported a problem downloading from the repository"
fi
cmp -s "${work}/download/dish_${version}_${deb_arch}.deb" "${deb}" || fail "apt downloaded a different .deb than the one published"

echo "=== DNF metadata ==="
repodata="${pages}/rpm/${rpm_arch}/repodata"
gpg --batch --verify "${repodata}/repomd.xml.asc" "${repodata}/repomd.xml" \
    || fail "repomd.xml's signature does not verify"
primary="$(find "${repodata}" -name '*primary.xml*' -print -quit)"
# The pool's copy: build-dnf-repo.sh signed it, so its bytes, not the staged
# file's, are what the metadata describes and what dnf installs.
pooled="${pages}/rpm/${rpm_arch}/$(basename "${rpm}")"
sum="$(sha256sum "${pooled}" | cut -d' ' -f1)"
case "${primary}" in
*.gz) contents="$(gzip -dc "${primary}")" ;;
*.zst) contents="$(zstd -dc "${primary}")" ;;
*) contents="$(cat "${primary}")" ;;
esac
grep -q "<name>dish</name>" <<<"${contents}" || fail "the DNF metadata lists no dish package"
grep -q "${sum}" <<<"${contents}" || fail "the DNF metadata's checksum is not the pooled .rpm's"
grep -qx "baseurl=file:///pages/rpm/${rpm_arch}" "${pages}/rpm/dish.repo" || fail "dish.repo points somewhere else"

echo "::notice::APT and DNF repositories built, signed and read back; nothing published"
