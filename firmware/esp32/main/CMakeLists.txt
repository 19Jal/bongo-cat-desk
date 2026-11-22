idf_component_register(SRCS "main.c" "sh1106.c" "ui.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_usb_serial_jtag
                                     esp_driver_uart
                                     esp_driver_i2c
                                     esp_timer
                                     led_strip)
