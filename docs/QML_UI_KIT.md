# QML UI kit

The component library and the shell conventions the Qt Quick UI is built from.
Together with [`QML_CONTRACT.md`](QML_CONTRACT.md), which covers the `App` model
surface, this is the whole surface a page is written against.

Everything here lives in the `Dish.Chrome` QML module, one import. Colours come
from the `Theme` singleton and metrics from `Tokens`, both in that module. The
palette values themselves are in [`Theme.cpp`](../src/UI/Theme.cpp), which is
what `test_theme_contrast` computes WCAG ratios over.

## Conventions

**Style.** `QtQuick.Controls.Basic`, imported in every file that uses Controls.
Every kit control is custom-painted from `Theme` tokens. Do not import Material,
Universal or any platform style.

**Imports.** Kit components live in `src/qml/kit/`. From a page in
`src/qml/pages/`:

```qml
import "../kit" as Kit
```

Then `Kit.Card`, `Kit.DishButton`, and so on. Files at the `src/qml/` root use
`import "kit" as Kit`.

**Surfaces.** The window paints `Theme.background`, the themed solid, in both
appearances. `Kit.Page` roots are transparent, and only `Kit.Card` and the
dialogs paint a `Theme.surface` panel on top. A page that paints its own opaque
full-area background is fighting the layering; if you add one, say why in a
comment.

**Tokens, never literals.** No hex colours and no raw pixel values outside
`src/qml/kit/`. `scripts/qml-lint-literals.sh` gates this in CI, and it skips
that one directory and nothing else. The kit is the layer that turns tokens into
pixels, and a token defined in terms of itself is not a token.

**qmllint.** The module declares `DEPENDENCIES QtQuick`, so `Theme`'s `QColor`
properties resolve statically and `unresolved-type` is a real error again. Keep
QML warning-clean: `missing-property`, `unused-imports` and `unresolved-type`
all gate CI. The one accepted downgrade is `unqualified`, because `App` is a
runtime context property the linter cannot see; see
[`QML_CONTRACT.md`](QML_CONTRACT.md).

## Library rules

Each of these closes a hole a design system dies through. They are
review-blocking, not advisory.

| # | Rule |
|---|---|
| C1 | **A page may not declare an inline `component`.** If two files would draw it, it is a kit component. If one file draws it and it has a variant, it is *still* a kit component. |
| C2 | **A kit component owns its own state machine.** Pages never set `opacity` to express disabled, never paint a hover wash, never draw a focus ring. |
| C3 | **A kit component never reads the domain.** No `App.*`, no model roles, no `AppViewModel`. It takes primitives and emits signals. `CapabilityTable` takes the rows `App.capabilityForCandidate()` returns **as data**; the caller passes them in. |
| C4 | **One name per colour.** No aliasing layer. `Theme.primaryHover` **is** the 12 % accent wash, and there is no second name for it. |
| C5 | **No component may stringify a `Theme` colour.** `String(Theme.outline)` yields `#AARRGGBB`, which Canvas 2D parses as `#RRGGBBAA`: silently wrong channels. Always `Qt.rgba(c.r, c.g, c.b, c.a)`. |
| C6 | **Every state a component can be in appears in `KitGallery.qml`.** A state that is not in the gallery is not shipped. |
| C7 | **Two components may not draw the same thing.** Check the inventory before adding one. A second pill style is review-blocking: the "as DualSense" pill and the Verified, Best-fit and Layout-guessed badges are all `CapabilityChip`. |
| C8 | **Glyphs are never recoloured by state.** `BrandGlyph` retints by **palette** (`Theme.glyph`); state colour lives in the dot and the chip. |

Two more bind every file.

**Disabled and unavailable are different mechanisms.** A dead *control* is
`enabled: false` plus `Tokens.disabledOpacity` plus `Theme.disabledFg`, and that
is legal only on an `AbstractButton`. Unavailable *information* gets no opacity
at all: full-opacity `Theme.mutedStrong` plus the outlined absent chip. Never
multiply an already-muted colour by an opacity.

