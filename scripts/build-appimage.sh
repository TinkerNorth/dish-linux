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
# .github/actions/setup-qt, which runs scripts/install-qt.sh), plus libsodium
# and an SDL2 >= 2.24 (scripts/build-sdl2.sh on jammy). The tools that build
# the bundle (linuxdeploy, its Qt plugin, appimagetool and the AppImage
# runtime) are fetched at the releases and SHA-256s scripts/tools.sh pins; a
# download that does not match fails the build.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

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
# DISH_SENTRY_DSN is empty unless release.yml exported it from the repository
# secret. A hand-run of this script therefore produces a build that cannot
# transmit, and one that does not pay to fetch and build the SDK either.
cmake -S . -B "${build_dir}/cmake" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DDISH_BUILD_TESTS=OFF \
    -DDISH_INSTALL_UDEV_RULES=OFF \
    -DDISH_SENTRY_DSN="${DISH_SENTRY_DSN:-}"
cmake --build "${build_dir}/cmake" --parallel
DESTDIR="${appdir}" cmake --install "${build_dir}/cmake" --component Runtime

install -Dm644 packaging/udev/70-dish-hidraw.rules \
    "${appdir}/usr/share/dish/70-dish-hidraw.rules"

fetch_pinned \
    "https://github.com/linuxdeploy/linuxdeploy/releases/download/${DISH_LINUXDEPLOY_VERSION}/linuxdeploy-${arch}.AppImage" \
    "${tools_dir}/linuxdeploy" "${DISH_LINUXDEPLOY_SHA256}"
fetch_pinned \
    "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/${DISH_LINUXDEPLOY_QT_VERSION}/linuxdeploy-plugin-qt-${arch}.AppImage" \
    "${tools_dir}/linuxdeploy-plugin-qt" "${DISH_LINUXDEPLOY_QT_SHA256}"
fetch_pinned \
    "https://github.com/AppImage/appimagetool/releases/download/${DISH_APPIMAGETOOL_VERSION}/appimagetool-${arch}.AppImage" \
    "${tools_dir}/appimagetool" "${DISH_APPIMAGETOOL_SHA256}"
fetch_pinned \
    "https://github.com/AppImage/type2-runtime/releases/download/${DISH_APPIMAGE_RUNTIME_VERSION}/runtime-${arch}" \
    "${tools_dir}/runtime-${arch}" "${DISH_APPIMAGE_RUNTIME_SHA256}"
chmod +x "${tools_dir}/linuxdeploy" "${tools_dir}/linuxdeploy-plugin-qt" "${tools_dir}/appimagetool"
# linuxdeploy finds its plugins on PATH; this puts the pinned Qt plugin first.
export PATH="${tools_dir}:${PATH}"

# A container without FUSE cannot mount an AppImage.
export APPIMAGE_EXTRACT_AND_RUN=1

# linuxdeploy-plugin-qt picks QML modules by scanning .qml SOURCES. Dish's QML
# is compiled into the binary by qt_add_qml_module, so without this the AppImage
# builds clean and dies at startup on `module "QtQuick" is not installed`.
export QML_SOURCES_PATHS="${repo_root}/src/qml"
# Qt6::Svg is linked, but the image-format plugin that renders the window icon
# is loaded at run time and has no DT_NEEDED to be found by.
export EXTRA_QT_MODULES="svg"
export QMAKE="${QMAKE:-$(command -v qmake6 || command -v qmake)}"
qt_plugin_dir="$("${QMAKE}" -query QT_INSTALL_PLUGINS)"

# Qt 6.10 ships one Wayland platform plugin, libqwayland.so, from qtbase;
# 6.7 to 6.9 ship the generic/egl pair from qtwayland. Probed, not pinned,
# so a build against either Qt bundles what it has, and a Qt with neither
# fails here rather than on a user's Wayland desktop.
if [ -e "${qt_plugin_dir}/platforms/libqwayland.so" ]; then
    wayland_platforms="libqwayland.so"
