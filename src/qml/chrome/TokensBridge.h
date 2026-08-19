// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Non-color design tokens (type scale, spacing, radii, shell metrics) as a
// `Tokens` singleton beside the color `Theme` one. Registered by instance in
// QmlEntryPoint: the declarative QML_SINGLETON registration is stripped under
// this target's LTCG, same as ThemeBridge.

#pragma once

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

namespace dish::chrome {

class TokensBridge : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Tokens)
    QML_SINGLETON

    // ── Type scale (px) ──────────────────────────────────────────────────────
    Q_PROPERTY(int textDisplay READ textDisplay CONSTANT)
    Q_PROPERTY(int textHero READ textHero CONSTANT)
    Q_PROPERTY(int textStatus READ textStatus CONSTANT)
    Q_PROPERTY(int textTitle READ textTitle CONSTANT)
    Q_PROPERTY(int textHeading READ textHeading CONSTANT)
    Q_PROPERTY(int textBase READ textBase CONSTANT)
    Q_PROPERTY(int textSummary READ textSummary CONSTANT)
    Q_PROPERTY(int textMeta READ textMeta CONSTANT)
    Q_PROPERTY(int textChip READ textChip CONSTANT)
    Q_PROPERTY(qreal sectionLetterSpacing READ sectionLetterSpacing CONSTANT)
    Q_PROPERTY(QString monoFamily READ monoFamily CONSTANT)
    Q_PROPERTY(QString sansFamily READ sansFamily CONSTANT)

    // ── Spacing scale (px) ───────────────────────────────────────────────────
    // s0 is FLUSH, not "unset": a seam the design draws with a divider or a
    // colour change instead of a gap. Named so those sites read as a decision.
    Q_PROPERTY(int s0 READ s0 CONSTANT)
    Q_PROPERTY(int s1 READ s1 CONSTANT)
    Q_PROPERTY(int s2 READ s2 CONSTANT)
    Q_PROPERTY(int s3 READ s3 CONSTANT)
    Q_PROPERTY(int s4 READ s4 CONSTANT)
    Q_PROPERTY(int s5 READ s5 CONSTANT)
    Q_PROPERTY(int s6 READ s6 CONSTANT)
    Q_PROPERTY(int s7 READ s7 CONSTANT)
    Q_PROPERTY(int s8 READ s8 CONSTANT)
    Q_PROPERTY(int s9 READ s9 CONSTANT)
    Q_PROPERTY(int s10 READ s10 CONSTANT)
    Q_PROPERTY(int s11 READ s11 CONSTANT)
    Q_PROPERTY(int pagePadding READ pagePadding CONSTANT)

    // ── Radii (px) ───────────────────────────────────────────────────────────
    Q_PROPERTY(int radiusChip READ radiusChip CONSTANT)
    Q_PROPERTY(int radiusButton READ radiusButton CONSTANT)
    Q_PROPERTY(int radiusCard READ radiusCard CONSTANT)
    Q_PROPERTY(int radiusBar READ radiusBar CONSTANT)
    Q_PROPERTY(int radiusDialog READ radiusDialog CONSTANT)

    // ── Glyph sizes (px) ─────────────────────────────────────────────────────
    Q_PROPERTY(int glyphSm READ glyphSm CONSTANT)
    Q_PROPERTY(int glyphMd READ glyphMd CONSTANT)
    Q_PROPERTY(int glyphLg READ glyphLg CONSTANT)
    Q_PROPERTY(int glyphXl READ glyphXl CONSTANT)
    Q_PROPERTY(int glyphHero READ glyphHero CONSTANT)

    // ── Motion durations (ms) ────────────────────────────────────────────────
    Q_PROPERTY(int durFast READ durFast CONSTANT)
    Q_PROPERTY(int durNormal READ durNormal CONSTANT)
    Q_PROPERTY(int durBusy READ durBusy CONSTANT)
    Q_PROPERTY(int durToast READ durToast CONSTANT)

    // ── Shell metrics ────────────────────────────────────────────────────────
    Q_PROPERTY(int titleBarHeight READ titleBarHeight CONSTANT)
    Q_PROPERTY(int captionButtonWidth READ captionButtonWidth CONSTANT)
    Q_PROPERTY(int railCompact READ railCompact CONSTANT)
    Q_PROPERTY(int railExpanded READ railExpanded CONSTANT)
    Q_PROPERTY(int navItemHeight READ navItemHeight CONSTANT)
    Q_PROPERTY(int hitRow READ hitRow CONSTANT)
    Q_PROPERTY(int minTouch READ minTouch CONSTANT)
    Q_PROPERTY(int dotSize READ dotSize CONSTANT)

    // ── Responsive breakpoints (CONTENT width, px) ───────────────────────────
    Q_PROPERTY(int minWindowWidth READ minWindowWidth CONSTANT)
    Q_PROPERTY(int minWindowHeight READ minWindowHeight CONSTANT)
    Q_PROPERTY(int narrowBreakpoint READ narrowBreakpoint CONSTANT)
    Q_PROPERTY(int stackBreakpoint READ stackBreakpoint CONSTANT)
    Q_PROPERTY(int wideBreakpoint READ wideBreakpoint CONSTANT)

    // Only legal on an AbstractButton, with enabled:false and Theme.disabledFg.
    // An unavailable capability keeps full opacity and uses Theme.mutedStrong.
    Q_PROPERTY(qreal disabledOpacity READ disabledOpacity CONSTANT)

    // The OS "animate controls inside windows" preference, inverted. Sampled in
    // the ctor, re-sampled by refreshMotionPreference().
    Q_PROPERTY(bool reducedMotion READ reducedMotion NOTIFY reducedMotionChanged)

  public:
    explicit TokensBridge(QObject* parent = nullptr);

    int textDisplay() const { return 26; }
    int textHero() const { return 21; }
    int textStatus() const { return 17; }
    int textTitle() const { return 20; }
    int textHeading() const { return 16; }
    int textBase() const { return 13; }
    int textSummary() const { return 12; }
    int textMeta() const { return 11; }
    int textChip() const { return 10; }
    qreal sectionLetterSpacing() const { return 1.5; }
    QString monoFamily() const;
    QString sansFamily() const;

    int s0() const { return 0; }
    int s1() const { return 2; }
    int s2() const { return 4; }
    int s3() const { return 6; }
    int s4() const { return 8; }
    int s5() const { return 10; }
    int s6() const { return 12; }
    int s7() const { return 14; }
    int s8() const { return 16; }
    int s9() const { return 20; }
    int s10() const { return 24; }
    int s11() const { return 32; }
    int pagePadding() const { return 24; }

    int radiusChip() const { return 5; }
    int radiusButton() const { return 6; }
    int radiusCard() const { return 8; }
    int radiusBar() const { return 2; }
    int radiusDialog() const { return 10; }

    int glyphSm() const { return 16; }
    int glyphMd() const { return 20; }
    int glyphLg() const { return 28; }
    int glyphXl() const { return 40; }
    int glyphHero() const { return 76; }

    int durFast() const { return 120; }
    int durNormal() const { return 200; }
    int durBusy() const { return 1100; }
    int durToast() const { return 4000; }

    int titleBarHeight() const { return 44; }
    int captionButtonWidth() const { return 46; }
    int railCompact() const { return 48; }
    int railExpanded() const { return 236; }
    int navItemHeight() const { return 40; }
    int hitRow() const { return 44; }
    int minTouch() const { return 32; }
    int dotSize() const { return 8; }

    int minWindowWidth() const { return 900; }
    int minWindowHeight() const { return 620; }
    int narrowBreakpoint() const { return 860; }
    int stackBreakpoint() const { return 760; }
    int wideBreakpoint() const { return 980; }

    qreal disabledOpacity() const { return 0.55; }

    bool reducedMotion() const { return reducedMotion_; }
    // Called on window activation: neither probe carries a change signal a
    // Quick app can bind to.
    Q_INVOKABLE void refreshMotionPreference();

  signals:
    void reducedMotionChanged();

  private:
    bool reducedMotion_ = false;
};

} // namespace dish::chrome
