/* Stratum v1 client: keeps a pool connection, tracks the current job, submits shares. */
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "app.h"
#include "config.h"

static const char *TAG = "stratum";

#define LINE_BUF 8192
#define IDLE_TIMEOUT_S 180
#define SUBMIT_ID_BASE 10

static int sock = -1;
static SemaphoreHandle_t tx_lock, job_lock;
static job_t job;
static bool have_job;
static volatile uint32_t generation;
static uint32_t en2_counter, submit_id;
static char extranonce1[64];
static size_t en2_size;
static char line[LINE_BUF];
/* The name the pool knows this miner by: the payout address, then the worker name if there is one. */
static char user[sizeof(g_settings.btc_address) + sizeof(g_settings.worker)];

static bool send_line(const char *msg)
{
    bool ok = false;
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    if (sock >= 0) ok = send(sock, msg, strlen(msg), 0) == (ssize_t)strlen(msg);
    xSemaphoreGive(tx_lock);
    return ok;
}

uint32_t stratum_generation(void)
{
    return generation;
}

bool stratum_get_work(uint32_t *gen, work_t *work, double *difficulty)
{
    bool ok;
    xSemaphoreTake(job_lock, portMAX_DELAY);
    ok = have_job;
    if (ok) {
        *gen = generation;
        work_build(&job, en2_counter++, work);
    }
    xSemaphoreGive(job_lock);
    if (ok) {
        state_lock();
        *difficulty = g_state.pool_diff;
        state_unlock();
    }
    return ok;
}

void stratum_submit(const work_t *work, uint32_t nonce)
{
    char msg[384];
    xSemaphoreTake(job_lock, portMAX_DELAY);
    uint32_t id = SUBMIT_ID_BASE + submit_id++;
    xSemaphoreGive(job_lock);
    snprintf(msg, sizeof(msg),
             "{\"id\":%u,\"method\":\"mining.submit\",\"params\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%08x\"]}\n",
             (unsigned)id, user, work->job_id, work->en2_hex, work->ntime_hex, (unsigned)nonce);
    if (!send_line(msg)) ESP_LOGW(TAG, "share not sent, pool offline");
}

