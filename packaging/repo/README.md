# Linux package repositories on GitHub Pages

This directory holds everything needed to publish the `dish` `.deb`
and `.rpm` releases as proper APT and DNF/YUM repositories on
`https://tinkernorth.github.io/dish-linux/`.

Ported from `TinkerNorth/satellite` `packaging/repo/`; the two directories
share their layout and scripts — keep them in step.

## End-user install (Debian 13+ and derivatives)

The `.deb` targets distros whose Qt meets the 6.7 floor (Debian 13 ships
6.8). Ubuntu 24.04 LTS ships Qt 6.4.2 — its answer is the AppImage or the
Flatpak, not this repo; see `docs/PACKAGING.md`.

```bash
# Add the signing key (one-time):
curl -fsSL https://tinkernorth.github.io/dish-linux/gpg.key \
  | sudo gpg --dearmor -o /usr/share/keyrings/dish-archive-keyring.gpg

# Add the repo (one-time):
echo "deb [signed-by=/usr/share/keyrings/dish-archive-keyring.gpg] \
  https://tinkernorth.github.io/dish-linux/debian stable main" \
  | sudo tee /etc/apt/sources.list.d/dish.list

# Install + future upgrades via apt:
sudo apt update
sudo apt install dish
```

## End-user install (Fedora / RHEL / Rocky / Alma / openSUSE)

```bash
# Drop the .repo file (it already references the gpg key). One-time:
sudo curl -fsSL -o /etc/yum.repos.d/dish.repo \
  https://tinkernorth.github.io/dish-linux/rpm/dish.repo

# Install + future upgrades via dnf:
sudo dnf install dish
```

## Signing key

The repository signing key is:

```
pub   ed25519 2026-08-23
      96FF AACB 78FE 75D1 8CEE  E332 C398 1795 12D6 BDF3
uid   Dish Releases <releases@tinkernorth.invalid>
```

The private key lives only in the `GPG_PRIVATE_KEY` / `GPG_KEY_ID`
repository secrets (and the maintainer's password manager); the public half
is committed here as `gpg.key` and published at the repo root of the Pages
site. This fingerprint is the pin — a Pages compromise cannot silently swap
the key without this README (in git history) disagreeing.

Rotation: generate a new key, add its public half here alongside the old
one, sign one release with both (`gpg.key` may contain multiple public
keys), then retire the old key in a follow-up release. Removing a key users
have pinned breaks `apt update` for them — announce first.

## How publishing works in CI

The `release.yml` workflow runs an `apt-publish` and `rpm-publish` job
after the main `publish` job succeeds. Both:

1. Check out the `gh-pages` branch into a working directory (creating an
   orphan branch with this directory's `index.html` on first publish).
2. Import the GPG signing key from the `GPG_PRIVATE_KEY` secret and skip
   gracefully (with a warning) when it is absent, because unsigned repo
   metadata is worse than no repo at all.
3. Run `build-apt-repo.sh` / `build-dnf-repo.sh` from this directory to
   stage the new package, regenerate indices, and sign them.
4. Commit and push `gh-pages`.

One-time repo setup: GitHub Pages must be enabled for the `gh-pages`
branch (Settings → Pages → Deploy from branch → `gh-pages` / root) after
the first release creates it.
