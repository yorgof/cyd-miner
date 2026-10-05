/* Periodically fetches network stats, price and fees from mempool.space. */
#include <stdbool.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "config.h"

static const char *TAG = "stats";

typedef struct {
    char *buf;
    int cap, len;
} response_t;

static esp_err_t on_http_event(esp_http_client_event_t *ev)
{
    if (ev->event_id == HTTP_EVENT_ON_DATA) {
        response_t *r = ev->user_data;
        int n = ev->data_len < r->cap - 1 - r->len ? ev->data_len : r->cap - 1 - r->len;
        memcpy(r->buf + r->len, ev->data, n);
        r->len += n;
    }
    return ESP_OK;
}

static cJSON *get_json(const char *url)
{
    static char body[4096];
    response_t r = {body, sizeof(body), 0};
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http_event,
        .user_data = &r,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "%s failed: %s, status %d", url, esp_err_to_name(err), status);
        return NULL;
    }
    body[r.len] = 0;
    return cJSON_Parse(body);
}

static double num(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static void refresh(void)
{
    cJSON *j;

    /* the tip height endpoint returns a bare number */
    if ((j = get_json(MEMPOOL_API_URL "/blocks/tip/height"))) {
        if (cJSON_IsNumber(j)) {
            state_lock();
            g_state.height = (uint32_t)j->valuedouble;
            state_unlock();
        }
        cJSON_Delete(j);
    }
    if ((j = get_json(MEMPOOL_API_URL "/v1/mining/hashrate/3d"))) {
        state_lock();
        g_state.net_difficulty = num(j, "currentDifficulty");
        g_state.net_hashrate = num(j, "currentHashrate");
        g_state.have_network = g_state.height != 0 && g_state.net_difficulty > 0;
        state_unlock();
        cJSON_Delete(j);
    }
    if ((j = get_json(MEMPOOL_API_URL "/v1/prices"))) {
        state_lock();
        g_state.price = num(j, g_settings.currency);
        g_state.have_price = g_state.price > 0;
        state_unlock();
        cJSON_Delete(j);
    }
    /*
     * "recommended" rounds up to whole sat/vB, which is 1 across the board
     * whenever blocks are not full. "precise" has the fractions; an older
     * mempool server only has the former.
     */
    if ((j = get_json(MEMPOOL_API_URL "/v1/fees/precise")) || (j = get_json(MEMPOOL_API_URL "/v1/fees/recommended"))) {
        state_lock();
        g_state.fee_fast = num(j, "fastestFee");
        g_state.fee_mid = num(j, "halfHourFee");
        g_state.fee_slow = num(j, "hourFee");
        g_state.have_fees = g_state.fee_fast > 0;
        state_unlock();
        cJSON_Delete(j);
    }
}

static void stats_task(void *arg)
{
    for (;;) {
        state_lock();
        bool wifi = g_state.wifi_connected;
        state_unlock();
        if (!wifi) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        refresh();
        vTaskDelay(pdMS_TO_TICKS(STATS_REFRESH_SECONDS * 1000));
    }
}

void stats_start(void)
{
    xTaskCreatePinnedToCore(stats_task, "stats", 8192, NULL, 3, NULL, 0);
}
