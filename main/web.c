/*
 * The device's web page: status, settings and firmware upload. In setup mode
 * the same page is what a phone joining the setup network is shown.
 *
 * Requests are only taken when addressed to the device by its IP address, and
 * changes only from the page itself. A web page elsewhere can make a browser
 * on this network send requests here, but not under that name: without the
 * check it could set the payout address.
 */
#include <ctype.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"
#include "app.h"

static const char *TAG = "web";

#define BODY_MAX 1024
#define UPLOAD_CHUNK 4096
#define SCAN_MAX 20
/* How far into a firmware image its description reaches. */
#define APP_DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define APP_DESC_END (APP_DESC_OFFSET + sizeof(esp_app_desc_t))

extern const char index_html[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t server;
static int64_t last_request;

static void own_ip(char out[16])
{
    state_lock();
    strcpy(out, g_state.ip);
    state_unlock();
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", message);
    httpd_resp_set_status(req, status);
    return send_json(req, json);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    return send_json(req, json);
}

/* Sends the browser to the page under the device's address. */
static esp_err_t redirect_home(httpd_req_t *req)
{
    char ip[16], url[32];
    own_ip(ip);
    snprintf(url, sizeof(url), "http://%s/", ip);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    /* an iPhone only takes a redirect with a body for a setup page */
    return httpd_resp_sendstr(req, "The page is at the device's address.");
}

static bool host_is_ours(httpd_req_t *req)
{
    char host[32], ip[16];
    own_ip(ip);
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    char *port = strchr(host, ':');
    if (port) *port = 0;
    return ip[0] && !strcmp(host, ip);
}

/* A browser names the page a request was made from; anything else that talks HTTP does not. */
static bool origin_is_ours(httpd_req_t *req)
{
    char origin[40], ip[16], want[32];
    if (!httpd_req_get_hdr_value_len(req, "Origin")) return true;
    own_ip(ip);
    snprintf(want, sizeof(want), "http://%s", ip);
    return httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK && !strcmp(origin, want);
}

static bool password_given(httpd_req_t *req)
{
    char value[128], given[sizeof(g_settings.admin_pass)] = {0};
    unsigned char plain[96];
    size_t n = 0;

    if (httpd_req_get_hdr_value_str(req, "Authorization", value, sizeof(value)) != ESP_OK) return false;
    if (strncmp(value, "Basic ", 6)) return false;
    if (mbedtls_base64_decode(plain, sizeof(plain) - 1, &n, (unsigned char *)value + 6, strlen(value + 6))) return false;
    plain[n] = 0;
    const char *colon = strchr((char *)plain, ':'); /* any user name will do */
    if (!colon) return false;

    /* compared in full, so that the time taken says nothing about where they differ */
    unsigned diff = strlcpy(given, colon + 1, sizeof(given)) >= sizeof(given);
    for (size_t i = 0; i < sizeof(given); i++) diff |= given[i] ^ g_settings.admin_pass[i];
    return !diff;
}

/*
 * Every handler asks this first. False means the request has been answered
 * and is not to be acted on. Setup mode asks for no password: it takes holding
 * the screen to get there, and it is the way back in for an owner who has
 * forgotten theirs.
 */
static bool admit(httpd_req_t *req, bool changes)
{
    last_request = esp_timer_get_time();
    if (!host_is_ours(req) || (changes && !origin_is_ours(req))) {
        send_error(req, "403 Forbidden", "Use the page at the device's own address.");
        return false;
    }
    if (!g_state.setup && g_settings.admin_pass[0] && !password_given(req)) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"cyd-miner\"");
        send_error(req, "401 Unauthorized", "Password needed.");
        return false;
    }
    return true;
}

static void restart(void *arg)
{
    esp_restart();
}

/* Restarts once the answer to the request has had time to leave. */
static void restart_soon(void)
{
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {.callback = restart, .name = "restart"};
    if (!timer && esp_timer_create(&args, &timer) == ESP_OK) esp_timer_start_once(timer, 1500 * 1000);
}

