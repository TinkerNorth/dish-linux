#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Builds dist/Dish-<version>-<arch>.AppImage: one self-contained file for every
# distro whose Qt is below this project's floor, Ubuntu LTS included.
#
#   scripts/build-appimage.sh
#   DISH_VERSION=1.2.3 scripts/build-appimage.sh
#
# Needs a Qt >= 6.7 on CMAKE_PREFIX_PATH with qmake on PATH (CI uses
# .github/actions/setup-qt), plus libsodium and SDL2 development packages.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

arch="${ARCH:-x86_64}"
build_dir="${repo_root}/build-appimage"
appdir="${build_dir}/AppDir"
tools_dir="${build_dir}/tools"
dist_dir="${repo_root}/dist"

version="${DISH_VERSION:-}"
if [ -z "${version}" ]; then
    version="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\+\([0-9]\+\.[0-9]\+\.[0-9]\+\).*/\1/p' \
                   CMakeLists.txt | head -n1)"
fi
if [ -z "${version}" ]; then
    echo "error: could not read project VERSION from CMakeLists.txt" >&2
    exit 1
fi

echo "==> Dish ${version} AppImage (${arch})"
rm -rf "${build_dir}"
mkdir -p "${appdir}" "${tools_dir}" "${dist_dir}"

# A udev rule under AppDir is read by nothing; it is carried as data below.
cmake -S . -B "${build_dir}/cmake" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DDISH_BUILD_TESTS=OFF \
    -DDISH_INSTALL_UDEV_RULES=OFF
cmake --build "${build_dir}/cmake" --parallel
DESTDIR="${appdir}" cmake --install "${build_dir}/cmake" --component Runtime

install -Dm644 packaging/udev/70-dish-hidraw.rules \
    "${appdir}/usr/share/dish/70-dish-hidraw.rules"

# Both tools publish only a rolling `continuous` tag, so pin by SHA-256 the way
# release.yml pins cosign. Record a new digest with `curl -fsSL <url> | sha256sum`.
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${arch}.AppImage"
LINUXDEPLOY_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-${arch}.AppImage"

fetch_tool() {
    local url="$1" out="$2" want="${3:-}"
    curl -fsSL --retry 3 --retry-delay 2 -o "${out}" "${url}"
    if [ -n "${want}" ]; then
        echo "${want}  ${out}" | sha256sum -c -
    else
        echo "::warning::${out##*/} not checksum-pinned; record $(sha256sum "${out}" | cut -d' ' -f1)"
    fi
    chmod +x "${out}"
}

fetch_tool "${LINUXDEPLOY_URL}"    "${tools_dir}/linuxdeploy"           "${LINUXDEPLOY_SHA256:-}"
fetch_tool "${LINUXDEPLOY_QT_URL}" "${tools_dir}/linuxdeploy-plugin-qt" "${LINUXDEPLOY_QT_SHA256:-}"

# A container without FUSE cannot mount an AppImage.
export APPIMAGE_EXTRACT_AND_RUN=1

# linuxdeploy-plugin-qt picks QML modules by scanning .qml SOURCES. Dish's QML
# is compiled into the binary by qt_add_qml_module, so without this the AppImage
# builds clean and dies at startup on `module "QtQuick" is not installed`.
export QML_SOURCES_PATHS="${repo_root}/src/qml"
# Qt6::Svg is linked, but the image-format plugin that renders the window icon
# is loaded at run time and has no DT_NEEDED to be found by.
export EXTRA_QT_MODULES="svg"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so"
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"

"${tools_dir}/linuxdeploy" \
    --appdir "${appdir}" \
    --executable "${appdir}/usr/bin/dish" \
    --desktop-file "${appdir}/usr/share/applications/com.tinkernorth.Dish.desktop" \
    --icon-file "${appdir}/usr/share/icons/hicolor/scalable/apps/com.tinkernorth.Dish.svg" \
    --icon-file "${appdir}/usr/share/icons/hicolor/512x512/apps/com.tinkernorth.Dish.png" \
    --plugin qt

install -Dm644 packaging/com.tinkernorth.Dish.metainfo.xml \
    "${appdir}/usr/share/metainfo/com.tinkernorth.Dish.metainfo.xml"

out="${dist_dir}/Dish-${version}-${arch}.AppImage"
rm -f "${out}"
OUTPUT="${out}" "${tools_dir}/linuxdeploy" --appdir "${appdir}" --output appimage

# A Qt Quick bundle missing one QML module builds perfectly and fails on the
# user's machine. Offscreen catches that here instead.
echo "==> Smoke test"
set +e
QT_QPA_PLATFORM=offscreen timeout 25 "${out}" --appimage-extract-and-run \
    >"${build_dir}/smoke.log" 2>&1
rc=$?
set -e
if grep -qE 'is not installed|Cannot load library|failed to load component|No such file or directory' \
        "${build_dir}/smoke.log"; then
    echo "::error::AppImage could not load its Qt/QML runtime:" >&2
    cat "${build_dir}/smoke.log" >&2
    exit 1
fi
echo "    exited ${rc} with no loader error"

sha256sum "${out}" | tee "${out}.sha256"

cat <<'BANNER'

---------------------------------------------------------------------------
 USB-direct needs a udev rule that an AppImage cannot install.

 The rule travels inside at usr/share/dish/70-dish-hidraw.rules. Until it is
 on the host, every USB-direct claim returns PermissionDenied and Dish keeps
 the pad on the SDL path — still streaming, just rate-capped.

   ./Dish-*.AppImage --appimage-extract usr/share/dish/70-dish-hidraw.rules
   sudo install -m 644 squashfs-root/usr/share/dish/70-dish-hidraw.rules \
        /etc/udev/rules.d/
   sudo udevadm control --reload-rules && sudo udevadm trigger
---------------------------------------------------------------------------
BANNER
