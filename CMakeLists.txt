cmake_minimum_required(VERSION 3.16)

project(SmartScale VERSION 2.13.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(QT_QML_GENERATE_QMLLS_INI ON)

# ============================================================
# 版本号配置 (手动模式)
#   编译时: cmake .. -DBUILD_NUMBER=10
# ============================================================
set(BUILD_NUMBER "9" CACHE STRING "Build number (手动递增)")
string(TIMESTAMP APP_BUILD_DATE "%Y%m%d")
set(APP_VERSION_FULL "${PROJECT_VERSION}.${BUILD_NUMBER}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
)
message(STATUS "应用版本: ${APP_VERSION_FULL}_${APP_BUILD_DATE}")

# ============================================================
# Modbus 协议变体 (编译期宏开关)
#   cmake .. -DSMARTSCALE_MODBUS_PROTOCOL=feigong|v2
#   默认: feigong (飞功标准协议)
# ============================================================
set(SMARTSCALE_MODBUS_PROTOCOL "feigong" CACHE STRING "Modbus protocol variant: feigong|v2")
if(SMARTSCALE_MODBUS_PROTOCOL STREQUAL "v2")
    add_compile_definitions(SMARTSCALE_MODBUS_PROTOCOL_V2)
    message(STATUS "Modbus 协议: V2 (旧版, 0x0010 起 9 寄存器)")
else()
    add_compile_definitions(SMARTSCALE_MODBUS_PROTOCOL_FEIGONG)
    message(STATUS "Modbus 协议: Feigong (飞功标准, 0x0000 起 5 寄存器)")
endif()

# Qt 组件（仅保留称重通讯所需）
find_package(Qt6 REQUIRED COMPONENTS Quick VirtualKeyboard SerialPort Concurrent)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(appSmartScale
    app/main.cpp
)

add_definitions(-DQT_NO_KEYWORDS)

qt_add_qml_module(appSmartScale
    URI SmartScale
    QML_FILES
        src/ui/Main.qml

    # --- 硬件层 (Hardware) — 称重通讯（飞功 Modbus 协议） ---
        SOURCES src/hardware/WeightSensorWorker.h src/hardware/WeightSensorWorker.cpp
        SOURCES src/hardware/WeightSensor.h src/hardware/WeightSensor.cpp
)

# 大资源用 qt_add_big_resources 拆分编译
qt_add_big_resources(RCC_SOURCES app.qrc)
target_sources(appSmartScale PRIVATE ${RCC_SOURCES})

target_include_directories(appSmartScale
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

target_link_libraries(appSmartScale
    PRIVATE
        Qt6::Quick
        Qt6::VirtualKeyboard
        Qt6::SerialPort
        Qt6::Concurrent
)

install(TARGETS appSmartScale
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 部署资源文件（图标、水印等）到编译输出目录
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/resources"
     DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
