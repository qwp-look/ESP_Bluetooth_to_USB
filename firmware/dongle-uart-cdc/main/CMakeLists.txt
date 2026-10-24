idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES ctrl_cdc esp_driver_uart esp_ringbuf freertos
)
