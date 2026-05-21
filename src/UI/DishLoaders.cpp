// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "DishLoaders.h"

#include "Theme.h"

#include <QColor>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStyleOptionButton>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace dish::ui {

namespace {

// 16 ms ≈ 60 Hz. The animations are linear/triangle interpolations off
// QElapsedTimer rather than tied to the timer cadence, so a missed tick just
// skips a frame instead of drifting the animation phase.
constexpr int kFrameIntervalMs = 16;

constexpr double kSpinnerPeriodMs = 1200.0;
constexpr double kDotsPeriodMs = 1200.0;
constexpr double kDotStaggerSec = 0.18;
constexpr double kBarPeriodMs = 1400.0;

// Spec: 64-unit canvas, 6-unit stroke, dasharray "50 88".
constexpr double kSpinnerStrokeRatio = 6.0 / 64.0;
constexpr double kSpinnerArcFraction = 50.0 / (50.0 + 88.0);
constexpr double kSpinnerBgAlpha = 0.25;

// Bar geometry (spec is a 240x16 viewbox).
constexpr int kBarDesignWidth = 240;
constexpr int kBarDesignHeight = 16;
constexpr int kBarTrackHeight = 8;
constexpr int kBarSliderWidth = 80;
constexpr double kBarTrackAlpha = 0.22;

QColor primaryColor() { return QColor::fromRgb(Theme::primary); }

} // namespace

// ─── Spinner ─────────────────────────────────────────────────────────────

DishSpinnerWidget::DishSpinnerWidget(int diameter, QWidget* parent)
    : QWidget(parent), diameter_(diameter), timer_(new QTimer(this)) {
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(diameter_, diameter_);
    setAccessibleName(tr("Loading"));
    timer_->setInterval(kFrameIntervalMs);
    QObject::connect(timer_, &QTimer::timeout, this, QOverload<>::of(&DishSpinnerWidget::update));
}

void DishSpinnerWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    clock_.start();
    timer_->start();
}

void DishSpinnerWidget::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void DishSpinnerWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const double stroke = static_cast<double>(diameter_) * kSpinnerStrokeRatio;
    // Inset by half the stroke width so the ring sits inside the widget bounds
    // — otherwise the outer edge would be clipped.
    const double inset = stroke / 2.0;
    const QRectF ringRect(inset, inset, diameter_ - stroke, diameter_ - stroke);

    QColor bg = primaryColor();
    bg.setAlphaF(static_cast<float>(kSpinnerBgAlpha));
    QPen bgPen(bg, stroke);
    bgPen.setCapStyle(Qt::FlatCap);
    p.setPen(bgPen);
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(ringRect);

    // Phase ∈ [0, 1). Use QElapsedTimer rather than a frame counter so the
    // 1.2 s cycle is wall-clock accurate even on slow machines.
    const double elapsed = clock_.isValid() ? static_cast<double>(clock_.elapsed()) : 0.0;
    const double phase = std::fmod(elapsed, kSpinnerPeriodMs) / kSpinnerPeriodMs;
    // QPainter::drawArc uses 1/16 degree units. Spec rotates the arc start
    // from 0° to 360° over the cycle. QPainter measures angles CCW from 3
    // o'clock and uses positive spans — match SVG's CW rotation by negating
    // the span direction and starting at 12 o'clock (90°).
    const int startAngle = static_cast<int>(std::lround((90.0 - phase * 360.0) * 16.0));
    const int spanAngle = static_cast<int>(std::lround(-kSpinnerArcFraction * 360.0 * 16.0));

    QPen arcPen(primaryColor(), stroke);
    arcPen.setCapStyle(Qt::RoundCap);
    p.setPen(arcPen);
    p.drawArc(ringRect, startAngle, spanAngle);
}

// ─── Dots ────────────────────────────────────────────────────────────────

DishDotsWidget::DishDotsWidget(int size, QWidget* parent)
    : QWidget(parent), size_(size), timer_(new QTimer(this)) {
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(size_, size_);
    setAccessibleName(tr("Working"));
    timer_->setInterval(kFrameIntervalMs);
    QObject::connect(timer_, &QTimer::timeout, this, QOverload<>::of(&DishDotsWidget::update));
}

void DishDotsWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    clock_.start();
    timer_->start();
}

void DishDotsWidget::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void DishDotsWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    // Design-unit → widget-pixel scale (the spec uses a 64u canvas).
    const double scale = static_cast<double>(size_) / 64.0;
    const double elapsedSec =
        clock_.isValid() ? static_cast<double>(clock_.elapsed()) / 1000.0 : 0.0;
    const double cycleSec = kDotsPeriodMs / 1000.0;
    // Three dots at design-x 16/32/48.
    const double designCxs[3] = {16.0, 32.0, 48.0};

    for (int i = 0; i < 3; ++i) {
        // Triangle wave: 0 → 1 → 0 over the cycle, with a per-dot stagger.
        const double t =
            std::fmod(elapsedSec + static_cast<double>(i) * kDotStaggerSec, cycleSec) / cycleSec;
        const double tri = 1.0 - std::fabs(t - 0.5) * 2.0;
        const double opacity = 0.25 + 0.75 * tri;
        const double r = scale * (4.0 + 2.0 * tri); // design 4 → 6
        const double cx = designCxs[i] * scale;
        const double cy = 32.0 * scale;

        QColor c = primaryColor();
        c.setAlphaF(static_cast<float>(opacity));
        p.setBrush(c);
        p.drawEllipse(QPointF(cx, cy), r, r);
    }
}

