# Design tokens

Every theme value lives in [`src/UI/Theme.h`](src/UI/Theme.h) and
[`src/UI/Theme.cpp`](src/UI/Theme.cpp). This file is the reference for what the
names mean. For the components built on top of them, see
[`docs/QML_UI_KIT.md`](docs/QML_UI_KIT.md).

Token names match the cross-client TinkerNorth schema. When you change a value,
change the matching token in the sibling clients too.

## Palettes

Dish ships two palettes, dark (default) and light, selected at runtime by the
theme mode. Every role has a value in both; `Theme::primary` and friends resolve
to the active palette, so the whole app re-themes on a mode switch.

### Dark

Cyan on deep space.

| Token | Value | Role |
|---|---|---|
| `background` | `0xFF060818` | Body |
| `surface` | `0xFF0C1027` | Card |
| `surfaceDim` | `0xFF131A3A` | Recessed |
| `primary` | `0xFF4FE3FF` | Accent, cyan |
| `primaryDark` | `0xFF2C93AD` | Pressed / disabled accent |
| `onPrimary` | `0xFF060818` | Text on primary |
| `onSurface` | `0xFFE6ECFF` | Body text |
| `muted` | `0xFF93A0C8` | Secondary text |
| `outline` | `0x2E4FE3FF` | Borders, cyan at ~18% alpha |
| `success` | `0xFF22C55E` | Status |
| `error` | `0xFFE74C3C` | Status |
| `warning` | `0xFFF59E0B` | Status |
| `pulse` | `0xFFFF6FB5` | Donation accent, the one hue beyond cyan |
| `glyph` | `0xFF8FCFE3` | Brand-glyph tint |
| `disabledFg` | `0xFF6C7799` | Disabled-control foreground |
| `mutedStrong` | `0xFF7E8CB4` | Drawn-but-unavailable information |

### Light

Body and surface roles invert to near-white, the accent darkens (bright cyan on
white is illegible), and the status hues shift to AA-contrast-on-light variants.

| Token | Value | Role |
|---|---|---|
| `background` | `0xFFF5F7FC` | Body, soft off-white |
| `surface` | `0xFFFFFFFF` | Card, white |
| `surfaceDim` | `0xFFE7ECF6` | Recessed, light grey |
| `primary` | `0xFF0E7C97` | Accent, darkened cyan |
| `primaryDark` | `0xFF0A5E73` | Pressed / disabled accent |
| `onPrimary` | `0xFFFFFFFF` | Text on primary |
| `onSurface` | `0xFF0C1430` | Body text, deep ink |
| `muted` | `0xFF5A6680` | Secondary text, slate |
| `outline` | `0x330E7C97` | Borders, darkened cyan at ~20% alpha |
| `success` | `0xFF1B873F` | Status |
| `error` | `0xFFC0392B` | Status |
| `warning` | `0xFFB7791F` | Status |
| `pulse` | `0xFFC2417F` | Donation accent, AA-darkened pink |
| `glyph` | `0xFF2F7E96` | Brand-glyph tint, darkened cyan |
| `disabledFg` | `0xFF8A93A6` | Disabled-control foreground |
| `mutedStrong` | `0xFF4B566E` | Drawn-but-unavailable information |

## The four roles that get confused

| Role | What it is for | Opacity rule |
|---|---|---|
| `muted` | Ordinary secondary text: sub-lines, captions. | Full. |
| `mutedStrong` | Information *about* something unavailable: a capability a layer refuses, and the reason. | Full, always. An unavailable capability is drawn, never faded. |
| `disabledFg` | The foreground of an inactive **control**. | Paired with `Tokens.disabledOpacity` (0.55), and legal only on an `AbstractButton`. |
| `glyph` | The brand-glyph tint. | Full. Re-tinted by palette, never by state; a separate coloured dot carries state. |

`glyph` exists because the shipped SVGs bake the dark tint, which computes to
1.7:1 on a white card, below the 3:1 floor for graphical objects and visually
just gone. `BrandGlyph` renders the raw image on dark and routes through a
colourisation effect on light.