**Focus is global.** Every focusable control gets `focusPolicy: Qt.StrongFocus`,
a 1 px solid `Theme.primary` border, and a 2 px `Theme.focusRing` ring outside
it, on `visualFocus` only.

**Amber is the problem colour, never the working one.** `Theme.warning` is spent
on stale, Needs pairing, unverified and claim failures. Anything merely in
flight (a scan running, a hot but still empty wizard slot) is `Theme.primary`:
nothing is wrong while the app is looking, and an amber spent on "working" stops
meaning anything.

## Component inventory

38 files in `src/qml/kit/`.

### Primitives

| Component | Base | Key API |
|---|---|---|
| `DishButton` | `Button` | `variant: Primary \| Outline \| Destructive` (default `Outline`), `size: Normal \| Small`. The one button type; new code uses this. |
| `KitButton` | `DishButton` | Alias for `variant: Primary`, kept so existing call sites keep working. |
| `OutlineButton` | `DishButton` | Alias for `variant: Outline`. |
| `RowButton` | `AbstractButton` | `title`, `subtitle`. A full-width, focusable, keyboard-activatable list row. Donation rails and license rows are these, never cursor-pointer divs. |
| `ComboButton` | `AbstractButton` | `options`, `value`, `picked(string)`. The drop-down trigger. |
| `SegmentedControl` | `Control` | `options`, `value`, `small`, `busy`, `picked(string)`. Disabled drops the accent thumb for a neutral `Theme.surface` chip with a hairline: `disabledFg` on a saturated accent at `disabledOpacity` is unreadable, and a dead control still has to report which segment is selected. |
| `LabeledSwitch` | `RowLayout` | `label`, `description`, `checked`, `toggled(bool)`. |
| `RadioMark` | `Rectangle` | `selected`. The selection mark inside `SelectRow` and `OptionCard`. |
| `SliderRow` | `ColumnLayout` | `label`, `value`, `maxValue`, `moved(int)` **and** `committed(int)`. Push live on `moved`, persist on `committed`. |
| `KitTextField` | `TextField` | Plus `hasError` and `errorText`: an error message beneath the field, and an `Accessible.description`. |
| `StatusDot` | `Rectangle` | `token` → `"success"` / `"warning"` / `"primary"` / `"error"` / `"muted"`. 10 px with a 1 px darker ring: at 8 px flat, success and warning are a coin flip for a low-vision user, and the ring gives the dot an edge independent of its fill. Never drawn without its chip. |
| `CapabilityChip` | `Rectangle` | `text`, `tone ∈ Present \| Absent \| Low \| Ok \| Warn \| Neutral`. Always renders: an absent capability is drawn, never hidden. |
| `Eyebrow` | `Text` | `mutedTone`. The mono, tracked, uppercase micro-label. |
| `SectionHeader` | `Row` | `label` (natural case in, uppercase out via `font.capitalization`, never an uppercase string) and an optional `glyph`. |
| `BrandGlyph` | `Image` | `glyph`, `glyphForToken(token)`, `tinted`, `accessibleName`. Palette-tinted per C8. |
| `AppMark` | `BrandGlyph` | The identity mark, `dish-logo` (the window icon as a glyph): the title bar and Home's rail entry. `busy` pulses opacity, the one sanctioned brand transient. |
| `LiveStat` | `Text` | `live`, `rateText(hz, measured)`, `latencyText(ms, samples)`. **The only rate and latency formatter in the app.** `~` means derived or estimated; a sub-millisecond latency reads `"<1 ms"`, never `"~0.0 ms"`, mirroring `reducer::formatLatencyMs`, which is what the models render. |
| `DishProgressBar` | `Rectangle` | `indeterminate`, `value`. Track `Theme.surfaceDim`, height fixed at 3, no caller-settable height. Static filled track under `Tokens.reducedMotion`. |
| `LoadingSpinner` | `ColumnLayout` | `text`, `running`. The apply-overlay step spinner and the type-catalog loader. |

