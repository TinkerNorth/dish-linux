#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Runs a package lane locally, in the container CI runs it in, through the
# scripts CI calls:
#
#   scripts/package-in-container.sh deb                build, lint, install and launch the .deb
#   scripts/package-in-container.sh rpm                the same for the .rpm
#   scripts/package-in-container.sh arch <AppImage>    build the dish-bin package and a signed
#                                                      pacman repository from that AppImage
#   scripts/package-in-container.sh repos <dir>        build and read back the APT and DNF
#                                                      repositories from the .deb and .rpm in <dir>,
#                                                      then install and launch from each
#                                                      (scripts/install-from-repos.sh)
#
# The image tags come from scripts/tools.sh, the same tags linux-ci.yml and
# release.yml name. The tree goes in as a copy of the tracked and untracked
# (not ignored) files, so a run never writes root-owned files into the
# checkout; the artifacts come back under dist/container/<lane>/.
#
# Needs Docker. Resource limits default to a share of a shared machine;
# override with DISH_DOCKER_CPUS and DISH_DOCKER_MEMORY. The CPUs are a cpuset
# rather than a quota, so nproc inside the container, and with it Ninja's job
# count, matches the share instead of the host's core count (a quota with the
# host's job count is how a 3 GB container runs out of memory compiling moc's
# output).
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

lane="${1:-}"
cpus="${DISH_DOCKER_CPUS:-4}"
memory="${DISH_DOCKER_MEMORY:-4g}"
out="$(pwd)/dist/container/${lane}"

case "${lane}" in
deb)
    image="${DISH_DEB_IMAGE}"
    platform=()
    run='scripts/package-deps.sh deb && scripts/build-deb.sh && scripts/test-package.sh deb && cp dist/*.deb /out/'
    extra=()
    ;;
rpm)
    image="${DISH_RPM_IMAGE}"
    platform=()
    run='scripts/package-deps.sh rpm && scripts/build-rpm.sh && scripts/test-package.sh rpm && cp dist/*.rpm /out/'
    extra=()
    ;;
arch)
    appimage="${2:?usage: $0 arch <AppImage>}"
    image="${DISH_ARCH_IMAGE}"
    # The Arch image is published for x86_64 only, as is the package.
    platform=(--platform linux/amd64)
    run='scripts/package-deps.sh arch && scripts/check-arch-package.sh /appimage/Dish.AppImage && cp -r dist/arch dist/pages /out/'
    extra=(-v "$(cd "$(dirname "${appimage}")" && pwd)/$(basename "${appimage}"):/appimage/Dish.AppImage:ro")
    ;;
repos)
    staging="${2:?usage: $0 repos <dir with the .deb and .rpm>}"
    image="${DISH_DEB_IMAGE}"
    platform=()
    run='scripts/package-deps.sh repos && scripts/check-package-repos.sh /staging && cp -r dist/pages /out/'
    extra=(-v "$(cd "${staging}" && pwd):/staging:ro")
    ;;
*)
    echo "usage: $0 deb | rpm | arch <AppImage> | repos <dir>" >&2
    exit 2
    ;;
esac

rm -rf "${out}"
mkdir -p "${out}"
echo "=== ${lane} lane in ${image} ==="
git ls-files -z --cached --others --exclude-standard \
    | COPYFILE_DISABLE=1 tar --no-xattrs --null -T - -cf - \
    | docker run --rm -i "${platform[@]+"${platform[@]}"}" \
        --cpuset-cpus "0-$((cpus - 1))" --memory "${memory}" \
        -v "${out}:/out" "${extra[@]+"${extra[@]}"}" \
        "${image}" bash -c "set -euo pipefail; mkdir -p /src; tar -xf - -C /src; cd /src; ${run}"
if [ "${lane}" = repos ]; then
    # The install a user does, from the tree just built, each in its own
    # clean container; it starts those itself, so it runs from the host.
    scripts/install-from-repos.sh "${out}/pages"
fi
echo "=== ${lane} lane passed; artifacts in ${out} ==="
