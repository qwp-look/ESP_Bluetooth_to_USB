# 阶段 2：S3 单板 BLE 即插即用 dongle——USB class 0xE0（蓝牙 HCI 传输）。
# 主机侧无需 btattach，Linux btusb / Windows inbox 驱动按类匹配自动加载。
cmake_minimum_required(VERSION 3.16)

# 本工程直接使用原生 TinyUSB（bth 类），不使用 esp_tinyusb 封装
# 与共享组件 ctrl_cdc（它依赖 esp_tinyusb）

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dongle_usb_bth)
