#!/usr/bin/env bash
# build-pacman-repo.sh: add signed dish-bin packages to the pacman repository
# tree under a given pages directory.
#
# The layout (rooted at $PAGES_DIR):
#
#   arch/x86_64/dish-bin-<ver>-1-x86_64.pkg.tar.zst{,.sig}
#   arch/x86_64/dish.db{,.tar.gz}{,.sig}
#   arch/x86_64/dish.files{,.tar.gz}{,.sig}
#
# Usage (release.yml's arch-publish job, and linux-ci.yml's pull-request lane
# with a throwaway key):
#   build-pacman-repo.sh <pages-dir> <package-dir>
#
# Requires: pacman's repo-add, gpg (with $GPG_KEY_ID imported).
set -euo pipefail

PAGES_DIR="${1:-}"
PACKAGE_DIR="${2:-}"

if [ -z "$PAGES_DIR" ] || [ -z "$PACKAGE_DIR" ]; then
    echo "Usage: $0 <pages-dir> <package-dir>" >&2
    exit 2
fi
if [ -z "${GPG_KEY_ID:-}" ]; then
    echo "GPG_KEY_ID must be set (the public key fingerprint or short ID)." >&2
    exit 2
fi

shopt -s nullglob
pkgs=("$PACKAGE_DIR"/*.pkg.tar.zst)
if [ "${#pkgs[@]}" -eq 0 ]; then
    echo "No *.pkg.tar.zst in $PACKAGE_DIR" >&2
    exit 1
fi

repo_dir="$PAGES_DIR/arch/x86_64"
mkdir -p "$repo_dir"
for pkg in "${pkgs[@]}"; do
    gpg --batch --yes --default-key "$GPG_KEY_ID" --detach-sign "$pkg"
    cp "$pkg" "$pkg.sig" "$repo_dir/"
done

(
    cd "$repo_dir"
    for pkg in "${pkgs[@]}"; do
        repo-add --sign --key "$GPG_KEY_ID" dish.db.tar.gz "./$(basename "$pkg")"
    done
    # repo-add emits dish.db / dish.files as symlinks to the .tar.gz files;
    # GitHub Pages does not serve symlinks, so replace each with a real copy.
    find . -maxdepth 1 -type l -print0 | while IFS= read -r -d '' link; do
        target="$(readlink -f "$link")"
        rm "$link"
        cp "$target" "$link"
    done
)
ls -l "$repo_dir"
