idf_component_register(SRCS "main.c" "signals.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES skincheck_net driver esp_adc esp_timer)
