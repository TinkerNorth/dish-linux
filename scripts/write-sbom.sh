#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Writes the SPDX bill of materials of what the release's packages carry:
#
#   scripts/write-sbom.sh <AppDir> <out.spdx.json>
#
# Syft, which the release hardening runs over the release directory, sees a
# .deb, an .rpm, an AppImage and a Flatpak bundle and names almost nothing
# inside them. This document is written from the build instead, at the versions
# the build used, and the shared _release_harden.yml folds it into the product
# SBOM (Syft's sbom-cataloger) and scans that:
#
#   * in every package: ENet at DISH_ENET_COMMIT (vendored), Inter 4.001
#     (compiled in as resources);
#   * in the AppImage: every shared library in <AppDir>/usr/lib, the Qt ones as
#     qtbase at DISH_QT_VERSION (the official binaries), SDL2 at
#     DISH_SDL2_VERSION (built from source), every other one as the
#     distribution package the build host installed it from, at that package's
#     version; and sentry-native when the AppImage's dish links it;
#   * in the Flatpak: the modules packaging/flatpak/com.tinkernorth.Dish.yml
#     builds (libsodium, SDL2, sentry-native at the scripts/tools.sh pins), and
#     the KDE runtime it runs on, which provides Qt, Opus, OpenSSL and libcurl.
#
# The .deb and the .rpm bundle no library: they depend on the distribution's.
# Each package's comment says which artifacts carry it. scripts/check-sbom.sh
# fails a document that misses a library assets/licenses/licenses.json lists.
# appimage-ci.yml and release.yml's appimage job run both after the AppImage
# is built, on its build host.
set -euo pipefail
cd "$(dirname "$0")/.."
# shellcheck source=scripts/tools.sh
source scripts/tools.sh

appdir="${1:?usage: $0 <AppDir> <out.spdx.json>}"
out="${2:?usage: $0 <AppDir> <out.spdx.json>}"
[ -d "${appdir}/usr/lib" ] || { echo "write-sbom: ${appdir} has no usr/lib" >&2; exit 1; }

version="$(awk '/^project\(Dish/ { inproj = 1 }
                inproj && /^[[:space:]]*VERSION[[:space:]]/ { print $2; exit }' CMakeLists.txt)"

# One line per bundled library: file<TAB>dpkg package<TAB>package version, from
# the build host's package database (empty for a library no package owns).
bundled="$(mktemp)"
trap 'rm -f "${bundled}"' EXIT
while IFS= read -r -d '' lib; do
    name="$(basename "${lib}")"
    owner=""
    pkg_version=""
    if command -v dpkg >/dev/null 2>&1; then
        for dir in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu /usr/lib /lib; do
            if [ -e "${dir}/${name}" ]; then
                owner="$(dpkg -S "${dir}/${name}" 2>/dev/null | head -n1 | cut -d: -f1 || true)"
                [ -n "${owner}" ] && break
            fi
        done
        [ -n "${owner}" ] && pkg_version="$(dpkg-query -W -f '${Version}' "${owner}" 2>/dev/null || true)"
    fi
    printf '%s\t%s\t%s\n' "${name}" "${owner}" "${pkg_version}" >> "${bundled}"
done < <(find "${appdir}/usr/lib" -maxdepth 1 -type f -name '*.so*' -print0 | sort -z)

sentry_in_appimage=false
if nm "${appdir}/usr/bin/dish" 2>/dev/null | grep -q ' sentry_init$'; then sentry_in_appimage=true; fi

python3 - "${bundled}" "${out}" "${version}" "${sentry_in_appimage}" <<'PY'
import datetime, json, os, re, sys, uuid

bundled_path, out, version, sentry_in_appimage = sys.argv[1:]
env = os.environ
qt, sdl, sodium = env["DISH_QT_VERSION"], env["DISH_SDL2_VERSION"], env["DISH_LIBSODIUM_VERSION"]
sentry, enet, kde = env["DISH_SENTRY_NATIVE_VERSION"], env["DISH_ENET_COMMIT"], env["DISH_KDE_RUNTIME"]

packages = {}

def add(name, version, licence, download, carried_by, purl=""):
    key = (name, version)
    if key in packages:
        packages[key]["carried"].update(carried_by)
        return
    packages[key] = {"name": name, "version": version, "licence": licence,
                     "download": download, "carried": set(carried_by), "purl": purl}

every = ["the .deb", "the .rpm", "the AppImage", "the Flatpak"]
add("enet", enet, "MIT", f"git+https://github.com/cgutman/enet@{enet}", every,
    f"pkg:github/cgutman/enet@{enet}")
add("inter", "4.001", "OFL-1.1", "https://github.com/rsms/inter", every)

