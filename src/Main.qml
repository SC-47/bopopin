import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 960
    height: 640
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: qsTr("Pinyin Practice")
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.background: theme.background
    Material.foreground: theme.foreground

    readonly property bool rightToLeft: Qt.application.layoutDirection === Qt.RightToLeft
    // Traditional Chinese glyphs, whatever language the desktop is in.
    readonly property string cjk: "Noto Sans CJK TC"

    // Bare letters are the practice itself, so every action is on another key.
    Shortcut { sequences: ["F1", "Ctrl+?"]; onActivated: help.opened ? help.close() : help.open() }
    Shortcut { sequences: ["Ctrl+Q", "Ctrl+W"]; onActivated: Qt.quit() }
    Shortcut { sequence: "Tab"; enabled: !help.visible; onActivated: backend.reveal() }
    Shortcut { sequence: "Ctrl+H"; onActivated: backend.hints = !backend.hints }
    Shortcut { sequence: "Escape"; enabled: !help.visible; onActivated: backend.skip() }
    Shortcut {
        sequence: "Ctrl+Right"
        onActivated: backend.lesson = (backend.lesson + 1) % backend.lessons.length
    }
    Shortcut {
        sequence: "Ctrl+Left"
        onActivated: backend.lesson = (backend.lesson + backend.lessons.length - 1) % backend.lessons.length
    }
    Shortcut { sequence: "Ctrl+R"; onActivated: backend.resetStats() }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        id: content
        anchors.fill: parent
        LayoutMirroring.enabled: win.rightToLeft
        LayoutMirroring.childrenInherit: true

        // A plain item takes no input method, so the letters arrive as typed
        // even while a Chinese input method is switched on.
        focus: true
        Keys.onPressed: (event) => {
            if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                return
            if (/^[a-zA-Z]$/.test(event.text)) {
                backend.type(event.text)
                event.accepted = true
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 28
            spacing: 0

            Flow {
                Layout.fillWidth: true
                spacing: 6

                Repeater {
                    model: backend.lessons
                    delegate: Rectangle {
                        id: chip
                        required property string modelData
                        required property int index
                        readonly property bool selected: index === backend.lesson

                        objectName: "lesson" + index
                        implicitWidth: chipLabel.implicitWidth + 16
                        implicitHeight: chipLabel.implicitHeight + 12
                        radius: Math.min(4, theme.radius)
                        color: selected ? theme.accent : "transparent"
                        border.color: selected ? theme.accent : theme.colors.selection

                        Label {
                            id: chipLabel
                            anchors.centerIn: parent
                            text: chip.modelData === "" ? qsTr("All")
                                : chip.modelData === "*" ? qsTr("Tricky")
                                : chip.modelData
                            color: chip.selected ? theme.accentForeground : theme.foreground
                            font.family: win.cjk
                            font.pixelSize: 14
                        }

                        TapHandler { onTapped: backend.lesson = chip.index }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Label {
                    anchors.centerIn: parent
                    visible: backend.wordCount === 0
                    text: qsTr("The word list could not be loaded.")
                    color: theme.colors.red
                    font.pixelSize: 18
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 28
                    visible: backend.wordCount > 0

                    // Pinyin reads left to right in every language.
                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 32
                        LayoutMirroring.enabled: false
                        LayoutMirroring.childrenInherit: true

                        Repeater {
                            model: backend.syllables
                            delegate: Column {
                                id: cell
                                required property var modelData
                                required property int index
                                readonly property bool current: index === backend.position
                                readonly property bool finished: index < backend.position
                                readonly property bool missed: backend.misses[index] === true
                                // A wrong key alone shows nothing: the pinyin appears only when asked for.
                                readonly property bool reveal: backend.hints || backend.revealed[index] === true
                                property bool flash: false

                                spacing: 4
                                transform: Translate { id: shift }

                                SequentialAnimation {
                                    id: shake
                                    PropertyAction { target: cell; property: "flash"; value: true }
                                    NumberAnimation { target: shift; property: "x"; to: -8; duration: 40 }
                                    NumberAnimation { target: shift; property: "x"; to: 8; duration: 80 }
                                    NumberAnimation { target: shift; property: "x"; to: 0; duration: 40 }
                                    PropertyAction { target: cell; property: "flash"; value: false }
                                }

                                Connections {
                                    target: backend
                                    function onWrongKey() {
                                        if (cell.current)
                                            shake.restart()
                                    }
                                }

                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: cell.modelData.zhuyin
                                    color: theme.colors.muted
                                    font.family: win.cjk
                                    font.pixelSize: 24
                                }

                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: cell.modelData.character
                                    color: cell.flash ? theme.colors.red
                                         : cell.finished ? theme.colors.muted
                                         : theme.foreground
                                    opacity: cell.current || cell.finished ? 1 : 0.55
                                    font.family: win.cjk
                                    font.pixelSize: 96
                                }

                                Rectangle {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: 96
                                    height: 3
                                    color: cell.current ? theme.accent : "transparent"
                                }

                                Item {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    width: Math.max(96, letters.width)
                                    height: 40

                                    Row {
                                        id: letters
                                        anchors.centerIn: parent

                                        Label {
                                            objectName: "typed" + cell.index
                                            text: cell.finished ? cell.modelData.pinyin
                                                : cell.current ? cell.modelData.pinyin.substring(0, backend.typed)
                                                : ""
                                            color: cell.missed ? theme.colors.orange : theme.colors.green
                                            font.pixelSize: 28
                                            font.bold: true
                                        }

                                        Label {
                                            objectName: "rest" + cell.index
                                            text: cell.finished || !cell.reveal ? ""
                                                : cell.current ? cell.modelData.pinyin.substring(backend.typed)
                                                : cell.modelData.pinyin
                                            color: theme.colors.muted
                                            font.pixelSize: 28
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // How the zhuyin of the character in turn becomes its pinyin.
                    Label {
                        id: breakdown
                        objectName: "breakdown"
                        readonly property var syllable: backend.syllables[backend.position]
                        readonly property bool shown: syllable !== undefined
                                                      && (backend.hints || backend.revealed[backend.position] === true)

                        anchors.horizontalCenter: parent.horizontalCenter
                        opacity: shown ? 1 : 0
                        text: {
                            if (syllable === undefined)
                                return " "
                            let pieces = syllable.parts.map(part => part.zhuyin + " → " + part.pinyin)
                            if (syllable.keys.indexOf("v") >= 0)
                                pieces.push(qsTr("(type v for ü)"))
                            return pieces.join("      ")
                        }
                        color: theme.colors.muted
                        font.family: win.cjk
                        font.pixelSize: 20
                    }
                }
            }

            Label {
                objectName: "weak"
                Layout.fillWidth: true
                Layout.bottomMargin: 8
                opacity: backend.weakest.length > 0 ? 1 : 0
                text: qsTr("Weak spots: %1").arg(backend.weakest.join("      "))
                color: theme.colors.muted
                font.family: win.cjk
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 24

                Label {
                    objectName: "done"
                    text: qsTr("Done %1").arg(backend.done)
                    font.family: win.cjk
                }
                Label {
                    objectName: "accuracy"
                    text: qsTr("Accuracy %1%").arg(backend.done > 0 ? Math.round(100 * backend.correct / backend.done) : 100)
                    font.family: win.cjk
                }
                Label {
                    objectName: "streak"
                    text: qsTr("Streak %1").arg(backend.streak)
                    font.family: win.cjk
                }
                Label {
                    objectName: "speed"
                    text: qsTr("%1 chars/min").arg(backend.speed > 0 ? backend.speed : "–")
                    font.family: win.cjk
                }

                Item { Layout.fillWidth: true }

                Label {
                    text: qsTr("Tab hint · Esc skip · F1 shortcuts")
                    color: theme.colors.muted
                    font.family: win.cjk
                }
            }
        }
    }

    Popup {
        id: help
        objectName: "help"
        anchors.centerIn: Overlay.overlay
        modal: true
        focus: true
        padding: 24
        onClosed: content.forceActiveFocus()
        background: Rectangle {
            color: theme.background
            border.color: theme.colors.selection
            radius: theme.radius
        }

        ColumnLayout {
            spacing: 8
            // A popup is not an Item and doesn't inherit the window's
            // mirroring, so its content opts in.
            LayoutMirroring.enabled: win.rightToLeft
            LayoutMirroring.childrenInherit: true

            Repeater {
                model: [
                    { keys: "A – Z", action: qsTr("Type the pinyin of the character in turn") },
                    { keys: "Tab", action: qsTr("Show the pinyin of this character") },
                    { keys: "Ctrl+H", action: qsTr("Always show the pinyin, or stop") },
                    { keys: "Esc", action: qsTr("Skip this word") },
                    { keys: "Ctrl+← →", action: qsTr("Previous or next lesson") },
                    { keys: "Ctrl+R", action: qsTr("Reset the score and the weak spots") },
                    { keys: "F1", action: qsTr("Show or hide the shortcuts") },
                    { keys: "Ctrl+Q", action: qsTr("Quit") }
                ]
                delegate: RowLayout {
                    required property var modelData
                    spacing: 24

                    Label {
                        Layout.preferredWidth: 96
                        text: modelData.keys
                        color: theme.accent
                        font.bold: true
                    }
                    Label {
                        text: modelData.action
                        font.family: win.cjk
                    }
                }
            }
        }
    }
}
