# Flathub submission

Flathub is the software-centre path (GNOME Software, KDE Discover) on every
distro, the only path on Steam Deck's immutable desktop, and the answer for
an LTS whose Qt is below this project's floor. Once listed, updates are
automatic: the store rebuilds every tagged release. The manifest is ready at
[`packaging/flatpak/flathub/com.tinkernorth.Dish.yml`](../packaging/flatpak/flathub/com.tinkernorth.Dish.yml);
this file is the submission procedure.

## Before submitting

1. A published release must exist. The manifest builds from a pinned git
   tag; fill its `commit:` placeholder with `git rev-list -n 1 <tag>`.

2. Screenshots are in `docs/screenshots/` and referenced from the
   AppStream metainfo, pinned to the release tag. When retaking them,
   update the tag in the URLs; a published listing must not have its
   images change underneath it.

3. Run Flathub's own linter. It checks more than `appstreamcli validate`:

   ```sh
   flatpak install -y flathub org.flatpak.Builder
   flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
     manifest packaging/flatpak/flathub/com.tinkernorth.Dish.yml
   flatpak run --command=flatpak-builder-lint org.flatpak.Builder \
     appstream packaging/com.tinkernorth.Dish.metainfo.xml
   ```

## Submitting

1. Fork `github.com/flathub/flathub` and branch from `new-pr`.
2. Add the flathub-variant manifest (commit pin filled) as
   `com.tinkernorth.Dish.yml`.
3. Open the PR against the `new-pr` branch and fill in their template.
   Test instructions: the app runs standalone; pairing needs a Satellite
   server on the same network.
4. Expect review pushback on `--device=all`. The position is written into
   the manifest comment: there is no hidraw portal, and the USB portal
   covers raw USB devices, not hidraw character nodes. If the reviewer
   holds the line, take `--device=input`: the SDL path still works,
   USB-direct degrades exactly as [PACKAGING.md](PACKAGING.md) documents,
   and a user restores it with
   `flatpak override --user --device=all com.tinkernorth.Dish`.
   A listed app with the narrower grant beats an unlisted one.

## After acceptance

- Flathub creates `flathub/com.tinkernorth.Dish`; that copy is the build
  source of truth. Mirror any change back into `packaging/flatpak/` here.
- The manifest's `x-checker-data` block lets Flathub's external-data-checker
  open the version-bump PR there on every new tag. Verify the first one.
- Point the README and the Pages landing page at Flathub for Ubuntu and
  older-LTS users.
- The in-app update check already defaults off inside the sandbox
  (`UpdatePreferenceStore`); the store owns delivery.
- Consider dropping the `.flatpak` release asset once the listing is
  established: a sideloaded bundle has no update origin.
