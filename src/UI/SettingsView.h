// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.

#pragma once

#include <QWidget>

class QComboBox;

namespace dish {
class FeatureSettings;
}

namespace dish::ui {

// Feature-forwarding preferences surface. A self-contained QWidget so the two
// desktop shells can host it however they like — dish-windows wraps it in a
// modal QDialog, dish-linux adds it as a QStackedWidget page — while the
// settings controls themselves stay one shared implementation.
//
// Today this is just the "Light bar" control (Follow game / Off), matching
// dish-mac's SettingsView lightbar row. Future feature toggles slot into the
// same column.
class SettingsView : public QWidget {
    Q_OBJECT
  public:
    // `settings` is owned by the AppModel and outlives this view.
    explicit SettingsView(FeatureSettings* settings, QWidget* parent = nullptr);

  signals:
    // Emitted when the user dismisses the view (the Done button). The host
    // shell closes the dialog / pops the page.
    void closeRequested();

  private:
    // Push the combo selection back into FeatureSettings.
    void onLightbarModeChanged(int index);

    FeatureSettings* settings_;
    QComboBox* lightbarCombo_;
};

} // namespace dish::ui
