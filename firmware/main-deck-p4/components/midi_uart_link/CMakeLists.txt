idf_component_register(
    SRCS "midi_uart_link.c" "midi_stream_parser.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart controller_runtime control_link log freertos
)
