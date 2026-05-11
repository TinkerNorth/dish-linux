# dish-linux — Design tokens

All theme values live in [src/UI/Theme.h](src/UI/Theme.h) and
[src/UI/Theme.cpp](src/UI/Theme.cpp).

Token names follow the cross-repo schema documented in
`d:\TinkerNorth\BRAND.md` (TinkerNorth design system). When updating a value,
keep it in sync with the matching token in dish-android, dish-mac, and the
Satellite local web UI.

## Available tokens

Colors (in `dish::ui::Theme`):

Palette: **cyan / deep-space** — mirrors dish-website.

| Token | Value | Role |
|---|---|---|
| `background` | `0xFF060818` | Body (`--tn-ink`) |
| `surface` | `0xFF0C1027` | Card (`--tn-night`) |
| `surfaceDim` | `0xFF131A3A` | Recessed (`--tn-deep`) |
| `primary` | `0xFF4FE3FF` | Main accent — cyan (`--tn-signal`) |
| `primaryDark` | `0xFF2C93AD` | Pressed / disabled (`--tn-signal-dim`) |
| `onPrimary` | `0xFF060818` | Text on primary |
| `onSurface` | `0xFFE6ECFF` | Body text (`--body-color`) |
| `muted` | `0xFF93A0C8` | Secondary text (`--muted`) |
| `outline` | `0x2E4FE3FF` | Borders — cyan @ ~18% alpha |
| `success` | `0xFF22C55E` | Status — success |
| `error` | `0xFFE74C3C` | Status — error |
| `warning` | `0xFFF59E0B` | Status — warning |

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
