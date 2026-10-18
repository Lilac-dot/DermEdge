idf_component_register(SRCS "main.c" "screen_model.cc" "model/rash_gate_model_data.cc"
                       INCLUDE_DIRS "." "model"
                       PRIV_REQUIRES dermedge_net driver esp_timer json esp_psram)