static esp_err_t page_get(httpd_req_t *req)
{
    last_request = esp_timer_get_time();
    if (!host_is_ours(req)) return redirect_home(req);
    if (!admit(req, false)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, index_html, index_html_end - index_html - 1);
}

/* In setup mode, whatever page a phone asks for to test the network leads to ours. */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t err)
{
    last_request = esp_timer_get_time();
    if (g_state.setup) redirect_home(req);
    else httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Nothing here.");
    return ESP_FAIL; /* closes the connection, which may still hold a request body */
}

static esp_err_t status_get(httpd_req_t *req)
{
    if (!admit(req, false)) return ESP_OK;

    state_lock();
    state_t s = g_state;
    state_unlock();
    wifi_ap_record_t ap;
    const settings_t *set = &g_settings;
    cJSON *j = cJSON_CreateObject(), *fees, *o;

    cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
    cJSON_AddBoolToObject(j, "setup", s.setup);
    cJSON_AddNumberToObject(j, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(j, "heap", esp_get_free_heap_size());

    cJSON_AddNumberToObject(j, "hashrate", s.hashrate);
    cJSON_AddStringToObject(j, "sha", s.hw_loop ? s.hw_loop : "software");
    cJSON_AddNumberToObject(j, "shares_ok", s.shares_ok);
    cJSON_AddNumberToObject(j, "shares_bad", s.shares_bad);
    cJSON_AddNumberToObject(j, "best_diff", s.best_diff);
    cJSON_AddNumberToObject(j, "pool_diff", s.pool_diff);
    cJSON_AddBoolToObject(j, "pool_connected", s.pool_connected);

    cJSON_AddStringToObject(j, "ip", s.ip);
    if (s.wifi_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(j, "rssi", ap.rssi);

    if (s.have_network) {
        cJSON_AddNumberToObject(j, "height", s.height);
        cJSON_AddNumberToObject(j, "net_difficulty", s.net_difficulty);
        cJSON_AddNumberToObject(j, "net_hashrate", s.net_hashrate);
    }
    if (s.have_price) cJSON_AddNumberToObject(j, "price", s.price);
    if (s.have_fees) {
        fees = cJSON_AddObjectToObject(j, "fees");
        cJSON_AddNumberToObject(fees, "fast", s.fee_fast);
        cJSON_AddNumberToObject(fees, "mid", s.fee_mid);
        cJSON_AddNumberToObject(fees, "slow", s.fee_slow);
    }

    /* the passwords stay here: the page is only told whether there is one */
    o = cJSON_AddObjectToObject(j, "settings");
    cJSON_AddStringToObject(o, "ssid", set->wifi_ssid);
    cJSON_AddBoolToObject(o, "has_wifi_pass", set->wifi_pass[0]);
    cJSON_AddStringToObject(o, "pool_host", set->pool_host);
    cJSON_AddNumberToObject(o, "pool_port", set->pool_port);
    cJSON_AddStringToObject(o, "address", set->btc_address);
    cJSON_AddStringToObject(o, "worker", set->worker);
    cJSON_AddStringToObject(o, "currency", set->currency);
    cJSON_AddBoolToObject(o, "flip", set->lcd_flip);
    cJSON_AddBoolToObject(o, "has_admin_pass", set->admin_pass[0]);
    return send_json(req, j);
}

/* Networks in range, strongest first, for the page to offer. */
static esp_err_t scan_get(httpd_req_t *req)
{
    if (!admit(req, false)) return ESP_OK;

    cJSON *list = cJSON_CreateArray();
    uint16_t n = SCAN_MAX;
    wifi_ap_record_t *aps = calloc(n, sizeof(*aps));
    if (aps && esp_wifi_scan_start(NULL, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, aps) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            bool seen = !aps[i].ssid[0]; /* a hidden network has no name to offer */
            for (int k = 0; k < i && !seen; k++) seen = !strcmp((char *)aps[k].ssid, (char *)aps[i].ssid);
            if (seen) continue;
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", (char *)aps[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
            cJSON_AddBoolToObject(o, "open", aps[i].authmode == WIFI_AUTH_OPEN);
            cJSON_AddItemToArray(list, o);
        }
    }
    free(aps);
    return send_json(req, list);
}

static bool all_of(const char *s, const char *also)
{
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && !strchr(also, *s)) return false;
    }
    return true;
}