static const char *str_at(const cJSON *arr, int i)
{
    const cJSON *item = cJSON_GetArrayItem(arr, i);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static void on_notify(const cJSON *params)
{
    const char *branches[MERKLE_MAX];
    const cJSON *list = cJSON_GetArrayItem(params, 4);
    int n = cJSON_GetArraySize(list);
    const char *f[8];
    static const int idx[] = {0, 1, 2, 3, 5, 6, 7}; /* id, prevhash, coinb1, coinb2, version, nbits, ntime */

    for (int i = 0; i < 7; i++) {
        if (!(f[i] = str_at(params, idx[i]))) return;
    }
    if (n > MERKLE_MAX || !extranonce1[0]) return;
    for (int i = 0; i < n; i++) {
        if (!(branches[i] = str_at(list, i))) return;
    }

    xSemaphoreTake(job_lock, portMAX_DELAY);
    have_job = job_build(&job, f[0], f[1], f[2], extranonce1, en2_size, f[3], branches, n, f[4], f[5], f[6]);
    generation++;
    xSemaphoreGive(job_lock);
    if (!have_job) ESP_LOGE(TAG, "could not use job %s", f[0]);
}

static void on_message(const char *text)
{
    cJSON *msg = cJSON_Parse(text);
    if (!msg) {
        ESP_LOGW(TAG, "bad json from pool");
        return;
    }
    const cJSON *method = cJSON_GetObjectItem(msg, "method");
    const cJSON *params = cJSON_GetObjectItem(msg, "params");
    const cJSON *id = cJSON_GetObjectItem(msg, "id");
    const cJSON *result = cJSON_GetObjectItem(msg, "result");

    if (cJSON_IsString(method)) {
        if (!strcmp(method->valuestring, "mining.notify")) {
            on_notify(params);
        } else if (!strcmp(method->valuestring, "mining.set_difficulty")) {
            const cJSON *d = cJSON_GetArrayItem(params, 0);
            if (cJSON_IsNumber(d) && d->valuedouble > 0) {
                state_lock();
                g_state.pool_diff = d->valuedouble;
                state_unlock();
                generation++; /* make the miners pick up the new target */
                ESP_LOGI(TAG, "difficulty %g", d->valuedouble);
            }
        }
    } else if (cJSON_IsNumber(id)) {
        int n = id->valueint;
        if (n == 1) {
            const char *en1 = str_at(result, 1);
            const cJSON *size = cJSON_GetArrayItem(result, 2);
            if (en1 && strlen(en1) < sizeof(extranonce1) && cJSON_IsNumber(size)) {
                strcpy(extranonce1, en1);
                en2_size = (size_t)size->valueint;
            } else {
                ESP_LOGE(TAG, "subscribe failed");
            }
        } else if (n == 3) {
            if (!cJSON_IsTrue(result)) ESP_LOGE(TAG, "pool rejected the worker name");
        } else if (n >= SUBMIT_ID_BASE) {
            bool ok = cJSON_IsTrue(result);
            state_lock();
            if (ok) g_state.shares_ok++;
            else g_state.shares_bad++;
            state_unlock();
            if (!ok) ESP_LOGW(TAG, "share rejected: %s", text);
        }
    }
    cJSON_Delete(msg);
}

static int pool_connect(void)
{
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res;
    char port[8];
    snprintf(port, sizeof(port), "%u", g_settings.pool_port);
    if (getaddrinfo(g_settings.pool_host, port, &hints, &res) != 0 || !res) return -1;

    int s = socket(res->ai_family, res->ai_socktype, 0);
    if (s >= 0 && connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        close(s);
        s = -1;
    }
    freeaddrinfo(res);
    if (s >= 0) {
        struct timeval tv = {.tv_sec = IDLE_TIMEOUT_S};
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    return s;
}

static void set_connected(bool up)
{
    state_lock();
    g_state.pool_connected = up;
    state_unlock();
}

/* Runs one pool session; returns when the connection is lost. */
static void session(void)
{
    char hello[512];
    size_t used = 0;

    extranonce1[0] = 0;
    snprintf(hello, sizeof(hello),
             "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"cyd-miner/%s\"]}\n"
             "{\"id\":2,\"method\":\"mining.suggest_difficulty\",\"params\":[%g]}\n"
             "{\"id\":3,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"x\"]}\n",
             esp_app_get_description()->version, (double)SUGGEST_DIFFICULTY, user);
    if (!send_line(hello)) return;
    set_connected(true);
    ESP_LOGI(TAG, "connected to %s:%u", g_settings.pool_host, g_settings.pool_port);

    for (;;) {
        if (used == LINE_BUF - 1) {
            ESP_LOGE(TAG, "line too long");
            return;
        }
        int n = recv(sock, line + used, LINE_BUF - 1 - used, 0);
        if (n <= 0) return; /* closed, error, or nothing heard for IDLE_TIMEOUT_S */
        used += n;
        line[used] = 0;

        char *start = line, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = 0;
            if (nl > start) on_message(start);
            start = nl + 1;
        }
        used -= start - line;
        memmove(line, start, used);
    }
}

static void stratum_task(void *arg)
{
    for (;;) {
        state_lock();
        bool wifi = g_state.wifi_connected;
        state_unlock();

        int s = wifi ? pool_connect() : -1;
        if (s >= 0) {
            xSemaphoreTake(tx_lock, portMAX_DELAY);
            sock = s;
            xSemaphoreGive(tx_lock);

            session();

            xSemaphoreTake(tx_lock, portMAX_DELAY);
            sock = -1;
            xSemaphoreGive(tx_lock);
            close(s);

            xSemaphoreTake(job_lock, portMAX_DELAY);
            have_job = false;
            generation++;
            xSemaphoreGive(job_lock);
            set_connected(false);
            ESP_LOGW(TAG, "pool connection lost");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void stratum_start(void)
{
    strcpy(user, g_settings.btc_address);
    if (g_settings.worker[0]) {
        strcat(user, ".");
        strcat(user, g_settings.worker);
    }
    tx_lock = xSemaphoreCreateMutex();
    job_lock = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(stratum_task, "stratum", 6144, NULL, 5, NULL, 0);
}
