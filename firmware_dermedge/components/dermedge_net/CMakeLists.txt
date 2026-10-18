idf_component_register(SRCS "dermedge_net.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash)