/*
 * Copies the string at key, if the request has one, into out. The value has
 * to be min to size - 1 characters long and, when also is given, made of
 * letters, digits and the characters in also: the pool settings go into JSON
 * sent to the pool as they are.
 */
static bool take(const cJSON *j, const char *key, char *out, size_t size, size_t min, const char *also)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    if (!v) return true;
    if (!cJSON_IsString(v)) return false;
    size_t len = strlen(v->valuestring);
    if (len < min || len >= size || (also && !all_of(v->valuestring, also))) return false;
    strcpy(out, v->valuestring);
    return true;
}

/* Applies the request to s; returns what is wrong with it, or NULL. */
static const char *apply(const cJSON *j, settings_t *s)
{
    static const char *const currencies[] = {"USD", "EUR", "GBP", "CAD", "CHF", "AUD", "JPY"};
    const cJSON *v;
    bool known = false;

    if (!take(j, "ssid", s->wifi_ssid, sizeof(s->wifi_ssid), 1, NULL)) return "Enter the name of a WiFi network.";
    if (!take(j, "wifi_pass", s->wifi_pass, sizeof(s->wifi_pass), 0, NULL) ||
        (s->wifi_pass[0] && strlen(s->wifi_pass) < 8))
        return "A WiFi password has 8 to 64 characters.";
    if (!take(j, "pool_host", s->pool_host, sizeof(s->pool_host), 1, ".-")) return "Enter the pool's host name.";
    if ((v = cJSON_GetObjectItem(j, "pool_port"))) {
        if (!cJSON_IsNumber(v) || v->valuedouble < 1 || v->valuedouble > 65535) return "The pool port is 1 to 65535.";
        s->pool_port = (uint16_t)v->valuedouble;
    }
    if (!take(j, "address", s->btc_address, sizeof(s->btc_address), 1, "._-")) return "Enter a Bitcoin address.";
    if (!take(j, "worker", s->worker, sizeof(s->worker), 0, "_-"))
        return "A worker name has up to 31 letters, digits, - or _.";
    if (!take(j, "currency", s->currency, sizeof(s->currency), 3, "")) return "Unknown currency.";
    for (size_t i = 0; i < sizeof(currencies) / sizeof(currencies[0]); i++) known |= !strcmp(s->currency, currencies[i]);
    if (!known) return "Unknown currency.";
    if ((v = cJSON_GetObjectItem(j, "flip"))) s->lcd_flip = cJSON_IsTrue(v);
    if (!take(j, "admin_pass", s->admin_pass, sizeof(s->admin_pass), 0, NULL))
        return "A page password has up to 32 characters.";
    /* trailing bytes of the old password would count as part of the new one */
    memset(s->admin_pass + strlen(s->admin_pass), 0, sizeof(s->admin_pass) - strlen(s->admin_pass));

    if (!s->wifi_ssid[0]) return "Enter the name of a WiFi network.";
    if (!s->btc_address[0]) return "Enter a Bitcoin address.";
    return NULL;
}

/* Takes any subset of the settings as JSON, saves and restarts. */
static esp_err_t settings_post(httpd_req_t *req)
{
    if (!admit(req, true)) return ESP_OK;

    char body[BODY_MAX];
    size_t len = req->content_len, got = 0;
    if (len >= sizeof(body)) return send_error(req, "400 Bad Request", "Too much to be settings.");
    while (got < len) {
        int n = httpd_req_recv(req, body + got, len - got);
        if (n <= 0) return ESP_FAIL;
        got += n;
    }

    cJSON *j = cJSON_ParseWithLength(body, len);
    settings_t s = g_settings;
    const char *wrong = cJSON_IsObject(j) ? apply(j, &s) : "That is not settings.";
    cJSON_Delete(j);
    if (wrong) return send_error(req, "400 Bad Request", wrong);
    if (!settings_save(&s)) return send_error(req, "500 Internal Server Error", "Could not save the settings.");

    ESP_LOGI(TAG, "settings saved, restarting");
    restart_soon();
    return send_ok(req);
}

