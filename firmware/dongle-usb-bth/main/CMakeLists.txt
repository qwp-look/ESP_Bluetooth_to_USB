idf_component_register(
    SRCS "main.c" "usb_descriptors.c" "hci_tl_bth.c" "ctrl_tusb.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES espressif__tinyusb bt freertos esp_ringbuf nvs_flash esp_hw_support
)

# 把本目录的 tusb_config.h 注入 TinyUSB 组件的编译（esp_tinyusb 同款做法）
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

# TinyUSB 的描述符回调在本组件里定义，但链接顺序上 main 在 tinyusb 之前，
# 无人引用 usb_descriptors.o 会被归档库跳过——用 -u 强制拉入。
idf_build_set_property(LINK_OPTIONS "-u;tud_descriptor_device_cb" APPEND)
