/*
 * Hashing tasks, one per CPU core. Core 1 uses the hardware SHA engine when it
 * passes a self-test; core 0 hashes in software alongside WiFi and the UI.
 */
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"
#include "sha256.h"

static const char *TAG = "miner";

/* Checks the hardware engine against the software hash on a few inputs. */
static bool hw_self_test(void)
{
    uint32_t block1[16], mid[8], tail[3] = {0x4b1e5e4a, 0x29ab5f49, 0xffff001d}, a[8], b[8];

    for (int i = 0; i < 16; i++) block1[i] = 0x01234567u * (i + 1);
    sha256_init(mid);
    sha256_transform(mid, block1);
    for (uint32_t w3 = 0; w3 < 16; w3++) {
        sha256_mine(mid, tail, w3 * 0x9e3779b9u, a);
        sha_hw_mine(block1, tail, w3 * 0x9e3779b9u, b);
        if (memcmp(a, b, sizeof(a))) return false;
    }
    return true;
}

static void miner_task(void *arg)
{
    int id = (int)(intptr_t)arg;
    bool hw = false;
    work_t work;
    uint32_t gen, digest[8];
    double difficulty;
    TickType_t last_rest = xTaskGetTickCount();

    if (id == 1) {
        sha_hw_init();
        hw = hw_self_test();
        ESP_LOGI(TAG, "hardware SHA %s", hw ? "in use on core 1" : "self-test failed, using software");
    }

    for (;;) {
        if (!stratum_get_work(&gen, &work, &difficulty)) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        uint32_t filter = difficulty_filter(difficulty);
        uint32_t w3 = 0;
        do {
            uint32_t top = hw ? sha_hw_mine(work.block1, work.tail, w3, digest)
                              : sha256_mine(work.midstate, work.tail, w3, digest);
            if (__builtin_bswap32(top) <= filter) {
                double d = digest_difficulty(digest);
                state_lock();
                if (d > g_state.best_diff) g_state.best_diff = d;
                state_unlock();
                if (d >= difficulty) {
                    ESP_LOGI(TAG, "share found, difficulty %g", d);
                    stratum_submit(&work, __builtin_bswap32(w3));
                }
            }
            w3++;
            if ((w3 & 0xFFF) == 0) {
                g_state.hashes[id] += 0x1000;
                if (stratum_generation() != gen) break;
                /* let the idle task run once a second so the watchdog stays fed */
                if (xTaskGetTickCount() - last_rest >= pdMS_TO_TICKS(1000)) {
                    vTaskDelay(1);
                    last_rest = xTaskGetTickCount();
                }
            }
        } while (w3 != 0);
    }
}

void miner_start(void)
{
    xTaskCreatePinnedToCore(miner_task, "miner0", 8192, (void *)0, 1, NULL, 0);
    xTaskCreatePinnedToCore(miner_task, "miner1", 8192, (void *)1, 1, NULL, 1);
}
