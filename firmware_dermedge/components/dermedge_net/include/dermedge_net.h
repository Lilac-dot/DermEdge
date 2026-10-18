// WiFi + laptop-hub HTTP helpers shared by the DermEdge ESP32 projects.
// The hub is app/server.py on the laptop; both devices join the same WiFi
// (e.g. the laptop's hotspot) so nothing goes over the internet.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Connects to CONFIG_DERMEDGE_WIFI_SSID and blocks until an IP is assigned
// (or timeout_ms passes). Initialises NVS, netif and the default event loop.
esp_err_t dermedge_wifi_connect(int timeout_ms);

// POSTs a JPEG as multipart/form-data field "image" to CONFIG_DERMEDGE_HUB_URL + path
// (path may carry a query string). Writes the response body (NUL-terminated) to resp.
// Returns the HTTP status code, or -1 on a connection error.
int dermedge_hub_post_jpeg(const char *path, const uint8_t *jpg, size_t jpg_len,
                            char *resp, size_t resp_len);

// POSTs a JSON body to CONFIG_DERMEDGE_HUB_URL + path. Returns the HTTP status code, or -1.
int dermedge_hub_post_json(const char *path, const char *json, size_t json_len,
                            char *resp, size_t resp_len);

#ifdef __cplusplus
}
#endif