### Containers and surfaces

| Component | Base | Key API |
|---|---|---|
| `Card` | `Control` | `filled` (false gives a transparent fill and keeps the hairline) and `dense` (tighter vertical padding). The one deliberately opaque element: content reads against a panel. |
| `ActionCard` | `AbstractButton` | `title`, `subtitle`, `showPlus`, `placeholder` (dashed `Theme.outline` border, no wash, no hover, no focus: an ActionCard that is not an action). |
| `Page` | `Page` | Transparent background, `padding: Tokens.pagePadding`, one page-level `ScrollView` over a default `Column`. Set `scrollable: false` when the page owns its own layout and pins its own regions, which the wizard does. |
| `Callout` | `Control` | `tone ∈ Info \| Warning \| Error`, `text`, optional leading `glyph`, trailing action slot. |
| `EmptyState` | `ColumnLayout` | `title`, `body`, `glyph`, `showAction`, `actionText`, `actionRequested()`. Every list has one. |
| `ErrorBanner` | `Rectangle` | `text`, `detail`, `tone ∈ Error \| Warning`, `showRetry`, `retryText`, `retryRequested()`. An error is a diagnosis **and** a next step, which is what `detail` is for. |
| `NotificationToastHost` | `Item` | `show(message, severity)`, tones `error \| warning \| success` only. A stray `"info"` maps to `success` and warns. The only elevated surface in the app. |
| `DishToolTip` | `ToolTip` | The only tooltip. **Declare it, never attach it**: the attached `ToolTip.text` property resolves its delegate through `QtQuick.Controls`, the style-selecting module, which the pages do not import, so it logs `Component is not ready` and no tip appears. A bare `ToolTip` would paint Basic's `palette.toolTipBase`, an unthemed white slab in both appearances. |
| `SelectRow` | `AbstractButton` | `selected`, `title`, `subtitle`, `glyph`, `dotToken`, `chipText`, `chipTone`, `picked()`. The one selectable radio row: pad picker, host picker, type card, bind destination. Selected is a 1 px `Theme.primary` border plus `Theme.primaryFill`. |
| `OptionCard` | `AbstractButton` | `selected`, `title`, `body`, `badgeText`, `badgeTone`. The two-up choice card (Standard against Direct). |

### Composites

| Component | Base | Draws |
|---|---|---|
| `WireLine` | `Item` | The pad-to-host wire. `live` solid, idle dashed, `transmitting` dashes crawling toward the host during an apply. Home and `WizardBanner` both compose it. |
| `BindingStrip` | `Item` | The binding chip flow plus Edit. Overflows into a real focusable `+N` chip that opens a popup listing the remainder **with their reasons**, never a bare count. |
| `CapabilityTable` | `Item` | The four-layer matrix. `rows` come from the caller, per C3. Check for available, cross for unavailable, em dash in `Theme.mutedStrong` for pending. Every layer chip shows its true state and the first failing one is heavier. |
| `WizardBanner` | `Item` | The pad slot, wire and host slot, plus the stage markers and the stage-3 sub-step dots. `compact` for a short window. |
| `StepList` | `Column` | The apply overlay's steps: done, active with a rotating ring, pending hollow, failed. |
| `ApplyOverlay` | `ContentDialog` | `steps`, `cancellable`, `slowHint`, `cancelRequested()`. Two callers: the wizard's Review page and Configure binding. |
| `ContentDialog` | `Popup` | The dialog shell. See below. |
| `ConfirmDialog` | `ContentDialog` | Plus `bodyText` and `bulletLines`. **Reject has default focus.** The discard confirm, Forget host, Forget controller, and the keep-awake quit confirm. |
| `BlockerDialog` | `ContentDialog` | The two **terminal** blockers only: `ConnectionLost` and `ControllerUnplugged`. An unsteady link is an inline `ErrorBanner`, not a modal. |
| `KitGallery` | `Item` | Every component in every state, with a theme switcher. No runtime entry point; reached by a developer editing `Main.qml`. The C6 artifact. |

