# 阶段 3b：最终形态——UART(H:4 来自经典板 radio-bridge) ↔ USB class 0xE0 双模即插即用 dongle。
# 直接使用原生 TinyUSB（bth 类），不使用 esp_tinyusb 封装与共享组件 ctrl_cdc。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dongle_uart_bth)
