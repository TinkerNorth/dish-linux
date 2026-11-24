#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Checks a built package the way a user meets it, as root inside the lane's
# container:
#
#   scripts/test-package.sh deb     dist/*.deb: metadata, lintian, payload, install, launch
#   scripts/test-package.sh rpm     dist/*.rpm: payload, udev scriptlet, install, launch
#
# linux-ci.yml's package jobs, release.yml's deb and rpm jobs and
# scripts/package-in-container.sh all run this after scripts/build-deb.sh or
# scripts/build-rpm.sh, so the gate a pull request passes is the gate a tag
# passes.
set -euo pipefail
cd "$(dirname "$0")/.."

kind="${1:-}"
dist="${DIST_DIR:-dist}"

fail() {
    echo "::error::$1" >&2
    exit 1
}

prepare_launch() {
    # No seat, no bus, no display in a container; offscreen is enough to prove
    # the QML engine resolved every import it needs.
    export QT_QPA_PLATFORM=offscreen
    export XDG_RUNTIME_DIR=/tmp/dish-runtime
    mkdir -p "${XDG_RUNTIME_DIR}"
    chmod 700 "${XDG_RUNTIME_DIR}"
}

test_deb() {
    shopt -s nullglob
    local debs=("${dist}"/dish_*.deb)
    [ "${#debs[@]}" -eq 1 ] || fail "expected one dish_*.deb in ${dist}/, found ${#debs[@]}"
    local deb="${debs[0]}"

    # Cheap, and they catch the two mistakes that reach users as "no icon in
    # the menu" and "missing from GNOME Software" rather than as a build error.
    desktop-file-validate packaging/dish.desktop
    appstreamcli validate --no-net packaging/com.tinkernorth.Dish.metainfo.xml

    # A lintian error means the package is malformed, not merely untidy.
    lintian --fail-on error --tag-display-limit 0 "${deb}"

    # The udev rule is the payload whose absence silently degrades a whole
    # feature, and it has shipped to the wrong path before.
    local contents
    contents="$(dpkg-deb -c "${deb}")"
    local path
    for path in \
        ./usr/bin/dish \
        ./usr/lib/udev/rules.d/70-dish-hidraw.rules \
        ./usr/share/applications/com.tinkernorth.Dish.desktop \
        ./usr/share/metainfo/com.tinkernorth.Dish.metainfo.xml \
        ./usr/share/icons/hicolor/scalable/apps/com.tinkernorth.Dish.svg \
        ./usr/share/doc/dish/copyright
    do
        grep -qE " ${path}\$" <<<"${contents}" || fail "${path} missing from the .deb"
    done
    local icon
    icon="$(sed -n 's/^Icon=//p' packaging/dish.desktop)"
    grep -q "/apps/${icon}\." <<<"${contents}" || fail "Icon=${icon} names no installed icon"

    DEBIAN_FRONTEND=noninteractive apt-get install -y "./${deb}"
    prepare_launch
    echo "--- Qt image-format plugins ---"
    ls -1 /usr/lib/*/qt6/plugins/imageformats/ 2>/dev/null || echo "(no imageformats directory)"
    dpkg -S /usr/lib/*/qt6/plugins/imageformats/libqsvg.so 2>/dev/null \
        || echo "libqsvg.so is absent; no package here provides it"
    scripts/launch-check.sh /tmp/dish-launch.log dish
}

test_rpm() {
    shopt -s nullglob
    local rpms=("${dist}"/dish-[0-9]*.rpm)
    [ "${#rpms[@]}" -eq 1 ] || fail "expected one dish-<version>.rpm in ${dist}/, found ${#rpms[@]}"
    local rpm="${rpms[0]}"

    rpm -qlp "${rpm}"
    rpm -qp --requires "${rpm}"
    # Without the reload a pad plugged in before the install keeps its
    # root-only node and every USB-direct claim fails.
    rpm -qp --scripts "${rpm}" | grep -q 'udevadm control' \
        || fail "the .rpm carries no udev reload scriptlet"
    rpm -qlp "${rpm}" | grep -qx '/usr/lib/udev/rules.d/70-dish-hidraw.rules' \
        || fail "the udev rule is missing or at the wrong path"

    dnf -y install "./${rpm}"
    prepare_launch
    # Fedora builds Qt with journald logging, and Qt then writes its messages
    # to the journal whenever stderr is not a terminal: the log this step reads
    # stayed empty, so every check on it was blind (2.8.0).
    export QT_FORCE_STDERR_LOGGING=1
    echo "--- Qt image-format plugins ---"
    ls -1 /usr/lib64/qt6/plugins/imageformats/ 2>/dev/null || echo "(no imageformats directory)"
    rpm -qf /usr/lib64/qt6/plugins/imageformats/libqsvg.so 2>/dev/null \
        || echo "libqsvg.so is absent; no package here provides it"
    scripts/launch-check.sh /tmp/dish-launch.log dish
}

case "${kind}" in
deb) test_deb ;;
rpm) test_rpm ;;
*)
    echo "usage: $0 deb|rpm" >&2
    exit 2
    ;;
esac