static esp_err_t restart_post(httpd_req_t *req)
{
    if (!admit(req, true)) return ESP_OK;
    restart_soon();
    return send_ok(req);
}

/* Reads until buf holds len bytes; false when the connection gave out. */
static bool recv_all(httpd_req_t *req, char *buf, size_t len)
{
    int stalls = 0;
    while (len) {
        int n = httpd_req_recv(req, buf, len);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++stalls < 6) continue;
        if (n <= 0) return false;
        buf += n;
        len -= n;
    }
    return true;
}

/*
 * Takes a firmware image as the request body and writes it to the app slot
 * that is not running. The slot is only booted from if all of the image
 * arrived and its checksum and hash are right, so an upload that fails
 * leaves the running firmware in charge.
 */
static esp_err_t update_post(httpd_req_t *req)
{
    if (!admit(req, true)) return ESP_OK;

    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    const char *wrong = NULL;
    esp_ota_handle_t ota = 0;
    bool writing = false;
    size_t left = req->content_len;
    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) return ESP_FAIL;

    if (!slot) wrong = "This device has no second firmware slot.";
    else if (left < APP_DESC_END || left > slot->size) wrong = "That is not a cyd-miner firmware file.";

    g_state.updating = true; /* writing flash stalls both cores, and the hardware loop gets hashes wrong for it */
    while (left) {
        size_t n = left < UPLOAD_CHUNK ? left : UPLOAD_CHUNK;
        if (!recv_all(req, buf, n)) {
            if (writing) esp_ota_abort(ota);
            free(buf);
            g_state.updating = false;
            ESP_LOGW(TAG, "firmware upload broke off");
            return ESP_FAIL;
        }
        left -= n;
        if (wrong) continue; /* the browser only reads the answer once it has sent everything */
        if (!writing) {
            const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + APP_DESC_OFFSET);
            if ((uint8_t)buf[0] != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD ||
                strncmp(desc->project_name, esp_app_get_description()->project_name, sizeof(desc->project_name))) {
                wrong = "That is not a cyd-miner firmware file. Use the one named ota.";
                continue;
            }
            if (esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
                wrong = "Could not start the update.";
                continue;
            }
            writing = true;
        }
        if (esp_ota_write(ota, buf, n) != ESP_OK) wrong = "Could not write the update.";
    }
    free(buf);

    if (wrong && writing) esp_ota_abort(ota);
    else if (!wrong && esp_ota_end(ota) != ESP_OK) wrong = "The firmware file is damaged.";
    if (!wrong && esp_ota_set_boot_partition(slot) != ESP_OK) wrong = "Could not switch to the new firmware.";
    if (wrong) {
        g_state.updating = false;
        ESP_LOGW(TAG, "firmware upload refused: %s", wrong);
        return send_error(req, "400 Bad Request", wrong);
    }

    ESP_LOGI(TAG, "firmware written to %s, restarting", slot->label);
    restart_soon();
    return send_ok(req);
}

void web_confirm_firmware(void)
{
    /* without the page there would be no way to upload a better one */
    if (server) esp_ota_mark_app_valid_cancel_rollback();
}

int web_idle_seconds(void)
{
    return (int)((esp_timer_get_time() - last_request) / 1000000);
}

void web_start(void)
{
    static const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post},
        {.uri = "/api/restart", .method = HTTP_POST, .handler = restart_post},
        {.uri = "/api/update", .method = HTTP_POST, .handler = update_post},
    };
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0;
    config.stack_size = 8192;
    /* lwIP has ten sockets, and the pool, the stats and the server itself need theirs */
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    if (g_state.setup) {
        /* phones testing the network ask for much that is not here */
        esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
        esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
        esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    }
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "the web page could not be started");
        server = NULL;
        return;
    }
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) httpd_register_uri_handler(server, &routes[i]);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
}
