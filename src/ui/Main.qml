import QtQuick
import QtQuick.Controls
import App.Backend 1.0

ApplicationWindow {
    id: window
    width: 1024
    height: 600
    visible: true
    title: "AI秤"
    visibility: Window.FullScreen

    // 白底背景
    Rectangle {
        anchors.fill: parent
        color: "#FFFFFF"

        // 顶部标题
        Column {
            id: titleBar
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 40
            width: parent.width - 200
            spacing: 24

            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "切菜机重量实时监测"
                font.pixelSize: 48
                font.bold: true
                font.family: "PingFang SC"
                color: "#1A5FB4"
            }

            // 标题下分隔线
            Rectangle {
                width: parent.width
                height: 2
                color: "#BBD3F0"
            }
        }

        // 主体：左右对称布局
        Item {
            anchors.top: titleBar.bottom
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 40

            Row {
                anchors.fill: parent
                anchors.leftMargin: 120
                anchors.rightMargin: 120
                anchors.topMargin: 0
                anchors.bottomMargin: 80
                spacing: 104

                // ============ 左侧模块（去皮）============
                Column {
                    width: (parent.width - 104) / 2
                    height: parent.height
                    spacing: 24
                 
                    // 标签：左
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: "左"
                        font.pixelSize: 32
                        font.bold: true
                        font.family: "PingFang SC"
                        color: "#1A5FB4"
                    }

                    // 重量显示卡片（撑满中间区域）
                    Rectangle {
                        width: 340
                        height: 150
                        radius: 16
                        color: "#FFFFFF"
                        border.color: "#BBD3F0"
                        border.width: 2

                        Row {
                            anchors.centerIn: parent
                            spacing: 16

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: WeightManagerLeft.displayWeight.toFixed(2)
                                font.pixelSize: 120
                                font.bold: true
                                font.family: "DIN"
                                color: "#1A5FB4"
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "kg"
                                font.pixelSize: 40
                                font.bold: true
                                font.family: "PingFang SC"
                                color: "#1A5FB4"
                            }
                        }
                    }
                    // 归零 + 去皮 按钮（横向排列）
                    Row {
                        width: parent.width
                        height: 110
                        spacing: 24

                        // 归零按钮
                        Rectangle {
                            width: (parent.width - parent.spacing) / 2
                            height: 110
                            radius: 12
                            color: zeroBtnMa.pressed ? "#134A8E" : "#1A5FB4"

                            Text {
                                anchors.centerIn: parent
                                text: "归零"
                                font.pixelSize: 36
                                font.bold: true
                                font.family: "PingFang SC"
                                color: "#FFFFFF"
                            }

                            MouseArea {
                                id: zeroBtnMa
                                anchors.fill: parent
                                onClicked: {
                                    console.log("[Main] 左模块归零按钮")
                                    WeightManagerLeft.zero()
                                }
                            }
                        }

                        // 去皮按钮
                        Rectangle {
                            width: (parent.width - parent.spacing) / 2
                            height: 110
                            radius: 12
                            color: tareBtnMa.pressed ? "#134A8E" : "#1A5FB4"

                            Text {
                                anchors.centerIn: parent
                                text: "去皮"
                                font.pixelSize: 36
                                font.bold: true
                                font.family: "PingFang SC"
                                color: "#FFFFFF"
                            }

                            MouseArea {
                                id: tareBtnMa
                                anchors.fill: parent
                                onClicked: {
                                    console.log("[Main] 左模块去皮按钮")
                                    WeightManagerLeft.tare()
                                }
                            }
                        }
                    }
                }

                // ============ 右侧模块（归零）============
                Column {
                    width: (parent.width - 104) / 2
                    height: parent.height
                    spacing: 24

                    // 标签：右
                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        text: "右"
                        font.pixelSize: 32
                        font.bold: true
                        font.family: "PingFang SC"
                        color: "#1A5FB4"
                    }

                    // 重量显示卡片
                    Rectangle {
                        width: 340
                        height: 150
                        radius: 16
                        color: "#FFFFFF"
                        border.color: "#BBD3F0"
                        border.width: 2

                        Row {
                            anchors.centerIn: parent
                            spacing: 16

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: WeightManagerRight.displayWeight.toFixed(2)
                                font.pixelSize: 120
                                font.bold: true
                                font.family: "DIN"
                                color: "#1A5FB4"
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "kg"
                                font.pixelSize: 40
                                font.bold: true
                                font.family: "PingFang SC"
                                color: "#1A5FB4"
                            }
                        }
                    }
                    // 归零 + 去皮 按钮（横向排列）
                    Row {
                        width: parent.width
                        height: 110
                        spacing: 24

                        // 归零按钮
                        Rectangle {
                            width: (parent.width - parent.spacing) / 2
                            height: 110
                            radius: 12
                            color: zeroBtnMa.pressed ? "#134A8E" : "#1A5FB4"

                            Text {
                                anchors.centerIn: parent
                                text: "归零"
                                font.pixelSize: 36
                                font.bold: true
                                font.family: "PingFang SC"
                                color: "#FFFFFF"
                            }

                            MouseArea {
                                id: zeroBtnMa1
                                anchors.fill: parent
                                onClicked: {
                                    console.log("[Main] 右模块归零按钮")
                                    WeightManagerRight.zero()
                                }
                            }
                        }

                        // 去皮按钮
                        Rectangle {
                            width: (parent.width - parent.spacing) / 2
                            height: 110
                            radius: 12
                            color: tareBtnMa.pressed ? "#134A8E" : "#1A5FB4"

                            Text {
                                anchors.centerIn: parent
                                text: "去皮"
                                font.pixelSize: 36
                                font.bold: true
                                font.family: "PingFang SC"
                                color: "#FFFFFF"
                            }

                            MouseArea {
                                id: tareBtnMa1
                                anchors.fill: parent
                                onClicked: {
                                    console.log("[Main] 右模块去皮按钮")
                                    WeightManagerRight.tare()
                                }
                            }
                        }
                    }
                 
                }
            }
        }
    }

    // Esc 退出
    Shortcut {
        sequence: "Escape"
        enabled: true
        context: Qt.ApplicationShortcut
        onActivated: Qt.quit()
    }
}