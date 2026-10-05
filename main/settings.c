/* Settings the owner enters on the web page, stored in NVS. */
#include <string.h>
#include "esp_attr.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "app.h"

#define NAMESPACE "cyd"
#define SETUP_MAGIC 0x5E7UL

settings_t g_settings = {
    .pool_host = "public-pool.io",
    .pool_port = 21496,
    .worker = "cyd",
    .currency = "USD",
};

/* Outlives a restart but not a power cycle, after which it holds noise. */
static RTC_NOINIT_ATTR uint32_t setup_request;

/* Leaves the default in place when nothing is stored. */
static void load_str(nvs_handle_t h, const char *key, char *out, size_t size)
{
    nvs_get_str(h, key, out, &size);
}

bool settings_load(void)
{
    if (nvs_flash_init() != ESP_OK) {
        /* whatever another firmware left in this part of the flash */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    nvs_handle_t h;
    settings_t *s = &g_settings;
    uint8_t flip = 0;
    if (nvs_open(NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false; /* nothing saved yet */
    load_str(h, "ssid", s->wifi_ssid, sizeof(s->wifi_ssid));
    load_str(h, "wifi_pass", s->wifi_pass, sizeof(s->wifi_pass));
    load_str(h, "pool_host", s->pool_host, sizeof(s->pool_host));
    nvs_get_u16(h, "pool_port", &s->pool_port);
    load_str(h, "address", s->btc_address, sizeof(s->btc_address));
    load_str(h, "worker", s->worker, sizeof(s->worker));
    load_str(h, "currency", s->currency, sizeof(s->currency));
    nvs_get_u8(h, "flip", &flip);
    s->lcd_flip = flip;
    load_str(h, "admin_pass", s->admin_pass, sizeof(s->admin_pass));
    nvs_close(h);
    return s->wifi_ssid[0] && s->btc_address[0];
}

bool settings_save(const settings_t *s)
{
    nvs_handle_t h;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_str(h, "ssid", s->wifi_ssid);
    if (!err) err = nvs_set_str(h, "wifi_pass", s->wifi_pass);
    if (!err) err = nvs_set_str(h, "pool_host", s->pool_host);
    if (!err) err = nvs_set_u16(h, "pool_port", s->pool_port);
    if (!err) err = nvs_set_str(h, "address", s->btc_address);
    if (!err) err = nvs_set_str(h, "worker", s->worker);
    if (!err) err = nvs_set_str(h, "currency", s->currency);
    if (!err) err = nvs_set_u8(h, "flip", s->lcd_flip);
    if (!err) err = nvs_set_str(h, "admin_pass", s->admin_pass);
    if (!err) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

void settings_request_setup(void)
{
    setup_request = SETUP_MAGIC;
}

bool settings_take_setup_request(void)
{
    bool asked = setup_request == SETUP_MAGIC;
    setup_request = 0;
    return asked;
}
