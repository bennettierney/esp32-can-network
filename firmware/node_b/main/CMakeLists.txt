idf_component_register(SRCS "main.cpp" "ble_env.c"
                       PRIV_REQUIRES esp_driver_twai can_protocol bt nvs_flash
                       INCLUDE_DIRS "")