## Dialogs

`Kit.ContentDialog` is a centred `Popup` painting a `Theme.surface` card over
`Theme.scrim`, with a 1 px `Theme.outline` border, `Tokens.radiusDialog`, and no
shadow. Declare one anywhere in the page tree and call `open()`: a `Popup`
reparents to the window overlay layer, so it floats above the shell
automatically.

| Property | Type | Default | Meaning |
|---|---|---|---|
| `eyebrow` | string | `""` | The micro-label above the heading. |
| `heading` | string | `""` | Dialog title. |
| `acceptText` | string | `"OK"` | Primary footer button label. Empty hides the button. |
| `rejectText` | string | `"Cancel"` | Outline footer button label. Empty hides the button. |
| `acceptEnabled` | bool | `true` | Gate the accept button, for example until input validates. |
| `destructiveAccept` | bool | `false` | Paint accept as the destructive variant. |
| `preferredWidth` | int | `430` | Dialog width. |
| `body` | `list<QtObject>` alias | | The body content: `body: [ ... ]`. Aliased to the internal column's `data`, so the type is statically known to tooling. |
| `accepted()` | signal | | Accept clicked. **The dialog does not auto-close on accept**; call `close()` yourself after a successful action, and keep it open on error. |
| `rejected()` | signal | | Reject clicked. Auto-closes. |

```qml
Kit.ContentDialog {
    id: pairDialog
    heading: qsTr("Enter pairing PIN")
    acceptText: qsTr("Pair")
    acceptEnabled: pinField.text.length === 6

    body: [
        Kit.KitTextField {
            id: pinField
            placeholderText: qsTr("6-digit PIN")
            maximumLength: 6
            inputMethodHints: Qt.ImhDigitsOnly
        }
    ]

    onAccepted: App.pairByServerId(serverId, pinField.text)  // closes on success
    onRejected: pinField.clear()
}
```

## Pages

A page extends `Kit.Page`. Its default content is a padded `Column`, so just
declare children and they stack.

```qml
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Dish.Chrome
import "../kit" as Kit

Kit.Page {
    readonly property string headerTitle: qsTr("Controllers")
    readonly property string headerSub: qsTr("%n connected", "", App.slotCount)
    readonly property string headerDot: App.slotCount > 0 ? "success" : "muted"

    Kit.Card {
        // page body
    }
}
```

The header is drawn by the shell from the page's `headerTitle`, `headerSub` and
`headerDot`; all three are optional and fall back to the rail label and empty.
The full page contract, including the leave guard and `shellApi`, is in
[`QML_CONTRACT.md`](QML_CONTRACT.md).

The tree:

| Kind | Files |
|---|---|
| Destinations | `pages/HomePage.qml` · `ControllersPage.qml` · `ConnectionsPage.qml` · `DonatePage.qml` · `SettingsPage.qml` |
| Pushed details | `pages/ConfigureBindingPage.qml` · `ControlsRemapPage.qml` · `DeadzoneSettingsPage.qml` · `LicensesPage.qml` |
| Shared dialog | `pages/PairingDialog.qml` |
| Setup wizard | `wizard/SetupWizardPage.qml` plus `WizardInputPage` · `WizardDestinationPage` · `WizardTypePage` · `WizardFeelPage` · `WizardReviewPage` |
| Binding draft | `shared/BindingDraft.qml`, the one draft type; both binding editors instantiate it |
| Onboarding | `onboarding/OnboardingFlow.qml` · `WelcomeScreen.qml` · `HelpScreen.qml` |

If a page needs real logic, it belongs in a tested C++ store or reducer, not in
QML.

Every new `.qml` must be listed in `qt_add_qml_module(... QML_FILES ...)` in
`CMakeLists.txt`, or it will not be in the module at runtime.

