// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Three indeterminate progress indicators ported from the design spec in
// `app-icon/project/app-essentials.jsx` (sections `LoaderSpinner`,
// `LoaderDots`, `LoaderBar`). Proportions (stroke width, dasharray ratio,
// timings) are pixel-faithful to the spec; the color is `Theme::primary`
// rather than the spec's hard-coded `#8FCFE3` — this app's brand intentionally
// runs on a different blue (see `Theme.h`), but every other geometric rule of
// the spec is preserved.
//
// All three widgets:
//   * Drive their animation off a single 16-ms QTimer per instance so the
//     painter just renders phase → geometry on each frame.
//   * Stop ticking when hidden (showEvent / hideEvent) so an off-screen
//     loader doesn't burn CPU.
//   * Are transparent — the parent's background shows through.
//
// The accompanying `DishInFlightButton` lets a QPushButton render a
// spinner + label combination inside itself, mirroring the dish-mac
// `HStack(DishSpinner, Text)` pattern in `DishOutlinedButtonStyle`.

#pragma once

#include <QElapsedTimer>
#include <QPushButton>
#include <QWidget>

class QLabel;
class QTimer;

namespace dish::ui {

// Indeterminate rotating arc. Spec reference: `LoaderSpinner`.
//
// 64x64 design canvas → stroke width is `size * 6/64`. Background ring sits at
// 25 % alpha; the visible arc is `50 / (50 + 88) = 50 / 138 ~= 36.2 %` of the
// circumference. 1.2 s linear rotation, indefinite.
class DishSpinnerWidget : public QWidget {
    Q_OBJECT
  public:
    explicit DishSpinnerWidget(int diameter = 16, QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(diameter_, diameter_); }
    QSize minimumSizeHint() const override { return QSize(diameter_, diameter_); }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    int diameter_;
    QTimer* timer_;
    QElapsedTimer clock_;
};

// Three pulsing circles. Spec reference: `LoaderDots`.
//
// Each dot oscillates in opacity (0.25 → 1) and radius (4 → 6 design units)
// on a 1.2 s cycle, staggered 0.18 s between dots. The original SMIL
// `<animate values="A;B;A" dur="1.2s">` is a linear interpolation that
// produces a triangle wave — mirrored here so the visual matches.
class DishDotsWidget : public QWidget {
    Q_OBJECT
  public:
    explicit DishDotsWidget(int size = 16, QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(size_, size_); }
    QSize minimumSizeHint() const override { return QSize(size_, size_); }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    int size_;
    QTimer* timer_;
    QElapsedTimer clock_;
};

// Indeterminate horizontal bar — an 80-unit-wide highlight slides across a
// 240-unit track on a 1.4 s linear cycle. Spec reference: `LoaderBar`.
//
// The slider starts off-screen at `x = -80` and ends off-screen at `x = 240`,
// so the bright pip animates fully through the visible area.
class DishBarWidget : public QWidget {
    Q_OBJECT
  public:
    explicit DishBarWidget(int width = 240, QWidget* parent = nullptr);

    QSize sizeHint() const override { return QSize(width_, height_); }
    QSize minimumSizeHint() const override { return QSize(width_, height_); }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

  private:
    int width_;
    int height_;
    QTimer* timer_;
    QElapsedTimer clock_;
};

// QPushButton that renders its content as a centred `[spinner] [label]`
// pair instead of QPushButton's auto-text. Used by ConnectionsPage / Scan +
// Connect and PairingPage / Pair so the in-flight UI (DishSpinner +
// "Scanning…", "Connecting…", "Pairing…") lives where the user clicked.
//
// QPushButton::sizeHint is computed from text() + icon() and ignores the
// child layout, which would otherwise leave a layout-only button too narrow
// for its children. We override sizeHint / minimumSizeHint to return the
// layout's preferred size, padded by the QStyle's standard push-button
// margins so the QSS `padding: 6 12` rule lines up.
class DishInFlightButton : public QPushButton {
    Q_OBJECT
  public:
    explicit DishInFlightButton(const QString& initialLabel, QWidget* parent = nullptr,
                                int spinnerSize = 12);

    // Toggle the in-flight visual: shows the spinner and swaps the label
    // text. The button itself is left enabled/disabled by the caller.
    void setInFlight(bool inFlight, const QString& busyLabel, const QString& restingLabel);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

  private:
    DishSpinnerWidget* spinner_;
    QLabel* label_;
};

} // namespace dish::ui
