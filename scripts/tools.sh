#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# The pinned tool and dependency versions, in one file. Sourced by
# scripts/ci-local.sh, the build and packaging scripts, the composite actions
# and the workflows, so a bump is one edit here plus the copies that cannot
# source a shell file (the two Flatpak manifests, CMakeLists.txt's
# FetchContent pin and the documents), which scripts/check-version-consistency.sh
# holds to these values.
#
#   source scripts/tools.sh            the variables below
#   fetch_pinned <url> <out> <sha256>  download and verify one file
#
# Every SHA-256 is the digest the upstream publishes for that release asset
# (GitHub's asset digest, or the project's signed tarball), checked when it was
# recorded. linuxdeploy-plugin-qt's 1-alpha-20250213-1 asset predates GitHub's
# digests and the project publishes no checksum, so its value was recorded from
# the release asset itself.

# Qt for CI and the AppImage, through aqtinstall (scripts/install-qt.sh). The
# floor (6.7) lives in CMakeLists.txt; this is the version every
# Qt-from-binaries lane builds with, the 6.12 LTS. Two constraints on it: the
# translation gate needs lupdate 6.9 or newer (CONTRIBUTING.md, Translations),
# and 6.9's AOT-compiled QML crashed on a method call into a context being torn
# down (QTBUG-142514, DISH-LINUX-1), which 6.10.3 and later turn into a thrown
# TypeError. Upstream's regression test for that bug (tst_qmlcppcodegen's
# deadContext cases) was re-run against 6.12.1 when it was pinned and passed.
DISH_QT_VERSION="6.12.1"
DISH_AQTINSTALL_VERSION="3.3.0"

# The formatter every C++ repo in the fleet pins (PyPI wheel through pipx).
DISH_CLANG_FORMAT_VERSION="23.1.5"

# SDL2 built from source for the AppImage (jammy's 2.0.20 is below the floor)
# and as a Flatpak module. The floor the code needs is in CMakeLists.txt.
DISH_SDL2_VERSION="2.32.10"
DISH_SDL2_SHA256="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"

# Crash reporting. sentry-native is also pinned in CMakeLists.txt and both
# Flatpak manifests (they cannot source this file).
DISH_SENTRY_NATIVE_VERSION="0.17.6"
DISH_SENTRY_NATIVE_RELEASE_ZIP_SHA256="40b4b70cbada5bfae5b73f8267831dd023ec676d9676fb0d890878f31d2b5055"
DISH_SENTRY_CLI_VERSION="3.8.2"
DISH_SENTRY_CLI_LINUX_X86_64_SHA256="c958865f9d4000183509ea21b1f04925d5390de6d8d87386b9e69f2b305070c3" # sentry-cli-Linux-x86_64

# AppImage tooling, each at a numbered release rather than `continuous`.
DISH_LINUXDEPLOY_VERSION="1-alpha-20251107-1"
DISH_LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d" # linuxdeploy-x86_64.AppImage
DISH_LINUXDEPLOY_QT_VERSION="1-alpha-20250213-1"
DISH_LINUXDEPLOY_QT_SHA256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724" # linuxdeploy-plugin-qt-x86_64.AppImage
DISH_APPIMAGETOOL_VERSION="1.9.1"
DISH_APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0" # appimagetool-x86_64.AppImage
# appimagetool downloads the newest runtime unless it is handed one.
DISH_APPIMAGE_RUNTIME_VERSION="20251108"
DISH_APPIMAGE_RUNTIME_SHA256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d" # runtime-x86_64

# The KDE Flatpak runtime branch (newest org.kde.Platform branch on Flathub).
DISH_KDE_RUNTIME="6.11"

# Package build containers. linux-ci.yml and release.yml name the same tags;
# scripts/package-in-container.sh runs them locally.
DISH_DEB_IMAGE="debian:13.7"
DISH_RPM_IMAGE="fedora:44"
DISH_ARCH_IMAGE="archlinux:base-devel-20261108.0.623074"

# fetch_pinned <url> <out> <sha256>: a download that fails rather than
# continues when the bytes are not the pinned ones.
fetch_pinned() {
    local url="$1" out="$2" want="$3"
    if [ -z "${want}" ]; then
        echo "fetch_pinned: no SHA-256 for ${url}" >&2
        return 1
    fi
    curl -fsSL --retry 3 --retry-delay 2 -o "${out}" "${url}"
    echo "${want}  ${out}" | sha256sum -c -
}