elif [ -e "${qt_plugin_dir}/platforms/libqwayland-generic.so" ] \
    && [ -e "${qt_plugin_dir}/platforms/libqwayland-egl.so" ]; then
    wayland_platforms="libqwayland-generic.so;libqwayland-egl.so"
else
    echo "::error::the Qt at ${qt_plugin_dir} ships no Wayland platform plugin; the AppImage would not start on a Wayland desktop" >&2
    exit 1
fi
# libqoffscreen.so is not a plugin a desktop ever selects (Qt picks wayland or
# xcb from the session), and it is what lets the bundle be launched with no
# display at all, which is the only way the smoke test below can run the
# shipped artifact rather than a build tree. The .deb and .rpm lanes get it
# from the distribution; nothing but this line puts it in the AppImage.
export EXTRA_PLATFORM_PLUGINS="${wayland_platforms};libqoffscreen.so"

"${tools_dir}/linuxdeploy" \
    --appdir "${appdir}" \
    --executable "${appdir}/usr/bin/dish" \
    --desktop-file "${appdir}/usr/share/applications/com.tinkernorth.Dish.desktop" \
    --icon-file "${appdir}/usr/share/icons/hicolor/scalable/apps/com.tinkernorth.Dish.svg" \
    --icon-file "${appdir}/usr/share/icons/hicolor/512x512/apps/com.tinkernorth.Dish.png" \
    --plugin qt

# The qt plugin deploys wayland-shell-integration and the decoration plugin
# but not the client buffer integrations, and a Wayland session with zero
# buffer integrations aborts at the first expose ("Available client buffer
# integrations: QList()", then QRhi fails and Qt Quick qFatals). Copy the
# directory it forgets; the packaging pass below pulls its libraries.
if [ -d "${qt_plugin_dir}/wayland-graphics-integration-client" ]; then
    cp -r "${qt_plugin_dir}/wayland-graphics-integration-client" \
        "${appdir}/usr/plugins/"
fi
must_have=(
    "platforms/libqxcb.so"
    "platforms/libqoffscreen.so"
    "wayland-graphics-integration-client"
    "wayland-shell-integration"
)
IFS=';' read -ra wayland_platform_files <<< "${wayland_platforms}"
for plugin in "${wayland_platform_files[@]}"; do
    must_have+=("platforms/${plugin}")
done
for must in "${must_have[@]}"; do
    if [ ! -e "${appdir}/usr/plugins/${must}" ]; then
        echo "::error::AppImage is missing usr/plugins/${must}; a Wayland desktop crashes at startup without it, and the smoke test below cannot run at all" >&2
        exit 1
    fi
done

install -Dm644 packaging/com.tinkernorth.Dish.metainfo.xml \
    "${appdir}/usr/share/metainfo/com.tinkernorth.Dish.metainfo.xml"

out="${dist_dir}/Dish-${version}-${arch}.AppImage"
rm -f "${out}" "${out}.zsync"

# Embed AppImageUpdate metadata so AppImageUpdate / Gear Lever can delta-update
# straight off the newest GitHub release instead of a full manual re-download.
# appimagetool also emits the matching .zsync index, which release.yml uploads
# beside the AppImage; the draft-then-flip publish keeps `releases/latest` atomic,
# so the pattern never resolves to a half-uploaded release. appimagetool is
# called directly with the pinned runtime: left to itself it downloads the
# newest runtime at build time, which is the unpinned input this replaces.
update_info="${DISH_APPIMAGE_UPDATE_INFO:-gh-releases-zsync|TinkerNorth|dish-linux|latest|Dish-*-${arch}.AppImage.zsync}"
(
    cd "${dist_dir}"
    ARCH="${arch}" "${tools_dir}/appimagetool" \
        --runtime-file "${tools_dir}/runtime-${arch}" \
        --updateinformation "${update_info}" \
        "${appdir}" "${out}"
)
# The delta channel is part of what ships: a bundle without its index fails
# the build rather than going out with updates silently dark.
if [ ! -f "${out}.zsync" ]; then
    echo "::error::appimagetool emitted no ${out}.zsync" >&2
    exit 1