## The shell

[`AppShell.qml`](../src/qml/AppShell.qml) is a collapsible left rail plus a
per-page header over a `StackView`. Collapsed, the rail is a 48 px icon strip
continuing the title bar's hamburger cell; expanded it is a 236 px labelled
pane, and the state persists through `App.railCollapsed`.

Overlays are in-scene: dialogs are `Kit.ContentDialog` popups over a scrim, and
transient errors land in the one `NotificationToastHost` the shell drops. No
extra OS windows.

Selecting a rail destination **replaces** the content stack, clearing any pushed
detail pages, so a rail switch is always a fresh root.

### Rail glyphs

Collapsed, each entry has to be identifiable from its picture alone at
`Tokens.glyphSm`, 16 px.

| Entry | Glyph |
|---|---|
| Home | `dish-logo` (the app mark, `Kit.AppMark`'s asset) |
| Controllers | `pad` |
| Connections | `satellite` |
| Support Dish | a heart text glyph in `Theme.pulse` |
| Settings | `gear` |

**No two rail entries may resolve to the same silhouette.** A different *state*
of the same family (`-connected`, `-receiving`, `-off`) is not a different rail
glyph: `dish` and `dish-connected` differ by a 3-unit dot in a 64-unit viewBox,
which is under one pixel at 16 px. Home carries the app mark, the same identity
as `dish.ico` and the title bar, and its ringed-dish silhouette cannot be read
as the wire vocabulary's plain dish. The plain set holds three separable
shapes, the tilted dish ellipse, the satellite's panel bar and the gear disc,
and the rail needs more, which is why `brand/pad.svg` exists.

## Brand glyphs

`Kit.BrandGlyph` renders `qrc:/brand/<glyph>.svg` through Qt6::Svg. Set `width`
and `height` and pass a bare asset name, no path and no extension.

| Property | Type | Default | Meaning |
|---|---|---|---|
| `glyph` | string | `"satellite"` | Bare asset name, e.g. `"satellite-connected"`. |
| `tinted` | bool | `true` | Route through the palette colourisation. |
| `accessibleName` | string | `""` | Announced name; empty means decorative. |
| `glyphForToken(token)` | function → string | | Maps a model `glyph` token (`"satelliteBase"` / `"satelliteConnected"` / `"satelliteOff"`) to an asset name. |

The `:/brand/` set: `dish`, `dish-connected`, `dish-disabled`, `dish-logo`,
`dish-master`, `dish-off`, `dish-receiving`, `satellite`,
`satellite-broadcasting`, `satellite-connected`, `satellite-disabled`,
`satellite-master`, `satellite-off`, `bluetooth`, `bluetooth-connected`,
`bluetooth-disabled`, `bluetooth-off`, `bluetooth-searching`, `gear`, `pad`.

```qml
Kit.BrandGlyph {
    width: Tokens.glyphSm; height: Tokens.glyphSm
    glyph: glyphForToken(glyph)   // `glyph` here is the model role
}
```

Two things to know:

**The `*-animated.svg` variants are unreachable through `BrandGlyph`.** They
contain no `<animate>` elements and Qt runs no SMIL, so they are static files
that merely look like states. Express a transient in QML instead: a
`DishProgressBar`, or an opacity or rotation animation over the **base** glyph,
gated on `Tokens.reducedMotion` and `visible`. The qrc entries stay, because
deleting them buys nothing and risks a missing-asset regression when the brand
set is resynced from the sibling repositories.

**Glyph contrast rides `Theme.glyph`,** a palette token. `BrandGlyph` renders
the raw image on dark and routes through a colourisation effect only on light,
because the baked SVG hex computes to 1.7:1 on a white card.

## Design tokens

`Tokens` ([`TokensBridge.h`](../src/qml/chrome/TokensBridge.h)) carries every
non-colour metric. All values are `CONSTANT` except `reducedMotion`.

| Group | Names |
|---|---|
| Type scale (px) | `textDisplay` 26 · `textHero` 21 · `textTitle` 20 · `textStatus` 17 · `textHeading` 16 · `textBase` 13 · `textSummary` 12 · `textMeta` 11 · `textChip` 10 |
| Type treatment | `sectionLetterSpacing` 1.5 · `monoFamily` · `sansFamily` |
| Spacing (px) | `s0` 0 · `s1` 2 · `s2` 4 · `s3` 6 · `s4` 8 · `s5` 10 · `s6` 12 · `s7` 14 · `s8` 16 · `s9` 20 · `s10` 24 · `s11` 32 · `pagePadding` 24 |
| Radii (px) | `radiusBar` 2 · `radiusChip` 5 · `radiusButton` 6 · `radiusCard` 8 · `radiusDialog` 10 |
| Glyph rungs (px) | `glyphSm` 16 · `glyphMd` 20 · `glyphLg` 28 · `glyphXl` 40 · `glyphHero` 76 |
| Durations (ms) | `durFast` 120 · `durNormal` 200 · `durBusy` 1100 · `durToast` 4000 |
| Shell metrics (px) | `titleBarHeight` 44 · `captionButtonWidth` 46 · `railCompact` 48 · `railExpanded` 236 · `navItemHeight` 40 · `hitRow` 44 · `minTouch` 32 · `dotSize` 8 |
| Breakpoints (px) | `minWindowWidth` 900 · `minWindowHeight` 620 · `narrowBreakpoint` 860 · `stackBreakpoint` 760 · `wideBreakpoint` 980 |
| Other | `disabledOpacity` 0.55 · `reducedMotion` |

`s0` is **flush**, not "unset": a seam the design draws with a divider or a
colour change instead of a gap. Naming it makes those sites read as a decision
and gives the literal scanner something to point at.

`sansFamily` and `monoFamily` are probed explicitly by `ui::FontStacks` rather
than trusting the platform generic, which can hand back Courier New.
`test_font_stacks` pins the probe.

`reducedMotion` mirrors the OS "animate controls inside windows" setting,
inverted, and is the one `NOTIFY` token. When true, indeterminate bars become a
static filled track, glyph animations stop, and `Behavior` durations go to 0.
Neither probe carries a change signal a Quick app can bind to, so `Main.qml` calls
`Tokens.refreshMotionPreference()` whenever the window regains activation.

`Theme` ([`ThemeBridge.h`](../src/qml/chrome/ThemeBridge.h)) carries the colour
roles. All of them `NOTIFY paletteChanged`, so a palette swap re-reads every
binding.

| Group | Roles |
|---|---|
| Surfaces | `background` · `surface` · `surfaceDim` · `outline` · `outlineSubtle` · `scrim` |
| Content | `onSurface` · `muted` · `mutedStrong` · `disabledFg` · `glyph` |
| Accent | `primary` · `primaryDark` · `onPrimary` · `primaryHover` (12 %) · `primaryPress` (18 %) · `accentWash24` (24 %) · `primaryFill` · `focusRing` |
| Status | `success` · `successFill` · `warning` · `warningFill` · `error` · `errorFill` |
| Donation | `pulse` · `pulseFill` (12 %) · `pulseEdge` (35 %) |

`Theme.alpha(color, 0..1)` re-alphas any token, so a one-off wash never has to
be spelled as a raw `Qt.rgba` literal in a page.

Four roles are easy to confuse:

- `muted` is secondary text that is present and readable.
- `mutedStrong` is drawn-but-unavailable **information**, always full opacity.
- `disabledFg` is the foreground of a dead **control**, used with
  `Tokens.disabledOpacity`.
- `glyph` is the brand-glyph tint, retinted by palette and never by state.

Text on any filled accent control is `Theme.onPrimary`, never
`Theme.background`. That includes the pulse surfaces.

`pulse` is the one hue Dish uses beyond cyan, reserved for the Support Dish
surface and its rail heart.