# The AppImage's libraries.
upstream = [  # dpkg package name pattern -> the upstream name licenses.json uses
    (r"^libsodium\d+$", "libsodium", "ISC"),
    (r"^libopus\d+$", "opus", "BSD-3-Clause"),
    (r"^libssl\d", "openssl", "Apache-2.0"),
    (r"^libcurl\d", "libcurl", "curl"),
]
for line in open(bundled_path, encoding="utf-8"):
    file_name, owner, pkg_version = (line.rstrip("\n").split("\t") + ["", ""])[:3]
    if re.match(r"^libQt6", file_name) or re.match(r"^libicu", file_name):
        add("qtbase", qt, "LGPL-3.0-only", "https://download.qt.io/official_releases/qt/",
            ["the AppImage"], f"pkg:generic/qt/qtbase@{qt}")
        continue
    if re.match(r"^libSDL2", file_name):
        add("SDL2", sdl, "Zlib", f"https://github.com/libsdl-org/SDL/releases/tag/release-{sdl}",
            ["the AppImage"], f"pkg:generic/libsdl-org/SDL2@{sdl}")
        continue
    if not owner:
        add(file_name, "NOASSERTION", "NOASSERTION", "NOASSERTION", ["the AppImage"])
        continue
    for pattern, name, licence in upstream:
        if re.match(pattern, owner):
            plain = re.sub(r"^\d+:", "", pkg_version).split("-")[0].split("+")[0]
            add(name, plain, licence, "NOASSERTION", ["the AppImage"],
                f"pkg:deb/ubuntu/{owner}@{pkg_version}")
            break
    else:
        add(owner, pkg_version or "NOASSERTION", "NOASSERTION", "NOASSERTION", ["the AppImage"],
            f"pkg:deb/ubuntu/{owner}@{pkg_version}" if pkg_version else "")
if sentry_in_appimage == "true":
    add("sentry-native", sentry, "MIT", "https://github.com/getsentry/sentry-native",
        ["the AppImage", "the .deb", "the .rpm"], f"pkg:github/getsentry/sentry-native@{sentry}")

# The Flatpak's modules, and the runtime that carries the rest.
add("libsodium", sodium, "ISC", f"https://github.com/jedisct1/libsodium/releases/tag/{sodium}-RELEASE",
    ["the Flatpak"], f"pkg:github/jedisct1/libsodium@{sodium}")
add("SDL2", sdl, "Zlib", f"https://github.com/libsdl-org/SDL/releases/tag/release-{sdl}",
    ["the Flatpak"], f"pkg:generic/libsdl-org/SDL2@{sdl}")
add("sentry-native", sentry, "MIT", "https://github.com/getsentry/sentry-native", ["the Flatpak"],
    f"pkg:github/getsentry/sentry-native@{sentry}")
for name, licence in (("qtbase", "LGPL-3.0-only"), ("opus", "BSD-3-Clause"),
                      ("openssl", "Apache-2.0"), ("libcurl", "curl")):
    add(name, f"org.kde.Platform//{kde}", licence, "https://flathub.org/", ["the Flatpak (from the KDE runtime)"])

root = "SPDXRef-Package-dish-linux"
spdx_packages = [{
    "name": "dish-linux", "SPDXID": root, "versionInfo": version,
    "downloadLocation": "https://github.com/TinkerNorth/dish-linux", "filesAnalyzed": False,
    "licenseConcluded": "LGPL-3.0-or-later", "licenseDeclared": "LGPL-3.0-or-later",
    "copyrightText": "Copyright (C) 2026 Dish contributors.",
}]
relationships = [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                  "relatedSpdxElement": root}]
for i, ((name, pkg_version), p) in enumerate(sorted(packages.items())):
    ident = f"SPDXRef-Package-{i}-" + re.sub(r"[^A-Za-z0-9.-]", "-", name)
    entry = {
        "name": name, "SPDXID": ident, "versionInfo": pkg_version,
        "downloadLocation": p["download"], "filesAnalyzed": False,
        "licenseConcluded": p["licence"], "licenseDeclared": p["licence"],
        "copyrightText": "NOASSERTION",
        "comment": "Carried by " + ", ".join(sorted(p["carried"])) + ".",
    }
    if p["purl"]:
        entry["externalRefs"] = [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                                  "referenceLocator": p["purl"]}]
    spdx_packages.append(entry)
    relationships.append({"spdxElementId": root, "relationshipType": "CONTAINS",
                          "relatedSpdxElement": ident})
document = {
    "spdxVersion": "SPDX-2.3", "dataLicense": "CC0-1.0", "SPDXID": "SPDXRef-DOCUMENT",
    "name": f"dish-linux-{version}-components",
    "documentNamespace": f"https://github.com/TinkerNorth/dish-linux/sbom/{version}/{uuid.uuid4()}",
    "creationInfo": {
        "created": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "creators": ["Organization: TinkerNorth", "Tool: dish-linux-scripts/write-sbom.sh"],
    },
    "packages": spdx_packages,
    "relationships": relationships,
}
with open(out, "w", encoding="utf-8") as f:
    json.dump(document, f, indent=2)
    f.write("\n")
print(f"Wrote {out}: {len(packages)} components of dish-linux {version}")
PY
