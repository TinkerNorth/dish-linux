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
#   source scripts/tools.sh            the variables below, exported, so a
#                                      script the sourcing one runs sees them
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
# The QML is now compiled to bytecode only (CMakeLists.txt), so no AOT code
# ships at all.
export DISH_QT_VERSION="6.12.1"
export DISH_AQTINSTALL_VERSION="3.3.0"

# The formatter every C++ repo in the fleet pins (PyPI wheel through pipx).
export DISH_CLANG_FORMAT_VERSION="23.1.5"
# The linter, the PyPI wheel too (it carries its own clang resource headers),
# so CI and a developer run the same checks instead of whatever clang-tidy the
# distribution ships.
export DISH_CLANG_TIDY_VERSION="23.1.4"

# SDL2 built from source for the AppImage (jammy's 2.0.20 is below the floor)
# and as a Flatpak module. The floor the code needs is in CMakeLists.txt.
export DISH_SDL2_VERSION="2.32.10"
export DISH_SDL2_SHA256="5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165"

# Built from source only inside the Flatpak (elsewhere the distribution's own,
# or the AppImage build host's, is linked). The Flatpak manifests repeat it.
export DISH_LIBSODIUM_VERSION="1.0.22"

# ENet, vendored under third_party/enet/ from Cameron Gutman's fork (the commit
# Moonlight and Wolf build against). THIRD_PARTY.md and licenses.json name it.
export DISH_ENET_COMMIT="4cde9cc3dcc5c30775a80da1de87f39f98672a31"

# Crash reporting. sentry-native is also pinned in CMakeLists.txt and both
# Flatpak manifests (they cannot source this file).
export DISH_SENTRY_NATIVE_VERSION="0.17.6"
export DISH_SENTRY_NATIVE_RELEASE_ZIP_SHA256="40b4b70cbada5bfae5b73f8267831dd023ec676d9676fb0d890878f31d2b5055"
export DISH_SENTRY_CLI_VERSION="3.8.2"
export DISH_SENTRY_CLI_LINUX_X86_64_SHA256="c958865f9d4000183509ea21b1f04925d5390de6d8d87386b9e69f2b305070c3" # sentry-cli-Linux-x86_64

# AppImage tooling, each at a numbered release rather than `continuous`.
export DISH_LINUXDEPLOY_VERSION="1-alpha-20251107-1"
export DISH_LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d" # linuxdeploy-x86_64.AppImage
export DISH_LINUXDEPLOY_QT_VERSION="1-alpha-20250213-1"
export DISH_LINUXDEPLOY_QT_SHA256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724" # linuxdeploy-plugin-qt-x86_64.AppImage
export DISH_APPIMAGETOOL_VERSION="1.9.1"
export DISH_APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0" # appimagetool-x86_64.AppImage
# appimagetool downloads the newest runtime unless it is handed one.
export DISH_APPIMAGE_RUNTIME_VERSION="20251108"
export DISH_APPIMAGE_RUNTIME_SHA256="2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d" # runtime-x86_64

# The KDE Flatpak runtime branch (newest org.kde.Platform branch on Flathub).
export DISH_KDE_RUNTIME="6.11"

# Package build containers. linux-ci.yml and release.yml name the same tags;
# scripts/package-in-container.sh runs them locally.
export DISH_DEB_IMAGE="debian:13.7"
export DISH_RPM_IMAGE="fedora:44"
export DISH_ARCH_IMAGE="archlinux:base-devel-20261108.0.623074"

