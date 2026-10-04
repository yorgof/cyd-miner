/*
 * On-device bench for the hardware SHA loops, built instead of the miner with
 * idf.py -DSHA_BENCH=1 (and -DSHA_BENCH=0 to get the miner back).
 *
 * Every loop hashes the same nonces in chunks, and every chunk is checked
 * against software: the sum of the digest bits the loop looked at, and where
 * it reported candidates. A chunk with one wrong hash counts as wrong. The
 * loops run first with core 0 idle, then with core 0 hammering the buses the
 * SHA registers share, then next to the rest of the firmware. One line each:
 *
 *   <core 0 load> <loop>  <best cycles per nonce>  <kH/s on average>  <wrong chunks>
 *
 * The firmware phase repeats until reset. It is the one that counts: a loop
 * is worth its wrong chunks if it still comes out ahead there.
 */
#include <stdio.h>
#include <string.h>
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/dport_reg.h"
#include "soc/hwcrypto_reg.h"
#include "soc/uart_reg.h"
#include "app.h"
#include "sha256.h"

#define CHUNK 128
#define NCHUNK 1024
#define W3_FIRST 0x1000
#define FIRMWARE_PASSES 32

#define TEXT ((volatile uint32_t *)SHA_TEXT_BASE)
#define REG(addr) (*(volatile uint32_t *)(addr))

typedef uint32_t (*scan_fn)(volatile uint32_t *text, sha_scan_t *ctx);

static uint32_t hdr[19], mid[8];
static uint32_t ref_sum[NCHUNK], ref_cand[NCHUNK];

static inline void run(uint32_t reg)
{
    REG(reg) = 1;
    while (REG(SHA_256_BUSY_REG)) {
    }
}

/* What the firmware did before the overlapped loops: fill, trigger, poll, read the whole digest. */
static uint32_t polled(volatile uint32_t *text, sha_scan_t *c)
{
    uint32_t digest[8];

    while (c->count) {
        for (int i = 0; i < 16; i++) text[i] = c->block1[i];
        run(SHA_256_START_REG);

        text[0] = c->tail[0]; text[1] = c->tail[1]; text[2] = c->tail[2]; text[3] = c->w3;
        text[4] = 0x80000000;
        for (int i = 5; i < 15; i++) text[i] = 0;
        text[15] = 640;
        run(SHA_256_CONTINUE_REG);
        run(SHA_256_LOAD_REG);

        text[8] = 0x80000000;
        for (int i = 9; i < 15; i++) text[i] = 0;
        text[15] = 256;
        run(SHA_256_START_REG);
        run(SHA_256_LOAD_REG);
        for (int i = 0; i < 8; i++) digest[i] = text[i];

        uint32_t low = digest[7] & 0xFFFF;
        c->w3++;
        c->count--;
        c->sum += low;
        if (!low) return 1;
    }
    return 0;
}

uint32_t sha_hw_scan_fast(volatile uint32_t *text, sha_scan_t *ctx);
uint32_t sha_hw_scan_safe(volatile uint32_t *text, sha_scan_t *ctx);
#define V(name, ...) uint32_t name(volatile uint32_t *text, sha_scan_t *ctx);
#include "bench_variants.def"
#undef V

static const struct {
    const char *name;
    scan_fn fn;
} loops[] = {
    {"polled", polled},
    {"fast", sha_hw_scan_fast},
    {"safe", sha_hw_scan_safe},
#define V(name, ...) {#name, name},
#include "bench_variants.def"
#undef V
};
#define NLOOP ((int)(sizeof(loops) / sizeof(loops[0])))

/* ---- load on core 0 ---- */

enum { QUIET, DPORT_READ, AES_WRITE, APB_READ, SW_SHA, FIRMWARE };
static const char *const load_name[] = {"quiet", "dport-rd", "aes-wr", "apb-rd", "sw-sha", "firmware"};
static volatile int load;
static volatile uint32_t sink;

static void load_task(void *arg)
{
    uint32_t digest[8], w3 = 0;

    for (;;) {
        int m = load;
        TickType_t t0 = xTaskGetTickCount();
        while (m != QUIET && load == m && xTaskGetTickCount() - t0 < pdMS_TO_TICKS(500)) {
            for (int i = 0; i < 256; i++) {
                if (m == DPORT_READ) sink = REG(DPORT_DATE_REG);
                else if (m == AES_WRITE) REG(AES_TEXT_BASE) = i;
                else if (m == APB_READ) sink = REG(UART_STATUS_REG(0));
                else sink = sha256_mine(mid, hdr + 16, w3++, digest);
            }
        }
        vTaskDelay(m == QUIET ? pdMS_TO_TICKS(50) : 1);
    }
}