// ─── Bar ─────────────────────────────────────────────────────────────────

DishBarWidget::DishBarWidget(int width, QWidget* parent)
    : QWidget(parent), width_(width),
      // The spec viewbox is 240x16; preserve that aspect ratio.
      height_(std::max(1, static_cast<int>(std::lround(
                              static_cast<double>(width) *
                              (static_cast<double>(kBarDesignHeight) / kBarDesignWidth))))),
      timer_(new QTimer(this)) {
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedSize(width_, height_);
    setAccessibleName(tr("Loading"));
    timer_->setInterval(kFrameIntervalMs);
    QObject::connect(timer_, &QTimer::timeout, this, QOverload<>::of(&DishBarWidget::update));
}

void DishBarWidget::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    clock_.start();
    timer_->start();
}

void DishBarWidget::hideEvent(QHideEvent* event) {
    timer_->stop();
    QWidget::hideEvent(event);
}

void DishBarWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    // Scale design units → widget pixels along the width axis. We also scale
    // heights so the proportions stay 240:16:8:80 even at non-default widths.
    const double scaleX = static_cast<double>(width_) / kBarDesignWidth;
    const double trackHeightPx = kBarTrackHeight * scaleX;
    const double sliderWidthPx = kBarSliderWidth * scaleX;
    const double radius = trackHeightPx / 2.0;
    const double trackY = (height_ - trackHeightPx) / 2.0;

    // Background track at 22 % alpha.
    QColor trackC = primaryColor();
    trackC.setAlphaF(static_cast<float>(kBarTrackAlpha));
    p.setBrush(trackC);
    p.drawRoundedRect(QRectF(0, trackY, width_, trackHeightPx), radius, radius);

    // Slider: x ∈ [-sliderWidth, width] over the cycle so the highlight
    // enters and exits cleanly through both edges. 1.4 s linear loop.
    const double elapsed = clock_.isValid() ? static_cast<double>(clock_.elapsed()) : 0.0;
    const double phase = std::fmod(elapsed, kBarPeriodMs) / kBarPeriodMs;
    const double sliderX = -sliderWidthPx + (width_ + sliderWidthPx) * phase;

    // Clip the slider to the track so the rounded edges line up cleanly when
    // the highlight pokes off the side.
    p.save();
    QPainterPath clip;
    clip.addRoundedRect(QRectF(0, trackY, width_, trackHeightPx), radius, radius);
    p.setClipPath(clip);
    p.setBrush(primaryColor());
    p.drawRoundedRect(QRectF(sliderX, trackY, sliderWidthPx, trackHeightPx), radius, radius);
    p.restore();
}

// ─── In-flight button ────────────────────────────────────────────────────

DishInFlightButton::DishInFlightButton(const QString& initialLabel, QWidget* parent,
                                       int spinnerSize)
    : QPushButton(parent) {
    // The QPushButton's own text stays empty so the style doesn't auto-draw
    // a label underneath our QLabel. The visible label sits in the layout.
    auto* lay = new QHBoxLayout(this);
    // QSS sets `padding: 6 12` on QPushButton, which insets the contentsRect
    // the layout fills; zero contentsMargins here avoids doubling the inset.
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    spinner_ = new DishSpinnerWidget(spinnerSize, this);
    spinner_->setVisible(false);
    label_ = new QLabel(initialLabel, this);
    // The label is purely visual — let clicks reach the button beneath it
    // by way of the standard QPushButton::mousePressEvent.
    label_->setAttribute(Qt::WA_TransparentForMouseEvents);
    label_->setAlignment(Qt::AlignCenter);
    lay->addStretch(1);
    lay->addWidget(spinner_, 0, Qt::AlignVCenter);
    lay->addWidget(label_, 0, Qt::AlignVCenter);
    lay->addStretch(1);
}

void DishInFlightButton::setInFlight(bool inFlight, const QString& busyLabel,
                                     const QString& restingLabel) {
    spinner_->setVisible(inFlight);
    label_->setText(inFlight ? busyLabel : restingLabel);
}

QSize DishInFlightButton::sizeHint() const {
    // QPushButton::sizeHint is computed from text() + icon() and ignores
    // child layouts, so we'd end up with a button too narrow for our
    // spinner+label content. Use the layout's preferred size as the hint,
    // then route through the style so QSS padding + frame are accounted for.
    QSize content = layout() != nullptr ? layout()->totalSizeHint() : QSize(0, 0);
    QStyleOptionButton opt;
    initStyleOption(&opt);
    return style()->sizeFromContents(QStyle::CT_PushButton, &opt, content, this);
}

} // namespace dish::ui
