/*
 * WiFi. With settings, joins the owner's network and keeps reconnecting.
 * In setup mode, serves an open network of its own on which every name
 * leads to the web page, so that a phone joining it is shown the page.
 */
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "app.h"
#include "config.h"

static const char *TAG = "net";

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        state_lock();
        g_state.wifi_connected = false;
        state_unlock();
        ESP_LOGW(TAG, "disconnected, retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        state_lock();
        snprintf(g_state.ip, sizeof(g_state.ip), IPSTR, IP2STR(&ev->ip_info.ip));
        g_state.wifi_connected = true;
        state_unlock();
        ESP_LOGI(TAG, "connected, ip %s", g_state.ip);
        web_confirm_firmware();
    }
}

static void join_network(void)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, HOSTNAME);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    /* a name or password of full length fills its field, with no terminator */
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, g_settings.wifi_ssid, strnlen(g_settings.wifi_ssid, sizeof(cfg.sta.ssid)));
    memcpy(cfg.sta.password, g_settings.wifi_pass, strnlen(g_settings.wifi_pass, sizeof(cfg.sta.password)));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

static void serve_setup_network(void)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta(); /* only scans, for the web page's list of networks */

    wifi_config_t cfg = {.ap = {.channel = 1, .max_connection = 4, .authmode = WIFI_AUTH_OPEN}};
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), HOSTNAME "-%02X%02X", mac[4], mac[5]);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));

    esp_netif_ip_info_t info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(netif, &info));
    state_lock();
    strlcpy(g_state.ap_ssid, (char *)cfg.ap.ssid, sizeof(g_state.ap_ssid));
    snprintf(g_state.ip, sizeof(g_state.ip), IPSTR, IP2STR(&info.ip));
    state_unlock();
    dns_start(info.ip.addr);
    ESP_LOGI(TAG, "setup network %s, page at http://%s/", g_state.ap_ssid, g_state.ip);
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* settings.c keeps the network name and password; the driver need not as well */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (g_state.setup) serve_setup_network();
    else join_network();
    /* ahead of the network, so that the page is there when the first address arrives */
    web_start();
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    if (g_state.setup) web_confirm_firmware();
}
