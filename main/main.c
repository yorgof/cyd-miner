#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "app.h"

state_t g_state;
static SemaphoreHandle_t state_mutex;

void state_lock(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
}

void state_unlock(void)
{
    xSemaphoreGive(state_mutex);
}

void app_main(void)
{
    state_mutex = xSemaphoreCreateMutex();
#ifdef SHA_BENCH
    bench_run();
#endif
    /* setup mode: until there are settings, and when the owner asks for it by holding the screen */
    bool asked = settings_take_setup_request();
    g_state.setup = !settings_load() || asked;
    lcd_init();
    net_start();
    ui_start();
    if (g_state.setup) return;
    stratum_start();
    stats_start();
#ifndef SHA_BENCH
    miner_start();
#endif
}
