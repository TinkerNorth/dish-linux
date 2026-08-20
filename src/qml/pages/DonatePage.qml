// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// Support Dish — a rail destination (hero column plus the three donation rails).
//
// Pulse pink (Theme.pulse / pulseFill / pulseEdge) is the ONE hue Dish uses
// beyond cyan and is scoped to THIS surface and the rail heart — nowhere else.

// Bound: the rail delegates reference the outer `donatePage` id.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "../kit" as Kit
import Dish.Chrome

Kit.Page {
    id: donatePage
    title: qsTr("Support Dish")

    readonly property string headerTitle: qsTr("Support Dish")
    readonly property string headerSub: qsTr("Dish is free and open source — donations keep it that way")

    readonly property string urlSponsors: App.donateSponsorsUrl
    readonly property string urlKofi: App.donateKofiUrl
    readonly property string urlBmac: App.donateBmacUrl

    readonly property int heroWidth: 330

    readonly property var rails: [
        {
            name: qsTr("GitHub Sponsors"),
            recommended: true,
            blurb: qsTr("Monthly sponsorship, processed by GitHub. Lowest fees, no platform cut, "
                      + "and the most stable way to support Dish month after month."),
            cadence: qsTr("Recurring"),
            currencies: qsTr("Card, PayPal, GitHub credit"),
            url: donatePage.urlSponsors
        },
        {
            name: qsTr("Ko-fi"),
            recommended: false,
            blurb: qsTr("Tip jar. Buy us a coffee, no account required — Ko-fi takes no cut on "
                      + "one-time tips."),
            cadence: qsTr("One-time"),
            currencies: qsTr("Card, Apple Pay, Google Pay, PayPal"),
            url: donatePage.urlKofi
        },
        {
            name: qsTr("Buy Me a Coffee"),
            recommended: false,
            blurb: qsTr("Quick one-time gift or a monthly membership. Great if you don’t have a "
                      + "GitHub account."),
            cadence: qsTr("Either"),
            currencies: qsTr("Card, Apple Pay, Google Pay"),
            url: donatePage.urlBmac
        }
    ]

    // App.openExternalUrl, never Qt.openUrlExternally: a failed open must toast.
    function openUrl(url) {
        if (url && url.length > 0) {
            App.openExternalUrl(url);
        }
    }

    GridLayout {
        width: parent ? parent.width : implicitWidth
        // The hero column is FIXED width, so two columns only fit once the rail
        // column still keeps a readable measure.
        columns: donatePage.width < Tokens.stackBreakpoint ? 1 : 2
        columnSpacing: Tokens.s10
        rowSpacing: Tokens.s10

        ColumnLayout {
            Layout.preferredWidth: donatePage.heroWidth
            Layout.fillWidth: donatePage.width < Tokens.stackBreakpoint
            Layout.alignment: Qt.AlignTop
            spacing: Tokens.s5

            Rectangle {
                implicitWidth: Tokens.glyphXl + Tokens.s7
                implicitHeight: Tokens.glyphXl + Tokens.s7
                radius: width / 2
                color: Theme.pulseFill
                border.width: 1
                border.color: Theme.pulseEdge

                Label {
                    anchors.centerIn: parent
                    text: "♥" // brand glyph, not localized
                    color: Theme.pulse
                    font.pixelSize: Tokens.glyphLg
                }
            }

            Kit.Eyebrow {
                Layout.topMargin: Tokens.s2
                text: qsTr("Support the project")
                color: Theme.pulse
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Dish runs on coffee and goodwill.")
                color: Theme.onSurface
                font.pixelSize: Tokens.textHero
                font.bold: true
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Dish, Satellite, and every client are free, open source, ad-free, and "
                         + "analytics-free. No paywalled features, no upsells — donations are what "
                         + "keep them true.")
                color: Theme.muted
                font.pixelSize: Tokens.textSummary
                wrapMode: Text.WordWrap
            }

            // Hand-rolled rather than a Kit.DishButton: the kit's variants are
            // all accent-toned and pulse must not leak into them.
            AbstractButton {
                id: sponsorCta

                Layout.topMargin: Tokens.s3
                implicitHeight: Tokens.glyphLg + Tokens.s3
                implicitWidth: ctaRow.implicitWidth + Tokens.s11
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                opacity: enabled ? 1.0 : Tokens.disabledOpacity
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Sponsor on GitHub Sponsors")
                onClicked: donatePage.openUrl(donatePage.urlSponsors)

                HoverHandler { cursorShape: Qt.PointingHandCursor }

                background: Rectangle {
                    radius: Tokens.radiusButton
                    color: sponsorCta.down || sponsorCta.hovered
                           ? Qt.darker(Theme.pulse, 1.15) : Theme.pulse
                    border.width: sponsorCta.visualFocus ? 1 : 0
                    border.color: Theme.primary

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -Tokens.s1
                        visible: sponsorCta.visualFocus
                        radius: parent.radius + Tokens.s1
                        color: "transparent"
                        border.width: 2
                        border.color: Theme.focusRing
                    }
                }

                contentItem: RowLayout {
                    id: ctaRow
                    spacing: Tokens.s4

                    Label {
                        Layout.leftMargin: Tokens.s8
                        text: "♥" // brand glyph, not localized
                        color: Theme.onPrimary
                        font.pixelSize: Tokens.textBase
                    }
                    Label {
                        Layout.rightMargin: Tokens.s8
                        text: qsTr("Sponsor on GitHub Sponsors")
                        color: Theme.onPrimary
                        font.pixelSize: Tokens.textBase
                        font.weight: Font.DemiBold
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Recommended — lowest fees, matched by GitHub where eligible.")
                color: Theme.muted
                font.pixelSize: Tokens.textChip
                wrapMode: Text.WordWrap
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: Tokens.s8
                text: qsTr("Thank you. Every dollar, every star, every shared link. — Emir")
                color: Theme.muted
                font.pixelSize: Tokens.textMeta
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Tokens.s6

            Repeater {
                model: donatePage.rails

                delegate: ColumnLayout {
                    id: rail
                    required property var modelData

                    Layout.fillWidth: true
                    spacing: Tokens.s3

                    Kit.RowButton {
                        Layout.fillWidth: true
                        title: rail.modelData.name
                        subtitle: rail.modelData.blurb
                        onClicked: donatePage.openUrl(rail.modelData.url)
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: Tokens.s6
                        spacing: Tokens.s9

                        Kit.Eyebrow {
                            visible: rail.modelData.recommended === true
                            text: qsTr("Recommended")
                            color: Theme.pulse
                        }
                        ColumnLayout {
                            spacing: Tokens.s1
                            Kit.Eyebrow { mutedTone: true; text: qsTr("Cadence") }
                            Label {
                                text: rail.modelData.cadence
                                color: Theme.onSurface
                                font.pixelSize: Tokens.textMeta
                            }
                        }
                        ColumnLayout {
                            spacing: Tokens.s1
                            Kit.Eyebrow { mutedTone: true; text: qsTr("Pays with") }
                            Label {
                                text: rail.modelData.currencies
                                color: Theme.onSurface
                                font.pixelSize: Tokens.textMeta
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // Bold tags only: a Theme colour stringified into StyledText markup
            // yields #AARRGGBB, which parses as #RRGGBBAA — silently wrong.
            Kit.Card {
                Layout.fillWidth: true
                Layout.topMargin: Tokens.s2
                dense: true

                contentItem: ColumnLayout {
                    spacing: Tokens.s3

                    Label {
                        Layout.fillWidth: true
                        text: qsTr("What your donation pays for")
                        color: Theme.onSurface
                        font.pixelSize: Tokens.textBase
                        font.weight: Font.DemiBold
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.StyledText
                        text: qsTr("<b>Hosting</b> — dish.tinkernorth.com and every signed-installer "
                                 + "mirror. <b>Code signing</b> — the EV certificate that keeps "
                                 + "SmartScreen friendly, renewed yearly. <b>Store fees</b> — "
                                 + "listings and policy upkeep. <b>Time</b> — honest answer: most "
                                 + "of it.")
                        color: Theme.muted
                        font.pixelSize: Tokens.textMeta
                        lineHeight: 1.4
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
