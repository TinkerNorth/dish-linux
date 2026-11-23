#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# The release workflow's two Sentry calls, with the pinned sentry-cli from
# scripts/tools.sh:
#
#   scripts/sentry-cli.sh symbols <build tree>     upload the unstripped dish binary's debug info
#   scripts/sentry-cli.sh debug-tree <directory>   upload every debug file under a directory (Flatpak)
#   scripts/sentry-cli.sh release <tag>            create, associate commits, finalize, deploy
#
# Reads SENTRY_DSN and SENTRY_AUTH_TOKEN, and SENTRY_ORG / SENTRY_PROJECT when
# set. Without a token or a DSN it says so and does nothing: a fork, or a
# repository without the secrets, still releases. The org and project fall back
# to the numeric ids inside the DSN, which the Sentry API accepts in place of
# slugs.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

action="${1:-}"
target="${2:-}"
if [ -z "${action}" ] || [ -z "${target}" ]; then
    echo "usage: $0 symbols <build tree> | debug-tree <directory> | release <tag>" >&2
    exit 2
fi

if [ -z "${SENTRY_AUTH_TOKEN:-}" ] || [ -z "${SENTRY_DSN:-}" ]; then
    echo "::notice::SENTRY_AUTH_TOKEN or SENTRY_DSN unset; skipping sentry-cli ${action}."
    exit 0
fi
if [ -z "${SENTRY_ORG:-}" ]; then
    SENTRY_ORG="$(sed -nE 's#.*@o([0-9]+)\..*#\1#p' <<<"${SENTRY_DSN}")"
    export SENTRY_ORG
fi
if [ -z "${SENTRY_PROJECT:-}" ]; then
    SENTRY_PROJECT="${SENTRY_DSN##*/}"
    export SENTRY_PROJECT
fi

cli="${RUNNER_TEMP:-/tmp}/sentry-cli"
fetch_pinned \
    "https://github.com/getsentry/sentry-cli/releases/download/${DISH_SENTRY_CLI_VERSION}/sentry-cli-Linux-x86_64" \
    "${cli}" "${DISH_SENTRY_CLI_LINUX_X86_64_SHA256}"
chmod +x "${cli}"

case "${action}" in
symbols)
    # The build-tree binary is the unstripped one; packaging strips its own copy.
    bin="$(find "${target}" -maxdepth 2 -type f -name dish -perm -u+x -print -quit)"
    if [ -z "${bin}" ]; then
        echo "::warning::no dish binary under ${target}"
        exit 0
    fi
    "${cli}" debug-files upload --include-sources "${bin}"
    ;;
debug-tree)
    if [ ! -d "${target}" ]; then
        echo "::warning::no debug tree at ${target}"
        exit 0
    fi
    "${cli}" debug-files upload "${target}"
    ;;
release)
    version="dish-linux@${target}"
    "${cli}" releases new "${version}"
    "${cli}" releases set-commits "${version}" --auto
    "${cli}" releases finalize "${version}"
    "${cli}" deploys new -e production -r "${version}"
    ;;
*)
    echo "unknown action: ${action}" >&2
    exit 2
    ;;
esac
