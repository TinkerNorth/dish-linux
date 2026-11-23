#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Installs the Qt CI builds with (DISH_QT_VERSION in scripts/tools.sh) from the
# official binaries through the pinned aqtinstall:
#
#   scripts/install-qt.sh <output dir>
#
# Prints the install prefix (<output dir>/<version>/<arch>) as its last line.
# .github/actions/setup-qt and scripts/install-deps.sh --ci-qt both run it, so
# a developer's Qt is CI's. Skips the download when the prefix is already
# there. Needs pipx (or an aqt of the pinned version on PATH).
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=scripts/tools.sh
source "${repo_root}/scripts/tools.sh"

outdir="${1:?usage: $0 <output dir>}"
version="${DISH_QT_VERSION}"

# aqt names the host and resolves linux_gcc_64 / linux_gcc_arm64 itself, so
# no architecture argument is needed. The desktop package's own archives are
# qtbase, qtdeclarative, qtsvg, qttools, qttranslations, qtwayland and icu,
# which covers every module this project links. qttasktree is added because
# from 6.12 the QML module's AssetDownloader plugin config depends on it, and
# without it every configure that finds Qt6Qml prints a CMake warning that the
# plugin's Qt6TaskTree dependency is missing.
case "$(uname -m)" in
x86_64) host=linux ;;
aarch64 | arm64) host=linux_arm64 ;;
*)
    echo "install-qt.sh: no Qt binaries for $(uname -m)" >&2
    exit 1
    ;;
esac

existing="$(find "${outdir}/${version}" -mindepth 1 -maxdepth 1 -xtype d -print -quit 2>/dev/null || true)"
if [ -z "${existing}" ] || [ ! -d "${existing}/lib/cmake/Qt6TaskTree" ]; then
    aqt_version="$(aqt version 2>/dev/null | sed -nE 's/^aqtinstall\(aqt\) v([0-9.]+).*/\1/p' || true)"
    if [ "${aqt_version}" != "${DISH_AQTINSTALL_VERSION}" ]; then
        pipx install --force "aqtinstall==${DISH_AQTINSTALL_VERSION}" >&2
        export PATH="${HOME}/.local/bin:${PATH}"
    fi
    aqt install-qt "${host}" desktop "${version}" -m qttasktree --outputdir "${outdir}" >&2
fi

# aqt lays the install down as <outdir>/<version>/<arch>. Read the arch
# directory back instead of hard-coding a name Qt has changed once already.
# -print -quit rather than a pipe to head: SIGPIPE under pipefail would fail.
root="$(find "${outdir}/${version}" -mindepth 1 -maxdepth 1 -xtype d -print -quit)"
[ -n "${root}" ] || { echo "install-qt.sh: aqt installed nothing under ${outdir}/${version}" >&2; exit 1; }
echo "${root}"
