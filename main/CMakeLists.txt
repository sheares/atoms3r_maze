idf_component_register(
    SRCS
        "main.c"
        "gc9107.c"
        "joystick2.c"
        "lp5562.c"
        "maze.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_driver_gpio
        esp_driver_spi
        esp_driver_i2c
        esp_timer
        heap
)