/* ---- measurement ---- */

typedef struct {
    uint32_t best, wrong, chunks;
    uint64_t total;
} result_t;

static void measure(scan_fn fn, result_t *r)
{
    sha_scan_t c = {.pad = 0x80000000};

    memcpy(c.block1, hdr, sizeof(c.block1));
    memcpy(c.tail, hdr + 16, sizeof(c.tail));
    /* what a call costs outside its loop; an interrupt can only make it look longer */
    uint32_t t0, call = UINT32_MAX;
    for (int i = 0; i < 3; i++) {
        t0 = esp_cpu_get_cycle_count();
        fn(TEXT, &c);
        uint32_t dt = esp_cpu_get_cycle_count() - t0;
        if (dt < call) call = dt;
    }

    for (int k = 0; k < NCHUNK; k++) {
        uint32_t first = W3_FIRST + k * CHUNK, cand = 0;
        c.w3 = first;
        c.count = CHUNK;
        c.sum = 0;
        t0 = esp_cpu_get_cycle_count();
        while (fn(TEXT, &c)) cand += c.w3 - first;
        uint32_t dt = esp_cpu_get_cycle_count() - t0 - call;
        if (c.sum != ref_sum[k] || cand != ref_cand[k] || c.w3 != first + CHUNK) r->wrong++;
        if (dt < r->best) r->best = dt;
        r->total += dt;
        r->chunks++;
    }
    vTaskDelay(1);
}

static void print(int v, const result_t *r)
{
    unsigned best = (unsigned)((uint64_t)r->best * 100 / CHUNK);
    printf("%-9s %-12s %3u.%02u cycles %5u kH/s  wrong %u/%u\n", load_name[load], loops[v].name, best / 100, best % 100,
           (unsigned)(240000ull * CHUNK * r->chunks / r->total), (unsigned)r->wrong, (unsigned)r->chunks);
}

static SemaphoreHandle_t firmware_go;

static void bench_task(void *arg)
{
    static result_t r[NLOOP];
    uint32_t digest[8];
    const char *selected = sha_hw_select();

    printf("\nself-test picks: %s\n", selected ? selected : "none");
    for (int i = 0; i < 19; i++) hdr[i] = 0x9E3779B9u * (i + 1);
    sha256_init(mid);
    sha256_transform(mid, hdr);

    TickType_t t0 = xTaskGetTickCount();
    for (int k = 0; k < NCHUNK; k++) {
        for (uint32_t i = 0; i < CHUNK; i++) {
            uint32_t low = sha256_mine(mid, hdr + 16, W3_FIRST + k * CHUNK + i, digest) & 0xFFFF;
            ref_sum[k] += low;
            if (!low) ref_cand[k] += i + 1;
        }
        if (k % 32 == 31) vTaskDelay(1);
    }
    printf("software: %u kH/s\n", (unsigned)(CHUNK * NCHUNK / ((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS)));

    for (load = QUIET; load < FIRMWARE; load++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        for (int v = 0; v < NLOOP; v++) {
            r[v] = (result_t){.best = UINT32_MAX};
            measure(loops[v].fn, &r[v]);
            print(v, &r[v]);
        }
    }

    /* the rest of the firmware starts on core 0, with a software miner as on the real thing */
    xSemaphoreGive(firmware_go);
    for (int i = 0; i < 300 && !g_state.pool_connected; i++) vTaskDelay(pdMS_TO_TICKS(100));
    printf("pool %s\n", g_state.pool_connected ? "connected" : "NOT connected");
    for (;;) {
        for (int v = 0; v < NLOOP; v++) r[v] = (result_t){.best = UINT32_MAX};
        /* one pass per loop in turn, so that a burst on core 0 does not land on a single loop */
        for (int p = 0; p < FIRMWARE_PASSES; p++) {
            for (int v = 1; v < NLOOP; v++) measure(loops[v].fn, &r[v]);
        }
        for (int v = 1; v < NLOOP; v++) print(v, &r[v]);
    }
}

/* Runs the bench up to its firmware phase, then returns for app_main to start the firmware. */
void bench_run(void)
{
    firmware_go = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(load_task, "load", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, 1, NULL, 1);
    xSemaphoreTake(firmware_go, portMAX_DELAY);
}
