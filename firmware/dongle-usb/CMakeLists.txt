# Top-level CMake file for the dongle-usb firmware (ESP32-S3, phase 1).
cmake_minimum_required(VERSION 3.16)

# 跨工程共享组件（按需精确引用，避免强拉依赖）
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/ctrl_cdc")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dongle_usb_ble)
