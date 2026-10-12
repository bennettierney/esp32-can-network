idf_component_register(SRCS "main.cpp" "bme280.cpp"
                       PRIV_REQUIRES esp_driver_i2c esp_driver_gpio
                       INCLUDE_DIRS "")
