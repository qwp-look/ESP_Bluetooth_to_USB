idf_component_register(
    SRCS "ctrl_cdc.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp_tinyusb esp_system soc esp_rom
)
