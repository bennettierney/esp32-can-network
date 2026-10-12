idf_component_register(SRCS "main.cpp" "bme280.cpp" "can_bus.c"
                       PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_timer esp_driver_twai can_protocol
                       INCLUDE_DIRS "")
