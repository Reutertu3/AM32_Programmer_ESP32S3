idf_component_register(
    SRCS "main.c"
         "host_link.c"
         "msp.c"
         "serial_4way.c"
         "blheli_bootloader.c"
         "esc_io.c"
    INCLUDE_DIRS "."
    # esp_driver_usb_serial_jtag is only used by the C3 backend
    # (ESC4W_USB_TINYUSB = 0), but listing it unconditionally costs
    # nothing and the header cannot be found without it.
    REQUIRES esp_driver_uart
             esp_driver_gpio
             esp_driver_usb_serial_jtag
             soc
             esp_rom
             esp_timer
             esp_hw_support
)
