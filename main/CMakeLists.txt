idf_component_register(
    SRCS "main.c"
         "host_link.c"
         "msp.c"
         "serial_4way.c"
         "blheli_bootloader.c"
         "esc_io.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_hw_support
)
