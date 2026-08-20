// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Dish contributors.
//
// The ONE elevated surface in the app: the only thing that floats without a
// scrim, so the shadow is what separates it from the page. Dialogs have none.
//
// There is no `info` tone. Persistent state lives on the surface that owns it,
// so an informational toast is by construction state that has escaped its
// surface; a stray "info" maps to success and warns.

// Bound: the toast delegate reads the outer `host` id and its own required
// model roles.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtQuick.Layouts
import Dish.Chrome

Item {
    id: host

    property int durationMs: Tokens.durToast
    // DROP_OLDEST beyond this, so a flood of errors can't grow without bound.
    property int maxVisible: 4

    // The host itself must stay hit-transparent: only the pills take clicks.
    anchors.fill: parent

    property int _nextId: 1

    // { toastId, message, severity }. A ListModel rather than createObject so
    // each toast's lifetime is its row's lifetime.
    ListModel { id: toastModel }

    // `severity` defaults to "error" (the App.errorMessage channel).
    function show(message, severity) {
        if (!message || message.length === 0)
            return;
        let sev = severity;
        if (sev === "info") {
            console.warn("NotificationToastHost: the 'info' tone was removed — "
                         + "persistent state belongs on its own surface. Mapped to "
                         + "'success': " + message);
            sev = "success";
        }
        if (sev !== "warning" && sev !== "success")
            sev = "error";
        while (toastModel.count >= host.maxVisible)
            toastModel.remove(0);
        toastModel.append({ toastId: host._nextId++, message: message, severity: sev });
    }

    // By id, not index: the index shifts as earlier toasts dismiss.
    function _dismiss(toastId) {
        for (let i = 0; i < toastModel.count; ++i) {
            if (toastModel.get(i).toastId === toastId) {
                toastModel.remove(i);
                return;
            }
        }
    }

    Column {
        id: stack
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: Tokens.s8
        spacing: Tokens.s4

        Repeater {
            model: toastModel

            delegate: Item {
                id: toast
                required property int index
                required property int toastId
                required property string message
                required property string severity

                implicitWidth: pill.implicitWidth
                implicitHeight: pill.implicitHeight
                width: implicitWidth
                height: implicitHeight

                readonly property color accent: toast.severity === "success" ? Theme.success
                                              : toast.severity === "warning" ? Theme.warning
                                              : Theme.error

                Accessible.role: Accessible.AlertMessage
                Accessible.name: toast.message

                Rectangle {
                    id: pill
                    implicitWidth: Math.min(pillRow.implicitWidth + 28, 380)
                    implicitHeight: Math.max(pillRow.implicitHeight + 24, 40)
                    radius: Tokens.radiusButton
                    color: Theme.surface
                    border.width: 1
                    border.color: Theme.outline
                    layer.enabled: true
                    layer.effect: MultiEffect {
                        shadowEnabled: true
                        shadowBlur: 0.8
                        shadowVerticalOffset: 8
                        shadowColor: Qt.rgba(0, 0, 0, 0.45)
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 3
                        color: toast.accent
                    }

                    RowLayout {
                        id: pillRow
                        anchors.fill: parent
                        anchors.leftMargin: Tokens.s7
                        anchors.rightMargin: Tokens.s6
                        anchors.topMargin: Tokens.s6
                        anchors.bottomMargin: Tokens.s6
                        spacing: Tokens.s5

                        Label {
                            text: toast.message
                            color: Theme.onSurface
                            font.pixelSize: Tokens.textSummary
                            lineHeight: 1.45
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                            Layout.maximumWidth: 320
                            Layout.alignment: Qt.AlignVCenter
                        }

                        // Drawn, not typed: ✕ has no reliable glyph across UI faces.
                        Canvas {
                            id: dismissMark
                            implicitWidth: Tokens.s5
                            implicitHeight: Tokens.s5
                            Layout.alignment: Qt.AlignTop
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.reset();
                                // The whole pill is the dismiss target, so the
                                // mark brightens with the pill.
                                var c = pillMouse.containsMouse ? Theme.onSurface : Theme.muted;
                                ctx.strokeStyle = Qt.rgba(c.r, c.g, c.b, c.a);
                                ctx.lineWidth = 1.4;
                                ctx.beginPath();
                                ctx.moveTo(1.5, 1.5);
                                ctx.lineTo(width - 1.5, height - 1.5);
                                ctx.moveTo(width - 1.5, 1.5);
                                ctx.lineTo(1.5, height - 1.5);
                                ctx.stroke();
                            }
                            Connections {
                                target: Theme
                                function onPaletteChanged() { dismissMark.requestPaint(); }
                            }
                            Connections {
                                target: pillMouse
                                function onContainsMouseChanged() { dismissMark.requestPaint(); }
                            }
                        }
                    }

                    // Never in the tab order: it announces itself as an
                    // AlertMessage and leaves on its own, so taking focus would
                    // interrupt whatever the user is actually doing.
                    MouseArea {
                        id: pillMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: host._dismiss(toast.toastId)
                    }
                }

                // Appear motion only: removing the model row destroys the
                // delegate, so there is nothing left to animate out.
                opacity: 0
                transform: Translate { id: slide; y: 12 }

                NumberAnimation {
                    id: slideIn
                    target: slide
                    property: "y"
                    from: 12
                    to: 0
                    duration: Tokens.reducedMotion ? 0 : Tokens.durFast
                    easing.type: Easing.OutCubic
                }
                NumberAnimation {
                    id: fadeIn
                    target: toast
                    property: "opacity"
                    from: 0
                    to: 1
                    duration: Tokens.reducedMotion ? 0 : Tokens.durFast
                }

                Timer {
                    id: autoDismiss
                    interval: host.durationMs
                    repeat: false
                    onTriggered: host._dismiss(toast.toastId)
                }

                Component.onCompleted: {
                    slideIn.start();
                    fadeIn.start();
                    autoDismiss.start();
                }
            }
        }
    }
}
