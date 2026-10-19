idf_component_register(
    SRCS "main.c" "link.c" "../../common/linkcodec.c"
    INCLUDE_DIRS "." "../../common"
    REQUIRES bt nvs_flash esp_driver_gpio esp_driver_usb_serial_jtag esp_app_format esp_ringbuf esp_timer
)
