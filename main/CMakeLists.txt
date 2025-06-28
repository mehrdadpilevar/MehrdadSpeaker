idf_component_register(SRCS "web_control.c" "bt_app_av.c"
                            "bt_app_core.c"
                            "main.c"
                    PRIV_REQUIRES esp_driver_i2s bt nvs_flash esp_ringbuf esp_driver_dac esp_driver_gpio esp_http_server
                    INCLUDE_DIRS ".")
