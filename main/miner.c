/*
 * Hashing tasks, one per CPU core. Core 1 drives the hardware SHA engine when
 * one of its loops passes a self-test; core 0 hashes in software alongside
 * WiFi and the UI.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "sha256.h"

static const char *TAG = "miner";

/* Unconfirmed candidates out of HEALTH_WINDOW that make the miner give up on a hardware loop. */
#define HEALTH_WINDOW 256
#define HEALTH_LIMIT 8
/* How often the hash rates go to the log. */
#define REPORT_SECONDS 60

/*
 * A mining task stops for one tick this often, in seconds per core, to let the
 * idle task feed the task watchdog, which allows it 5 s. Core 0 has to do so
 * more often: there the stats task's TLS handshakes can take the tick instead.
 */
static const uint8_t rest_seconds[2] = {1, 3};

static uint32_t hw_candidates, hw_wrong;

/* Records a hash that got through the first filter and submits it if it is a share. */
static void candidate(const work_t *work, uint32_t w3, const uint32_t digest[8], double difficulty)
{
    double d = digest_difficulty(digest);
    state_lock();
    if (d > g_state.best_diff) g_state.best_diff = d;
    state_unlock();
    if (d >= difficulty) {
        ESP_LOGI(TAG, "share found, difficulty %g", d);
        stratum_submit(work, __builtin_bswap32(w3));
    }
}

/*
 * The hardware loop only says that a hash had 16 zero bits, which every share
 * at difficulty 1/65536 or more has. The hash is then computed again in
 * software, so a wrong one from the hardware is never submitted, and the share
 * of candidates that do not check out says how many hashes the loop gets wrong.
 */
static bool confirm(const work_t *work, uint32_t w3, double difficulty)
{
    static uint32_t seen, unconfirmed;
    uint32_t digest[8];
    bool ok = (sha256_mine(work->midstate, work->tail, w3, digest) & 0xFFFF) == 0;

    hw_candidates++;
    if (ok) {
        candidate(work, w3, digest, difficulty);
    } else {
        hw_wrong++;
        unconfirmed++;
    }
    if (++seen < HEALTH_WINDOW) return true;

    bool healthy = unconfirmed < HEALTH_LIMIT;
    if (!healthy) ESP_LOGW(TAG, "%u of %u hardware candidates were wrong", (unsigned)unconfirmed, (unsigned)seen);
    seen = unconfirmed = 0;
    return healthy;
}

/* Software and hardware rates, and the hardware loop's record. */
static void report(void)
{
    static uint32_t last[2];
    static int64_t last_time;
    int64_t now = esp_timer_get_time();
    uint32_t sw = g_state.hashes[0], hw = g_state.hashes[1];

    if (now - last_time < REPORT_SECONDS * 1000000LL) return;
    if (last_time) {
        double ms = (double)(now - last_time) / 1000;
        ESP_LOGI(TAG, "%.1f + %.1f kH/s, %u hardware candidates, %u wrong", (double)(uint32_t)(sw - last[0]) / ms,
                 (double)(uint32_t)(hw - last[1]) / ms, (unsigned)hw_candidates, (unsigned)hw_wrong);
    }
    last[0] = sw;
    last[1] = hw;
    last_time = now;
}

static void miner_task(void *arg)
{
    int id = (int)(intptr_t)arg;
    const char *hw = NULL;
    work_t work;
    uint32_t gen, digest[8];
    double difficulty;
    TickType_t last_rest = xTaskGetTickCount();

    if (id == 1) {
        hw = sha_hw_select();
        if (hw) ESP_LOGI(TAG, "hardware SHA on core 1, %s loop", hw);
        else ESP_LOGW(TAG, "hardware SHA self-test failed, using software");
    }

    for (;;) {
        if (!stratum_get_work(&gen, &work, &difficulty)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint32_t filter = difficulty_filter(difficulty);
        sha_scan_t scan = {.pad = 0x80000000};
        memcpy(scan.block1, work.block1, sizeof(scan.block1));
        memcpy(scan.tail, work.tail, sizeof(scan.tail));
        do {
            if (hw) {
                scan.count = 0x1000;
                while (sha_hw_scan(&scan)) {
                    if (confirm(&work, scan.w3 - 1, difficulty)) continue;
                    hw = sha_hw_step_down();
                    ESP_LOGW(TAG, "switching to %s%s", hw ? "the hardware loop: " : "software", hw ? hw : "");
                    if (!hw) break;
                }
                /* without a loop left, the software path picks up at scan.w3 */
            } else {
                do {
                    uint32_t top = sha256_mine(work.midstate, work.tail, scan.w3, digest);
                    if (__builtin_bswap32(top) <= filter) candidate(&work, scan.w3, digest, difficulty);
                } while (++scan.w3 & 0xFFF);
            }
            if ((scan.w3 & 0xFFF) == 0) g_state.hashes[id] += 0x1000;
            if (stratum_generation() != gen) break;
            /* let the idle task run every few seconds so the watchdog stays fed */
            if (xTaskGetTickCount() - last_rest >= pdMS_TO_TICKS(rest_seconds[id] * 1000)) {
                if (id == 1) report();
                vTaskDelay(1);
                last_rest = xTaskGetTickCount();
            }
        } while (scan.w3 != 0);
    }
}

void miner_start(void)
{
    xTaskCreatePinnedToCore(miner_task, "miner0", 8192, (void *)0, 1, NULL, 0);
    xTaskCreatePinnedToCore(miner_task, "miner1", 8192, (void *)1, 1, NULL, 1);
}
