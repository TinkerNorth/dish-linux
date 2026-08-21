// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The Settings destination: one scrolling column that reflows to two above
// Tokens.wideBreakpoint. The page renders no local title — the shell header
// shows `headerTitle`.

// Bound: nested handlers/delegates reference the outer `settingsPage` id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../kit" as Kit
import Dish.Chrome

Kit.Page {
    id: settingsPage
    title: qsTr("Settings")

    readonly property string headerTitle: qsTr("Settings")

    // Order is load-bearing: an option's index IS its App.themeMode value.
    readonly property var themeOptions: [qsTr("Light"), qsTr("Dark"), qsTr("System")]
    readonly property string lightbarOn: qsTr("Follow game")
    readonly property string lightbarOff: qsTr("Off")

    // ── Updates ──────────────────────────────────────────────────────────────
    // One sentence per phase, assembled here rather than in C++ so every word
    // stays in the qsTr catalogues.
    readonly property string updateStatusText: {
        switch (App.updatePhase) {
        case "disabled":
            return qsTr("Automatic update checks are off.");
        case "checking":
            return qsTr("Checking for updates…");
        case "available":
            return qsTr("Dish %1 is available.").arg(App.updateVersion);
        case "failed":
            return settingsPage.updateFailureText;
        case "upToDate":
            return qsTr("You're on the latest version.");
        default:
            return qsTr("No update information yet.");
        }
    }

    // A diagnosis plus what happens next, never a bare error. Every arm retries
    // automatically, because a check is the only thing that can fail.
    readonly property string updateFailureText: {
        switch (App.updateErrorToken) {
        case "offline":
            return qsTr("You're offline. Dish will retry automatically.");
        case "manifestInvalid":
            return qsTr("The release information didn't parse. Dish will retry automatically.");
        default:
            return qsTr("The update check failed. Dish will retry automatically.");
        }
    }

    readonly property string lastCheckText: {
        const checked = App.updateLastCheck;
        if (!checked || isNaN(checked.getTime()))
            return qsTr("never");
        return Qt.formatDateTime(checked, Locale.ShortFormat);
    }

    // var-typed: `shellApi` is a dynamic property, resolved at runtime.
    readonly property var shellView: StackView.view
    readonly property var shellApi: settingsPage.shellView ? settingsPage.shellView.shellApi : null

    // Resolve the URL HERE: the shell's own Qt.resolvedUrl resolves against
    // AppShell.qml, which would miss ../onboarding/.
    function pushDetail(file, title) {
        if (!settingsPage.shellView) {
            return;
        }
        if (settingsPage.shellApi) {
            settingsPage.shellApi.pushDetail(Qt.resolvedUrl(file), title);
        } else {
            settingsPage.shellView.push(Qt.resolvedUrl(file));
        }
    }

    GridLayout {
        width: parent ? parent.width : implicitWidth
        columns: settingsPage.width < Tokens.wideBreakpoint ? 1 : 2
        columnSpacing: Tokens.pagePadding
        rowSpacing: Tokens.pagePadding

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            spacing: Tokens.s8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Tokens.s4

                Kit.SectionHeader { label: qsTr("Setup & help") }

                Kit.RowButton {
                    Layout.fillWidth: true
                    title: qsTr("Set up Dish")
                    subtitle: qsTr("Walk through input, destination and binding. Re-run any time.")
                    onClicked: if (settingsPage.shellApi) settingsPage.shellApi.openSetupWizard("")
                }
                Kit.RowButton {
                    Layout.fillWidth: true
                    title: qsTr("Help & FAQ")
                    subtitle: qsTr("Concepts, performance tips, and troubleshooting.")
                    onClicked: settingsPage.pushDetail("../onboarding/HelpScreen.qml",
                                                       qsTr("Help & FAQ"))
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Tokens.s4

                Kit.SectionHeader { label: qsTr("Appearance") }

                Kit.Card {
                    Layout.fillWidth: true
                    contentItem: ColumnLayout {
                        // Flush: the rows carry their own top margins, so a
                        // layout gap would double-space the label stack.
                        spacing: Tokens.s0

                        Label {
                            text: qsTr("Theme")
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textBase
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            Layout.topMargin: Tokens.s1
                            text: qsTr("Choose how Dish looks. System matches your desktop’s light or dark setting.")
                            color: Theme.muted
                            font.pixelSize: Tokens.textMeta
                            wrapMode: Text.WordWrap
                        }
                        Kit.SegmentedControl {
                            Layout.topMargin: Tokens.s5
                            options: settingsPage.themeOptions
                            value: settingsPage.themeOptions[App.themeMode]
                            onPicked: (option) => App.setThemeMode(settingsPage.themeOptions.indexOf(option))
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Tokens.s4

                Kit.SectionHeader { label: qsTr("Forwarded features") }

                Kit.Card {
                    Layout.fillWidth: true
                    contentItem: RowLayout {
                        spacing: Tokens.s6

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Tokens.s1

                            Label {
                                text: qsTr("Light bar")
                                color: Theme.onSurface
                                font.pixelSize: Tokens.textBase
                                font.weight: Font.DemiBold
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Follow game: the controller LED matches the host game. Off: leave the LED untouched.")
                                color: Theme.muted
                                font.pixelSize: Tokens.textMeta
                                wrapMode: Text.WordWrap
                            }
                        }

                        Kit.ComboButton {
                            Layout.alignment: Qt.AlignVCenter
                            options: [settingsPage.lightbarOn, settingsPage.lightbarOff]
                            value: App.lightbarFollowGame ? settingsPage.lightbarOn
                                                          : settingsPage.lightbarOff
                            onPicked: (option) => App.setLightbarFollowGame(option === settingsPage.lightbarOn)
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Features only apply when your controller’s hardware supports them — the controller list shows what was detected.")
                    color: Theme.muted
                    font.pixelSize: Tokens.textMeta
                    wrapMode: Text.WordWrap
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            Layout.alignment: Qt.AlignTop
            spacing: Tokens.s8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Tokens.s4

                Kit.SectionHeader { label: qsTr("Controller tuning") }

                Kit.RowButton {
                    Layout.fillWidth: true
                    title: qsTr("Dead zones & motion…")
                    subtitle: qsTr("Per-device stick / trigger flats and gyro forwarding.")
                    onClicked: settingsPage.pushDetail("DeadzoneSettingsPage.qml",
                                                       qsTr("Dead zones & motion"))
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Tokens.s4

                Kit.SectionHeader { label: qsTr("Window") }

                Kit.Card {
                    Layout.fillWidth: true
                    contentItem: Kit.LabeledSwitch {
                        label: qsTr("Keep running in the background")
                        description: App.trayAvailable
                            ? qsTr("Closing the window keeps controllers streaming. Quit from the tray icon.")
                            : qsTr("This desktop has no tray, so closing the window quits Dish.")
                        enabled: App.trayAvailable
                        checked: App.runInBackground
                        onToggled: (checked) => App.setRunInBackground(checked)
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Tokens.s4

                Kit.SectionHeader { label: qsTr("Diagnostics") }

                Kit.Card {
                    Layout.fillWidth: true
                    contentItem: Kit.LabeledSwitch {
                        // Nothing is transmitted, so the copy must not imply it is.
                        // This switch controls whether a crash is RECORDED locally.
                        label: qsTr("Save crash reports")
                        description: qsTr("Writes a crash report to this machine so you can read it and send it yourself. Nothing is uploaded.")
                        checked: App.crashReportingEnabled
                        onToggled: (checked) => App.setCrashReportingEnabled(checked)
                    }
                }

                // Only after a crash. In the ordinary case this card does not exist.
                Kit.Card {
                    Layout.fillWidth: true
                    visible: App.hasCrashReport
                    contentItem: ColumnLayout {
                        spacing: Tokens.s4

                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("Dish closed unexpectedly last time. The report below has had your home folder, network addresses and any key-like values removed.")
                            color: Tokens.textMeta
                        }

                        ScrollView {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 160
                            clip: true

                            TextArea {
                                readOnly: true
                                wrapMode: TextEdit.NoWrap
                                font.family: Tokens.monoFamily
                                text: App.crashReportText()
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Tokens.s4

                            Kit.OutlineButton {
                                text: qsTr("Copy")
                                onClicked: App.copyCrashReport()
                            }
                            Kit.OutlineButton {
                                text: qsTr("Report on GitHub")
                                onClicked: App.openCrashIssue()
                            }
                            Item { Layout.fillWidth: true }
                            Kit.OutlineButton {
                                text: qsTr("Dismiss")
                                onClicked: App.discardCrashReport()
                            }
                        }
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Tokens.s4

                Kit.SectionHeader { label: qsTr("Updates") }

                Kit.Card {
                    Layout.fillWidth: true
                    contentItem: ColumnLayout {
                        spacing: Tokens.s4

                        Label {
                            Layout.fillWidth: true
                            text: settingsPage.updateStatusText
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textBase
                            wrapMode: Text.WordWrap
                        }

                        // Absolute, never "3 hours ago": a relative time needs a
                        // counted string, and this line is not worth a plural
                        // form in six catalogues.
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Dish %1 · last checked %2").arg(App.appVersion)
                                  .arg(settingsPage.lastCheckText)
                            color: Theme.muted
                            font.family: Tokens.monoFamily
                            font.pixelSize: Tokens.textMeta
                            wrapMode: Text.WordWrap
                        }

                        Kit.Callout {
                            Layout.fillWidth: true
                            visible: App.updateRequired
                            tone: Kit.Callout.Warning
                            text: qsTr("This version is no longer supported. Please update.")
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Your package manager installs updates. Dish only tells you one exists.")
                            color: Theme.muted
                            font.pixelSize: Tokens.textMeta
                            wrapMode: Text.WordWrap
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Tokens.s4

                            Kit.OutlineButton {
                                size: Kit.DishButton.Small
                                text: qsTr("Check for updates")
                                enabled: App.updateChecksEnabled
                                         && App.updatePhase !== "checking"
                                onClicked: App.checkForUpdatesNow()
                            }
                            Kit.LoadingSpinner {
                                Layout.alignment: Qt.AlignVCenter
                                visible: App.updatePhase === "checking"
                                running: App.updatePhase === "checking"
                            }
                            Kit.KitButton {
                                visible: App.updatePhase === "available"
                                size: Kit.DishButton.Small
                                text: qsTr("Open release page")
                                onClicked: App.openReleaseNotes()
                            }
                            Item { Layout.fillWidth: true }
                        }
                    }
                }

                Kit.Card {
                    Layout.fillWidth: true
                    contentItem: ColumnLayout {
                        spacing: Tokens.s6

                        Kit.LabeledSwitch {
                            Layout.fillWidth: true
                            label: qsTr("Check for updates automatically")
                            description: qsTr("Dish asks GitHub for the newest release at startup and every few hours. Only a standard web request is sent; see the privacy policy for details.")
                            checked: App.updateChecksEnabled
                            onToggled: (checked) => App.setUpdateChecksEnabled(checked)
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Checking connects to github.com and sends no account data.")
                    color: Theme.muted
                    font.pixelSize: Tokens.textMeta
                    wrapMode: Text.WordWrap
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Tokens.s4

                Kit.SectionHeader { label: qsTr("About") }

                Kit.RowButton {
                    Layout.fillWidth: true
                    title: qsTr("Open source licenses")
                    subtitle: qsTr("Acknowledgements for the libraries Dish is built on.")
                    onClicked: settingsPage.pushDetail("LicensesPage.qml",
                                                       qsTr("Open source licenses"))
                }
                Kit.RowButton {
                    Layout.fillWidth: true
                    title: qsTr("Support Dish")
                    subtitle: qsTr("Donate via GitHub Sponsors, Ko-fi, or Buy Me a Coffee.")
                    onClicked: settingsPage.pushDetail("DonatePage.qml", qsTr("Support Dish"))
                }

                Label {
                    Layout.topMargin: Tokens.s1
                    text: qsTr("Dish %1 · TinkerNorth · LGPL-3.0").arg(App.appVersion)
                    color: Theme.muted
                    font.family: Tokens.monoFamily
                    font.pixelSize: Tokens.textMeta
                }
            }
        }
    }
}
