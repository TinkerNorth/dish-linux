#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Fails an SPDX document that does not name every library the app ships:
#
#   scripts/check-sbom.sh <components.spdx.json>
#
# The list of what ships is assets/licenses/licenses.json, the manifest the
# in-app Licenses screen renders. Every entry but Catch2 (test-only, never in
# a package) and libcurl, the Sentry SDK's transport, which a build ships only
# when the SDK is linked into the AppImage (a release build; the Flatpak takes
# it from the KDE runtime), must appear in the document as a package of that
# name, case aside; where the manifest names an exact version (not a floor such as
# "6.7+", nor "system"), a package of that name must carry it, a short commit
# matching its full form.
set -euo pipefail
cd "$(dirname "$0")/.."

sbom="${1:?usage: $0 <components.spdx.json>}"
python3 - "${sbom}" <<'PY'
import json, re, sys

sbom = sys.argv[1]
document = json.load(open(sbom, encoding="utf-8"))
libraries = json.load(open("assets/licenses/licenses.json", encoding="utf-8"))["libraries"]
missing = []
for library in libraries:
    artifact = library["artifact"]
    if artifact == "Catch2":
        continue
    if artifact == "libcurl" and not any(
            p["name"] == "sentry-native" and "the AppImage" in p.get("comment", "")
            for p in document["packages"]):
        continue
    named = [p for p in document["packages"] if p["name"].lower() == artifact.lower()]
    if not named:
        missing.append(f"{artifact} (no package of that name)")
        continue
    wanted = library["version"]
    if wanted.endswith("+") or wanted == "system":
        continue
    versions = [p["versionInfo"] for p in named]
    ok = any(v == wanted or (re.fullmatch(r"[0-9a-f]{7,39}", wanted) and v.startswith(wanted))
             for v in versions)
    if not ok:
        missing.append(f"{artifact} {wanted} (the document says {', '.join(versions)})")
if missing:
    for entry in missing:
        print(f"::error::{sbom} does not name {entry}")
    sys.exit(1)
print(f"{sbom}: every library assets/licenses/licenses.json lists is named ({len(libraries) - 1})")
PY
