#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Launches an installed or unpacked Dish offscreen, lets it come up, sends it
# SIGTERM, and fails on anything its log or its exit says went wrong:
#
#   scripts/launch-check.sh <log file> [command ...]    (command defaults to `dish`)
#
# The gate nothing else can stand in for. Unit tests, lints and a contents
# check all pass against a build tree, and a build tree is exactly what hides a
# missing runtime dependency: a QML module resolves from the Qt install during
# development and from a Depends: line, or the AppImage's own tree, after. The
# .deb and .rpm lanes run it on the installed package, scripts/build-appimage.sh
# on the unpacked bundle.
#
# The caller sets the environment (QT_QPA_PLATFORM=offscreen, XDG_RUNTIME_DIR,
# and on Fedora QT_FORCE_STDERR_LOGGING=1, without which Qt writes its messages
# to the journal whenever stderr is not a terminal and the log stays empty).
set -euo pipefail

log="${1:?usage: $0 <log file> [command ...]}"
shift
if [ "$#" -eq 0 ]; then
    set -- dish
fi

# Signalled by hand rather than through `timeout`, which reports 124 whether
# the child shut down cleanly or had to be killed, the one distinction this
# check exists to make. Every wait below is bounded, because an unbounded one
# is how this step once sat for 40 minutes and took the job's timeout with it.
set +e
"$@" >"${log}" 2>&1 &
app=$!
sleep 20
started=0
kill -0 "${app}" 2>/dev/null && started=1
needed_kill=0
if [ "${started}" = 1 ]; then
    kill -TERM "${app}" 2>/dev/null
    waited=0
    while [ "${waited}" -lt 15 ] && kill -0 "${app}" 2>/dev/null; do
        sleep 1
        waited=$((waited + 1))
    done
    if kill -0 "${app}" 2>/dev/null; then
        kill -KILL "${app}" 2>/dev/null
        needed_kill=1
    fi
fi
wait "${app}"
rc=$?
set -e

# Printed either way: an exit code on its own says nothing about why.
echo "--- launch log ---"
cat "${log}"
echo "--- end launch log ---"

fail() {
    echo "::error::$1" >&2
    exit 1
}

if grep -qE 'is not installed|failed to load component|Cannot load library|[Nn]o such file' "${log}"; then
    fail "the app cannot load its Qt/QML runtime"
fi
# The app renders no iconography at all when this fires, and the only symptom
# is a log flood nobody reads. The SVG handler is a run-time plugin, so its
# absence shows up as a decode failure per glyph rather than as a link error.
if grep -q 'Error decoding:' "${log}"; then
    fail "the app cannot decode its brand SVGs; the Qt SVG image plugin is missing or unloadable"
fi
# The bundled controller database is a resource compiled into the static
# dish_core. A binary that lost it still starts, with no controller list and
# "bundled version 0" on the Diagnostics page, so only its own start-up line
# can tell. An empty log fails here too, which is what a journald-only Qt gives.
if ! grep -qE 'controller database: bundled dbVersion [1-9][0-9]* with [1-9][0-9]* records' "${log}"; then
    fail "the app did not log loading its bundled controller database"
fi
if [ "${started}" != 1 ]; then
    fail "the app exited on its own (${rc}) instead of staying up"
fi
# SIGTERM is what a logout sends. Ignoring it means ~AppModel never runs, so
# the input thread is not stopped and QSettings never writes what the session
# changed: the user loses their settings every time they log out.
if [ "${needed_kill}" = 1 ]; then
    fail "the app did not exit on SIGTERM; it needed SIGKILL"
fi
if [ "${rc}" != 0 ]; then
    fail "the app shut down on SIGTERM but exited ${rc}"
fi
echo "::notice::the app came up, loaded its controller database, shut down on SIGTERM and exited 0"