`disabledFg` and `mutedStrong` are two roles because the old single rule (drop
the whole control to 0.4 opacity) conflated "you cannot press this yet" with
"this hardware can never do this", and landed both near 2.6:1.

## Derived washes (QML `Theme` singleton only)

These are not palette entries. `ThemeBridge` derives them from the **active**
accent at read time, so a palette swap re-tints them with no second table to
forget. Alphas are 0 to 255.

| Name | Derivation |
|---|---|
| `primaryHover` | `primary` at 31 dark / 26 light (12%) |
| `primaryPress` | `primary` at 46 dark / 41 light (18%) |
| `accentWash24` | `primary` at 61 dark / 56 light (24%) |
| `primaryFill` | `primary` at 36 dark / 31 light |
| `successFill` | `success` at 36 dark / 31 light |
| `warningFill` | `warning` at 41 |
| `errorFill` | `error` at 41 |
| `focusRing` | `primary` at 77 (30%) |
| `outlineSubtle` | `primary` at 23 (9%) |
| `scrim` | `background` at 153 (60%) |
| `pulseFill` / `pulseEdge` | `pulse` at 31 / 89 |

`primaryHover` and `primaryPress` **are** the 12% and 18% accent washes. There
is deliberately no second name for either: one colour, one name.
`Theme.alpha(c, a)` re-alphas any token for the rare one-off.

## Theme modes

`dish::source::ThemePreferenceStore` holds the mode under
`user_preferences/theme_mode`, with storage values `"system"`, `"light"`, and
`"dark"` (kept verbatim from dish-android so the two apps share one schema).

Dark and Light pin the corresponding palette. System follows the desktop:
`dish::ui::detectSystemAppearance()` reads `QStyleHints::colorScheme()`, which
Qt resolves from the XDG `org.freedesktop.appearance` portal. A desktop that
exposes no portal reports Unknown and Dish stays dark.

The split keeps the persisted value and the rendered palette from drifting: the
store derives the mode, and `dish::composer::ThemeController` subscribes to it,
resolves System, and sets the active palette.

System follows the desktop live. `AppViewModel` subscribes to
`QGuiApplication::styleHints()`'s `colorSchemeChanged`; while the stored mode is
System it re-resolves, re-applies the palette, and fires the theme-applied sink,
so an OS theme flip re-themes a running app. An explicit Light or Dark pick
ignores the OS.

## How to use them

There is no stylesheet layer. C++ reads the tokens directly; QML reads them
through the `Theme` singleton (`ThemeBridge`), which re-emits `paletteChanged`
on a mode switch so every binding re-evaluates.

```cpp
// C++ (chrome / native window attributes)
const bool dark = dish::ui::activeAppearance() == dish::ui::Appearance::Dark;
```

```qml
// QML: never a hex literal outside src/qml/kit/**
Rectangle { color: Theme.surface; border.color: Theme.outline }
```

`dish::ui::hex(QRgb)` remains for diagnostics and log lines.

## Enforcement

- `tests/test_theme_store.cpp`: palette completeness. Every dark role has a
  light value, and they differ.
- `tests/test_theme_contrast.cpp`: WCAG 2.1 ratios over the real palette values
  in both palettes. A token that reads on dark and vanishes on light fails the
  build.
- `scripts/qml-lint-literals.sh`: no raw `#rrggbb`, `Qt.rgba(...)`,
  `font.family: "..."`, or bare numeric `font.pixelSize`, `radius`, or spacing
  outside `src/qml/kit/**`. Errors for `src/qml/wizard/**` and
  `src/qml/shared/**`, warnings elsewhere. Runs in CI.

A flush layout is `Tokens.s0`, not `0`. A zero gap is a decision the design
makes (a seam drawn with a divider or a colour change instead of a space), and
naming it keeps the scale continuous and the scanner quiet.
