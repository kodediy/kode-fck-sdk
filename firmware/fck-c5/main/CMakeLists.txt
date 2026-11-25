idf_component_register(SRCS "fck_c5.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_uart esp_driver_gpio esp_wifi esp_event esp_timer bt
                                     esp_app_format)
