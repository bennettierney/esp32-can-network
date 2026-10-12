idf_component_register(SRCS "main.cpp" "bme280.cpp" "ble_env.c" "can_bus.c"
                       PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_timer bt nvs_flash esp_driver_twai can_protocol
                       INCLUDE_DIRS "")
