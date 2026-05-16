// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#include "SettingsView.h"

#include "FeatureSettings.h"
#include "Theme.h"

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

namespace dish::ui {

SettingsView::SettingsView(FeatureSettings* settings, QWidget* parent)
    : QWidget(parent), settings_(settings) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);

    // Header: title + Done.
    auto* headerRow = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("Settings"), this);
    title->setStyleSheet(
        QStringLiteral("font-size: 17px; font-weight: 600; color: %1;").arg(hex(Theme::onSurface)));
    auto* doneButton = new QPushButton(QStringLiteral("Done"), this);
    QObject::connect(doneButton, &QPushButton::clicked, this, &SettingsView::closeRequested);
    headerRow->addWidget(title, 1, Qt::AlignVCenter);
    headerRow->addWidget(doneButton, 0, Qt::AlignVCenter);
    layout->addLayout(headerRow);

    auto* sectionHeader = new QLabel(QStringLiteral("FORWARDED FEATURES"), this);
    sectionHeader->setStyleSheet(sectionHeaderQss());
    layout->addWidget(sectionHeader);

    // Light-bar row — a card holding the title, an explanatory line and the
    // Follow game / Off picker. Mirrors dish-mac's SettingsView lightbarRow.
    auto* card = new QFrame(this);
    card->setObjectName(QStringLiteral("card"));
    auto* cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(14, 12, 14, 12);
    cardLayout->setSpacing(12);

    auto* textColumn = new QVBoxLayout;
    textColumn->setSpacing(2);
    auto* rowTitle = new QLabel(QStringLiteral("Light bar"), card);
    rowTitle->setStyleSheet(
        QStringLiteral("font-weight: 600; color: %1;").arg(hex(Theme::onSurface)));
    auto* rowDetail =
        new QLabel(QStringLiteral("Follow game: the controller LED matches the host game. "
                                  "Off: leave the LED untouched."),
                   card);
    rowDetail->setWordWrap(true);
    rowDetail->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(hex(Theme::muted)));
    textColumn->addWidget(rowTitle);
    textColumn->addWidget(rowDetail);

    lightbarCombo_ = new QComboBox(card);
    // Item data carries the LightbarMode so the index→mode mapping survives a
    // future reordering of the rows.
    lightbarCombo_->addItem(lightbarModeLabel(LightbarMode::FollowGame),
                            QVariant::fromValue(static_cast<int>(LightbarMode::FollowGame)));
    lightbarCombo_->addItem(lightbarModeLabel(LightbarMode::Off),
                            QVariant::fromValue(static_cast<int>(LightbarMode::Off)));
    const int current = (settings_->lightbarMode() == LightbarMode::Off) ? 1 : 0;
    lightbarCombo_->setCurrentIndex(current);
    QObject::connect(lightbarCombo_, &QComboBox::currentIndexChanged, this,
                     &SettingsView::onLightbarModeChanged);

    cardLayout->addLayout(textColumn, 1);
    cardLayout->addWidget(lightbarCombo_, 0, Qt::AlignVCenter);
    layout->addWidget(card);

    auto* footnote =
        new QLabel(QStringLiteral("Features only apply when your controller's hardware "
                                  "supports them — the controller list shows what was detected."),
                   this);
    footnote->setWordWrap(true);
    footnote->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;").arg(hex(Theme::muted)));
    layout->addWidget(footnote);

    layout->addStretch(1);
}

void SettingsView::onLightbarModeChanged(int index) {
    const QVariant data = lightbarCombo_->itemData(index);
    const auto mode = static_cast<LightbarMode>(data.toInt());
    settings_->setLightbarMode(mode);
}

} // namespace dish::ui
