# dish-linux — Design tokens

All theme values live in [src/UI/Theme.h](src/UI/Theme.h) and
[src/UI/Theme.cpp](src/UI/Theme.cpp).

Token names follow the cross-repo schema documented in
`d:\TinkerNorth\BRAND.md` (TinkerNorth design system). When updating a value,
keep it in sync with the matching token in dish-android, dish-mac, and the
Satellite local web UI.

## Available tokens

Colors (in `dish::ui::Theme`):

- `background` — body background
- `surface` — card / raised panel
- `surfaceDim` — recessed / empty state
- `primary` — main accent (amber)
- `primaryDark` — pressed / disabled primary
- `onPrimary` — text/icon on top of primary
- `onSurface` — body text on surface
- `muted` — secondary text
- `outline` — borders / dividers
- `success`, `error`, `warning` — status

## How to use

Inside QSS strings, embed via `hex()`:

```cpp
label->setStyleSheet(
    QStringLiteral("color: %1;").arg(hex(Theme::muted)));
```

Pre-built QSS snippets are also available:

- `sectionHeaderQss()` — monospace section labels
- `outlinedButtonQss()` — outlined button styling
- `dotQss(QRgb)` — small colored status dot

## Outliers

None as of this commit. All previously hardcoded color literals
(`#EAEAEA`, `#6B7280`) in [src/UI/SlotCard.cpp](src/UI/SlotCard.cpp) and
[src/UI/ConnectionsDialog.cpp](src/UI/ConnectionsDialog.cpp) have been
migrated to `Theme::onSurface` / `Theme::muted`.

The Qt palette / global stylesheet in [src/UI/Theme.cpp](src/UI/Theme.cpp)
contains rgba shadow / hover literals (`rgba(255,193,7,0.12)`,
`rgba(255,193,7,0.18)`); these are derived from `Theme::primary` but
expressed inline because Qt's QSS does not support variable references.
Consider these intentional — keep in sync by hand if `primary` changes.