fi

# A Qt Quick bundle missing one QML module builds perfectly and fails on the
# user's machine. Offscreen catches that here instead.
#
# Unpacked first, and started through the unpacked AppRun rather than through
# the AppImage: a runner has no FUSE, so the runtime unpacks to a temp
# directory and forks the app anyway, and the process this script could signal
# would be the runtime rather than the app. Unpacking here makes the app the
# child, which is what lets the checks below mean anything, and it fails with
# the runtime's own message if the artifact cannot be read at all.
echo "==> Smoke test"
smoke_dir="${build_dir}/smoke"
smoke_log="${build_dir}/smoke.log"
rm -rf "${smoke_dir}"
mkdir -p "${smoke_dir}"
(cd "${smoke_dir}" && "${out}" --appimage-extract >/dev/null)
apprun="${smoke_dir}/squashfs-root/AppRun"
if [ ! -x "${apprun}" ]; then
    echo "::error::the AppImage unpacked without a runnable AppRun" >&2
    exit 1
fi
# AppRun is a symlink to usr/bin/dish. It has to land inside the unpacked tree:
# an absolute one would point back at the AppDir this build made, so the smoke
# test would run the build's binary and prove nothing about the artifact, and
# on a user's machine it would not resolve at all.
apprun_target="$(readlink -f "${apprun}")"
case "${apprun_target}" in
"${smoke_dir}/squashfs-root/"*) ;;
*)
    echo "::error::AppRun resolves to ${apprun_target}, outside the unpacked AppImage" >&2
    exit 1
    ;;
esac

# 0700 because Qt reports any other mode on the runtime directory as a warning.
# A HOME of its own as well, so a hand-run leaves no QSettings file behind in
# the developer's, and so the run starts from the same blank state every time.
xdg_runtime="${build_dir}/xdg-runtime"
smoke_home="${build_dir}/smoke-home"
mkdir -p "${xdg_runtime}" "${smoke_home}"
chmod 700 "${xdg_runtime}"

# `env -i` with a named few, because the build environment is exactly what this
# has to be kept away from: release.yml puts the Qt it built against and the
# SDL it just installed on LD_LIBRARY_PATH, and LD_LIBRARY_PATH wins over the
# DT_RUNPATH linuxdeploy writes, so an inherited environment loads the build
# tree's libraries and the bundle is never tested. Whatever the app needs has
# to come from the AppDir, which is the point.
# scripts/launch-check.sh is the gate the .deb and .rpm lanes run on the
# installed package: it starts the app, signals it, and fails on a loader
# error, an SVG decode failure, a missing controller database line, an early
# exit, or a shutdown that needed SIGKILL. env execs AppRun, so the process it
# signals is the app itself.
scripts/launch-check.sh "${smoke_log}" \
    env -i \
    HOME="${smoke_home}" \
    PATH="/usr/bin:/bin" \
    XDG_RUNTIME_DIR="${xdg_runtime}" \
    QT_QPA_PLATFORM=offscreen \
    "${apprun}"
rm -rf "${smoke_dir}"

sha256sum "${out}" | tee "${out}.sha256"

cat <<'BANNER'

---------------------------------------------------------------------------
 USB-direct needs a udev rule that an AppImage cannot install, except on
 SteamOS, whose own rules already open every hidraw node to the seat user.

 The rule travels inside at usr/share/dish/70-dish-hidraw.rules. On any other
 distribution, until it is on the host, every USB-direct claim returns
 PermissionDenied and Dish keeps the pad on the SDL path — still streaming,
 just rate-capped.

   ./Dish-*.AppImage --appimage-extract usr/share/dish/70-dish-hidraw.rules
   sudo install -m 644 squashfs-root/usr/share/dish/70-dish-hidraw.rules \
        /etc/udev/rules.d/
   sudo udevadm control --reload-rules && sudo udevadm trigger
---------------------------------------------------------------------------
BANNER
