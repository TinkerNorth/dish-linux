# dish-bin — AUR packaging

`PKGBUILD` re-bundles the release AppImage for Arch users, with the udev
rule, desktop entry, icon and AppStream metadata the bare AppImage cannot
install. Mirrors `TinkerNorth/satellite` `packaging/aur/`.

## First publish (one-time)

The AUR is a separate git remote owned by an AUR account, not something CI
can push to from this repo. After the first release is published:

```bash
# 1. Fill the real checksums (currently 64-zero placeholders):
cd packaging/aur
updpkgsums                       # rewrites sha256sums_x86_64 in PKGBUILD
# Cross-check the AppImage line against the release's SHA256SUMS asset.

# 2. Regenerate .SRCINFO from the PKGBUILD:
makepkg --printsrcinfo > .SRCINFO

# 3. Build + install locally to verify:
makepkg -si

# 4. Push to the AUR (needs an AUR account with an SSH key registered):
git clone ssh://aur@aur.archlinux.org/dish-bin.git /tmp/dish-bin-aur
cp PKGBUILD .SRCINFO dish-bin.install /tmp/dish-bin-aur/
cd /tmp/dish-bin-aur && git add -A && git commit -m "dish-bin 0.1.0-1" && git push
```

## Every release after that

Bump `pkgver`, reset `pkgrel=1`, run `updpkgsums`, regenerate `.SRCINFO`,
copy the three files into the AUR clone, commit, push. (A `release.yml`
step could template this; it is manual for now because the AUR SSH key is
personal, not a repo secret.)

## Name note

The bare AUR name `dish` is owned by an unrelated package (an interactive
shell), so this package neither provides nor conflicts with it — see the
comment in `PKGBUILD`.