# The security gates' tools, for scripts/ci-local.sh. CI pins the same versions
# and linux-amd64 SHA-256s inside the shared _security.yml (byte-identical
# across the fleet, so it cannot source this file);
# scripts/check-version-consistency.sh holds the two to each other. SHA-256s
# from the upstream checksum files (osv-scanner_SHA256SUMS,
# gitleaks_8.30.1_checksums.txt).
export DISH_OSV_SCANNER_VERSION="2.7.1"
export DISH_OSV_SCANNER_SHA256_LINUX_AMD64="fb7dab19a8bd9bbe5c3189f09d4b7ee1820c9553a23bac202648a56efd2f6ee8"
export DISH_OSV_SCANNER_SHA256_DARWIN_ARM64="b22c02877a811c8f78956e8643efa554932b7378f590d13f8e97d4e9448a29f3"
export DISH_GITLEAKS_VERSION="8.30.1"
export DISH_GITLEAKS_SHA256_LINUX_AMD64="551f6fc83ea457d62a0d98237cbad105af8d557003051f41f3e7ca7b3f2470eb"
export DISH_GITLEAKS_SHA256_DARWIN_ARM64="b40ab0ae55c505963e365f271a8d3846efbc170aa17f2607f13df610a9aeb6a5"

# fetch_pinned <url> <out> <sha256>: a download that fails rather than
# continues when the bytes are not the pinned ones.
fetch_pinned() {
    local url="$1" out="$2" want="$3"
    if [ -z "${want}" ]; then
        echo "fetch_pinned: no SHA-256 for ${url}" >&2
        return 1
    fi
    curl -fsSL --retry 3 --retry-delay 2 -o "${out}" "${url}"
    if command -v sha256sum >/dev/null 2>&1; then
        echo "${want}  ${out}" | sha256sum -c -
    else
        echo "${want}  ${out}" | shasum -a 256 -c -
    fi
}

# fetch_security_tools <dir>: OSV-Scanner and gitleaks at the pins above into
# <dir> (skipped when already there at that version). Linux x86_64 and macOS
# arm64 only: the two platforms upstream checksums are recorded for here.
fetch_security_tools() {
    local dir="$1" platform osv_sha gl_sha gl_asset
    case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)
        platform=linux_amd64
        osv_sha="${DISH_OSV_SCANNER_SHA256_LINUX_AMD64}"
        gl_sha="${DISH_GITLEAKS_SHA256_LINUX_AMD64}"
        gl_asset="gitleaks_${DISH_GITLEAKS_VERSION}_linux_x64.tar.gz"
        ;;
    Darwin-arm64)
        platform=darwin_arm64
        osv_sha="${DISH_OSV_SCANNER_SHA256_DARWIN_ARM64}"
        gl_sha="${DISH_GITLEAKS_SHA256_DARWIN_ARM64}"
        gl_asset="gitleaks_${DISH_GITLEAKS_VERSION}_darwin_arm64.tar.gz"
        ;;
    *)
        echo "fetch_security_tools: no pinned OSV-Scanner or gitleaks for $(uname -s)-$(uname -m)" >&2
        return 1
        ;;
    esac
    mkdir -p "${dir}"
    if [ "$(cat "${dir}/osv-scanner.sha256" 2>/dev/null)" != "${osv_sha}" ]; then
        fetch_pinned \
            "https://github.com/google/osv-scanner/releases/download/v${DISH_OSV_SCANNER_VERSION}/osv-scanner_${platform}" \
            "${dir}/osv-scanner" "${osv_sha}"
        chmod +x "${dir}/osv-scanner"
        echo "${osv_sha}" > "${dir}/osv-scanner.sha256"
    fi
    if [ "$(cat "${dir}/gitleaks.sha256" 2>/dev/null)" != "${gl_sha}" ]; then
        fetch_pinned \
            "https://github.com/gitleaks/gitleaks/releases/download/v${DISH_GITLEAKS_VERSION}/${gl_asset}" \
            "${dir}/gitleaks.tar.gz" "${gl_sha}"
        tar -xzf "${dir}/gitleaks.tar.gz" -C "${dir}" gitleaks
        rm -f "${dir}/gitleaks.tar.gz"
        echo "${gl_sha}" > "${dir}/gitleaks.sha256"
    fi
}
