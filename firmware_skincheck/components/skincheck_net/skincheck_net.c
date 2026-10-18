#include "skincheck_net.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "skincheck_net";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define BOUNDARY "----skincheckboundary7f3a"

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "connected, IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, CONFIG_SKINCHECK_WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, CONFIG_SKINCHECK_WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Power save adds 100+ ms latency to every HTTP request; the demo prefers speed.
    esp_wifi_set_ps(WIFI_PS_NONE);
}

esp_err_t skincheck_wifi_connect(int timeout_ms)
{
    // Safe to call again after a timeout: the stack starts once, later calls just wait
    // (the event handler keeps retrying the connection in the background).
    static bool started = false;
    if (!started) {
        wifi_start();
        started = true;
    }
    ESP_LOGI(TAG, "joining \"%s\"", CONFIG_SKINCHECK_WIFI_SSID);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

static int write_all(esp_http_client_handle_t c, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int n = esp_http_client_write(c, p, len);
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= n;
    }
    return 0;
}

static int finish(esp_http_client_handle_t c, char *resp, size_t resp_len)
{
    int status = -1;
    if (esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        int n = 0;
        if (resp && resp_len > 1) {
            n = esp_http_client_read_response(c, resp, resp_len - 1);
            resp[n > 0 ? n : 0] = '\0';
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static esp_http_client_handle_t open_post(const char *path, const char *content_type, size_t len)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", CONFIG_SKINCHECK_HUB_URL, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,  // the hub's first analyze after start-up can take a few seconds
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return NULL;
    }
    esp_http_client_set_header(c, "Content-Type", content_type);
    if (esp_http_client_open(c, len) != ESP_OK) {
        ESP_LOGE(TAG, "can't reach hub at %s", url);
        esp_http_client_cleanup(c);
        return NULL;
    }
    return c;
}

int skincheck_hub_post_jpeg(const char *path, const uint8_t *jpg, size_t jpg_len,
                            char *resp, size_t resp_len)
{
    static const char head[] = "--" BOUNDARY "\r\n"
                               "Content-Disposition: form-data; name=\"image\"; filename=\"frame.jpg\"\r\n"
                               "Content-Type: image/jpeg\r\n\r\n";
    static const char tail[] = "\r\n--" BOUNDARY "--\r\n";
    size_t total = sizeof(head) - 1 + jpg_len + sizeof(tail) - 1;
    esp_http_client_handle_t c = open_post(path, "multipart/form-data; boundary=" BOUNDARY, total);
    if (!c) {
        return -1;
    }
    if (write_all(c, head, sizeof(head) - 1) || write_all(c, jpg, jpg_len) ||
        write_all(c, tail, sizeof(tail) - 1)) {
        esp_http_client_cleanup(c);
        return -1;
    }
    return finish(c, resp, resp_len);
}

int skincheck_hub_post_json(const char *path, const char *json, size_t json_len,
                            char *resp, size_t resp_len)
{
    esp_http_client_handle_t c = open_post(path, "application/json", json_len);
    if (!c) {
        return -1;
    }
    if (write_all(c, json, json_len)) {
        esp_http_client_cleanup(c);
        return -1;
    }
    return finish(c, resp, resp_len);
}
