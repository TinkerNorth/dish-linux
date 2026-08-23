# Flathub submission runbook

Getting Dish onto Flathub is the single biggest distribution unlock on
Linux: it is the software-center path (GNOME Software / KDE Discover) on
every distro, the only sane path on Ubuntu LTS (whose Qt is below our
floor), the only path on Steam Deck's immutable desktop, and it makes
updates automatic. The manifest is ready at
[`packaging/flatpak/flathub/com.tinkernorth.Dish.yml`](../packaging/flatpak/flathub/com.tinkernorth.Dish.yml);
what remains is content and process.

## Blockers before submitting

1. **A published release.** The Flathub manifest builds from a pinned
   git tag + commit. Fill the `commit:` placeholder with
   `git rev-list -n 1 <tag>`.

2. **Screenshots in the AppStream metainfo.** Flathub's quality guidelines
   require at least one screenshot; without them the listing is also just
   worse everywhere. A commented template block sits in
   [`packaging/com.tinkernorth.Dish.metainfo.xml`](../packaging/com.tinkernorth.Dish.metainfo.xml)
   — capture 2–3 shots (controllers page, binding page; 16:9, PNG,
   1600×900 or better, both themes if convenient), host them at stable
   HTTPS URLs (e.g. committed under `docs/screenshots/` and referenced via
   `raw.githubusercontent.com` at a tag), uncomment, re-run
   `appstreamcli validate --no-net`.

3. **Lint pass.** Run Flathub's own linter locally:

   ```bash
   flatpak install -y flathub org.flatpak.Builder
   flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
     manifest packaging/flatpak/flathub/com.tinkernorth.Dish.yml
   flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
     appstream packaging/com.tinkernorth.Dish.metainfo.xml
   ```

## The `--device=all` conversation

Expect reviewer pushback on `--device=all`. The position, already written
into the manifest comments: there is no hidraw portal, and the USB portal
covers raw USB devices, not hidraw character nodes. If the reviewer holds
the line, accept `--device=input` — the SDL path still works, USB-direct
degrades exactly as documented in
[`PACKAGING.md`](PACKAGING.md#the-udev-rule-is-not-optional), and users
who want USB-direct restore it with
`flatpak override --user --device=all com.tinkernorth.Dish`. Do not let
this stall the submission; a listed app with a narrower grant beats an
unlisted perfect one.

## Submission process

1. Fork `github.com/flathub/flathub`, branch `new-pr` → `add-com.tinkernorth.Dish`.
2. Add `com.tinkernorth.Dish.yml` (the flathub variant, commit filled).
3. Open the PR against `flathub/flathub`'s `new-pr` branch; fill their
   template (test instructions: pair against a Satellite server, or note
   the app is usable standalone for binding/config).
4. After acceptance a `flathub/com.tinkernorth.Dish` repo is created —
   that copy becomes the build source of truth. Mirror any change back
   into `packaging/flatpak/` here.
5. The `x-checker-data` block in the manifest lets Flathub's
   external-data-checker open version-bump PRs there automatically on
   every new release tag; verify the first one, then it's hands-off.

## After it's live

- Update `README.md` and the Pages landing page to point Ubuntu/older-LTS
  users at Flathub first, bundle sideload second.
- The in-app update check already stands down inside the sandbox
  (`UpdatePreferenceStore` defaults it off under Flatpak) — Flathub owns
  delivery.
- Consider retiring the `.flatpak` bundle release asset once Flathub is
  established; a sideloaded bundle has no update origin.
