/* Minimal Wi-Fi STA bring-up for the dfu_example_app example.
   Connects to CONFIG_DFU_EXAMPLE_WIFI_SSID and blocks until
   IP_EVENT_STA_GOT_IP fires. */

#include "wifi_sta.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

static const char *TAG = "wifi_sta";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t wifi_events;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "disconnected, reconnecting");
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip=" IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_connect_blocking(void)
{
    wifi_events = xEventGroupCreate();
    if (!wifi_events) return ESP_ERR_NO_MEM;

    /* esp_netif_init and esp_event_loop_create_default may already be up.
       Tolerate ESP_ERR_INVALID_STATE. */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init");

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_event_handler, NULL, NULL),
        TAG, "register WIFI_EVENT");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            wifi_event_handler, NULL, NULL),
        TAG, "register IP_EVENT_STA_GOT_IP");

    wifi_config_t wifi_cfg = {};
    strncpy((char *)wifi_cfg.sta.ssid,     CONFIG_DFU_EXAMPLE_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_DFU_EXAMPLE_WIFI_PASS,
            sizeof(wifi_cfg.sta.password) - 1);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg),
                        TAG, "set_config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");
    ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi_connect");

    ESP_LOGI(TAG, "connecting to '%s'...", CONFIG_DFU_EXAMPLE_WIFI_SSID);
    xEventGroupWaitBits(wifi_events, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "connected, UDP OTA listening on port %d",
             CONFIG_DFU_TRANSPORT_UDP_PORT);
    return ESP_OK;
}
