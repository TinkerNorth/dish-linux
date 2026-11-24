#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# qmllint over every tracked QML file, against the debug tree's generated
# qmltypes: the invocation linux-ci.yml and scripts/ci-local.sh both run.
#
#   scripts/check-qml.sh            (after `cmake --build --preset debug`)
#
# Every category at its default level, `unqualified` included: App, Theme and
# Tokens are all module singletons the generated qmltypes describes, so every
# name a page reads resolves statically. -I "${QT_ROOT_DIR}/qml" when
# QT_ROOT_DIR is set (setup-qt exports it): a distro qmllint may otherwise
# resolve Qt's own modules differently than CI's.
set -euo pipefail
cd "$(dirname "$0")/.."

qt_qml_args=()
if [ -n "${QT_ROOT_DIR:-}" ] && [ -d "${QT_ROOT_DIR}/qml" ]; then
    qt_qml_args=(-I "${QT_ROOT_DIR}/qml")
fi
# git's * crosses directory levels; 'src/qml/**/*.qml' would miss the
# top-level Main and AppShell.
mapfile -t qml_files < <(git ls-files 'src/qml/*.qml')
qmllint -I build "${qt_qml_args[@]+"${qt_qml_args[@]}"}" "${qml_files[@]}"
echo "qmllint: OK (${#qml_files[@]} files)"
