idf_component_register(
    SRCS "main.c" "hci_tl_usb.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ctrl_cdc bt freertos esp_ringbuf nvs_flash
)
