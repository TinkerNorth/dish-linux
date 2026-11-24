#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Installs Dish from a built APT and DNF repository the way a user does, each
# in a clean container of the distribution the package targets, and launches
# it:
#
#   scripts/install-from-repos.sh <pages dir>
#
# <pages dir> is the tree scripts/check-package-repos.sh writes (dist/pages):
# gpg.key, debian/ and rpm/. It is mounted at /pages, which is the base URL
# that lane gives the repositories, so the .repo file and the key URL it names
# resolve inside the container exactly as the published ones do over HTTPS.
#
#   Debian (DISH_DEB_IMAGE): the key through gpg --dearmor, the
#     signed-by sources line, apt-get update, apt-get install dish.
#   Fedora (DISH_RPM_IMAGE): the repository's own dish.repo into
#     /etc/yum.repos.d/, dnf -y install dish, with its gpgcheck=1 and
#     repo_gpgcheck=1 as published.
#
# Then scripts/launch-check.sh on the installed app in each. The commands
# follow packaging/repo/README.md's end-user instructions; the published
# repositories differ only in the base URL. Needs Docker; the containers run
# as the packages' architecture (x86_64 under emulation on an arm64 host).
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

pages="$(cd "${1:?usage: $0 <pages dir>}" && pwd)"
repo_root="$(pwd)"

deb_arch="$(sed -n 's/^Architecture: //p' "${pages}"/debian/dists/stable/main/binary-*/Packages | head -n1)"
rpm_dir="$(find "${pages}/rpm" -mindepth 1 -maxdepth 1 -type d -print -quit)"
rpm_arch="$(basename "${rpm_dir}")"
case "${deb_arch}" in
amd64) deb_platform=linux/amd64 ;;
arm64) deb_platform=linux/arm64 ;;
*) echo "install-from-repos: no container platform for .deb architecture '${deb_arch}'" >&2; exit 1 ;;
esac
case "${rpm_arch}" in
x86_64) rpm_platform=linux/amd64 ;;
aarch64) rpm_platform=linux/arm64 ;;
*) echo "install-from-repos: no container platform for .rpm architecture '${rpm_arch}'" >&2; exit 1 ;;
esac

run_in() {
    local platform="$1" image="$2" script="$3"
    docker run --rm --platform "${platform}" --cpus "${DISH_DOCKER_CPUS:-4}" --memory "${DISH_DOCKER_MEMORY:-3g}" \
        -v "${pages}:/pages:ro" -v "${repo_root}/scripts:/scripts:ro" \
        "${image}" bash -c "set -euo pipefail; ${script}"
}

prepare_launch="export QT_QPA_PLATFORM=offscreen XDG_RUNTIME_DIR=/tmp/dish-runtime; mkdir -p /tmp/dish-runtime; chmod 700 /tmp/dish-runtime"

echo "=== apt-get install dish from the APT repository, in ${DISH_DEB_IMAGE} (${deb_platform}) ==="
run_in "${deb_platform}" "${DISH_DEB_IMAGE}" "
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends gpg ca-certificates >/dev/null
    gpg --dearmor < /pages/gpg.key > /usr/share/keyrings/dish-archive-keyring.gpg
    echo 'deb [signed-by=/usr/share/keyrings/dish-archive-keyring.gpg] file:///pages/debian stable main' \
        > /etc/apt/sources.list.d/dish.list
    apt-get update 2>&1 | tee /tmp/update.log
    if grep -qE '^(W|E):' /tmp/update.log; then echo '::error::apt reported a problem with the repository' >&2; exit 1; fi
    apt-get install -y dish
    ${prepare_launch}
    /scripts/launch-check.sh /tmp/dish-launch.log dish
"

echo "=== dnf -y install dish from the DNF repository, in ${DISH_RPM_IMAGE} (${rpm_platform}) ==="
run_in "${rpm_platform}" "${DISH_RPM_IMAGE}" "
    cp /pages/rpm/dish.repo /etc/yum.repos.d/dish.repo
    cat /etc/yum.repos.d/dish.repo
    grep -qx 'gpgcheck=1' /etc/yum.repos.d/dish.repo
    grep -qx 'repo_gpgcheck=1' /etc/yum.repos.d/dish.repo
    dnf -y install dish
    ${prepare_launch}
    export QT_FORCE_STDERR_LOGGING=1
    /scripts/launch-check.sh /tmp/dish-launch.log dish
"
echo "::notice::dish installed and launched from the APT repository and from the DNF repository"
