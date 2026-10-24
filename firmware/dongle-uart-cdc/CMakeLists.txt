# 阶段 3a：S3 前端固件——UART1(GPIO17/18) ↔ USB CDC 透明桥。
# 经典 ESP32 跑 radio-bridge 出 HCI H:4 字节流，本固件原样搬运到 USB，
# 主机侧 btattach -P h4 直接当标准蓝牙控制器用（双模）。
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components/ctrl_cdc")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dongle_uart_cdc)
