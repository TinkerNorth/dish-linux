#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Installs what a package lane needs inside its container, as root:
#
#   scripts/package-deps.sh deb     the .deb build, lint and launch (Debian)
#   scripts/package-deps.sh rpm     the .rpm build and launch (Fedora)
#   scripts/package-deps.sh arch    makepkg and repo-add (Arch Linux)
#   scripts/package-deps.sh repos   apt-ftparchive, createrepo_c, rpmsign and gpg (Debian or Ubuntu)
#   scripts/package-deps.sh appimage  the AppImage build host (Ubuntu 22.04)
#
# linux-ci.yml, release.yml and scripts/package-in-container.sh all call this,
# so the package lanes cannot drift from one another on what they install.
# Each lane builds against the distribution's own Qt: dpkg-shlibdeps and
# rpmbuild's soname scanner write the package's dependencies from the libraries
# the binary linked, so a Qt the distribution does not ship would produce a
# package that resolves on nothing.
set -euo pipefail

kind="${1:-}"

case "${kind}" in
deb)
    # A mirror that stops answering should cost seconds, not minutes.
    cat > /etc/apt/apt.conf.d/99-dish-acquire <<'CONF'
Acquire::http::Timeout "15";
Acquire::https::Timeout "15";
Acquire::Retries "2";
CONF
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        git ca-certificates \
        build-essential cmake ninja-build pkg-config \
        qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-svg-dev \
        qt6-tools-dev qt6-tools-dev-tools qt6-l10n-tools \
        libsodium-dev libsdl2-dev libssl-dev libopus-dev libdbus-1-dev \
        libcurl4-openssl-dev curl \
        dpkg-dev fakeroot file gzip \
        librsvg2-bin desktop-file-utils appstream lintian
    ;;
rpm)
    dnf -y install --setopt=install_weak_deps=False \
        git-core ca-certificates \
        gcc-c++ cmake ninja-build pkgconf-pkg-config rpm-build \
        qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel qt6-qttools-devel \
        libsodium-devel SDL2-devel openssl-devel opus-devel dbus-devel \
        libcurl-devel \
        librsvg2-tools desktop-file-utils libappstream-glib gzip \
        rpm-sign gnupg2
    # The image carries curl-minimal, which conflicts with the full curl the
    # Sentry upload step uses.
    dnf -y install --allowerasing curl
    ;;
appimage)
    cat > /etc/apt/apt.conf.d/99-dish-acquire <<'CONF'
Acquire::http::Timeout "15";
Acquire::https::Timeout "15";
Acquire::Retries "2";
CONF
    apt-get update
    # The xcb block is what libqxcb.so links: linuxdeploy's qt plugin refuses
    # to deploy a platform plugin whose dependencies it cannot resolve. The
    # GL, fontconfig, xkbcommon and X11 headers are what Qt itself links
    # (.github/actions/setup-qt installs the same on a runner).
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates pipx \
        libsodium-dev libssl-dev libopus-dev libpulse-dev libudev-dev libusb-1.0-0-dev \
        libcurl4-openssl-dev libdbus-1-dev \
        libgl1-mesa-dev libegl1-mesa-dev libfontconfig1-dev libxkbcommon-dev libx11-dev \
        libxkbcommon-x11-0 libxcb-cursor0 libxcb-icccm4 libxcb-image0 \
        libxcb-keysyms1 libxcb-randr0 libxcb-render-util0 libxcb-shape0 \
        libxcb-xinerama0 libxcb-xkb1 \
        file fakeroot rsync wget curl librsvg2-bin gzip
    ;;
repos)
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        apt-utils createrepo-c rpm gnupg gzip zstd ca-certificates
    ;;
arch)
    pacman -Syu --noconfirm --needed git pacman-contrib gnupg
    ;;
*)
    echo "usage: $0 deb|rpm|arch|repos|appimage" >&2
    exit 2
    ;;
esac
