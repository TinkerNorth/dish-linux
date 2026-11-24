#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# The pull-request rehearsal of release.yml's arch-publish job, as root in the
# archlinux:base-devel container:
#
#   scripts/check-arch-package.sh <AppImage>
#
# Builds dish-bin through scripts/build-arch-package.sh from this tree and the
# given AppImage, signs it and a pacman repository with a throwaway key through
# packaging/repo/build-pacman-repo.sh (the script arch-publish runs with the
# real one), then has pacman install dish-bin from that repository with
# signature checking on and checks what landed. Nothing is published: the
# repository is written to dist/pages/ and the key dies with the container.
set -euo pipefail
cd "$(dirname "$0")/.."

appimage="${1:?usage: $0 <AppImage>}"
version="$(awk '/^project\(Dish/ { inproj = 1 }
                inproj && /^[[:space:]]*VERSION[[:space:]]/ { print $2; exit }' CMakeLists.txt)"

fail() {
    echo "::error::$1" >&2
    exit 1
}

scripts/build-arch-package.sh "${version}" "${appimage}"

GNUPGHOME="$(mktemp -d)"
export GNUPGHOME
gpg --batch --pinentry-mode loopback --passphrase '' \
    --quick-gen-key 'Dish CI throwaway <ci@tinkernorth.invalid>' ed25519 sign 1d
GPG_KEY_ID="$(gpg --list-keys --with-colons | awk -F: '/^fpr/ { print $10; exit }')"
export GPG_KEY_ID

rm -rf dist/pages
packaging/repo/build-pacman-repo.sh "$(pwd)/dist/pages" "$(pwd)/dist/arch"

# Trust the throwaway key the way a user trusts the real one (the
# packaging/repo/README.md recipe), then install through the repository with
# pacman's default signature policy, so an unsigned or mis-signed package or
# database fails here.
pacman-key --init
gpg --batch --armor --export "${GPG_KEY_ID}" > /tmp/dish-ci.key
pacman-key --add /tmp/dish-ci.key
pacman-key --lsign-key "${GPG_KEY_ID}"
cat >> /etc/pacman.conf <<CONF

[dish]
SigLevel = Required DatabaseRequired
Server = file://$(pwd)/dist/pages/arch/\$arch
CONF
pacman -Sy --noconfirm dish-bin

installed="$(pacman -Q dish-bin)"
[ "${installed}" = "dish-bin ${version}-1" ] || fail "pacman installed '${installed}', not dish-bin ${version}-1"
for path in \
    /opt/dish/Dish.AppImage \
    /usr/bin/dish \
    /usr/lib/udev/rules.d/70-dish-hidraw.rules \
    /usr/share/applications/com.tinkernorth.Dish.desktop \
    /usr/share/icons/hicolor/scalable/apps/com.tinkernorth.Dish.svg \
    /usr/share/metainfo/com.tinkernorth.Dish.metainfo.xml
do
    [ -e "${path}" ] || fail "${path} missing after installing dish-bin"
done
cmp -s "${appimage}" /opt/dish/Dish.AppImage || fail "the installed AppImage is not the one packaged"
echo "::notice::dish-bin ${version}-1 built, signed, served from a pacman repository and installed"
