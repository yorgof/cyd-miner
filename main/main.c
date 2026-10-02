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
    lcd_init();
    ui_start();
    net_start();
    stratum_start();
    stats_start();
    miner_start();
}
