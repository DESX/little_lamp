#include "esp_check.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bulb.h"
#include "button.h"
#include "commissioning.h"
#include "console.h"
#include "log.h"
#include "rtc.h"
#include "schedule.h"
#include "state.h"
#include "zb.h"

// ── The system's long-lived state ───────────────────────────────────────────

static log_t            g_log;
static button_t         g_button;
static bulb_t           g_bulb;
static state_machine_t  g_state;
static commissioning_t  g_commissioning;

static uint32_t      g_press_count = 0;
static TaskHandle_t  g_press_task  = NULL;

// ── Status banner ───────────────────────────────────────────────────────────

void show_status_banner(void) {
    const char *night_str = "?";
    if (rtc_is_set()) {
        night_str = schedule_is_night(rtc_now()) ? "yes" : "no";
    }
    LAMP_UI("Button: %s | Bulb: %s | State: %s | Night: %s",
            button_is_known(&g_button) ? "PAIRED" : "no",
            bulb_is_known(&g_bulb)     ? "PAIRED" : "no",
            state_get(&g_state) == LAMP_STATE_ON ? "ON" : "OFF",
            night_str);
    if (!button_is_known(&g_button)) {
        LAMP_UI("Waiting for button pairing...");
    } else if (!bulb_is_known(&g_bulb)) {
        LAMP_UI("Waiting for bulb pairing...");
    }
}

// ── Press dispatch ──────────────────────────────────────────────────────────

static void press_dispatch_task(void *pv) {
    (void)pv;
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        state_handle_button_press(&g_state, &g_bulb);
    }
}

void on_button_press_event(void) {
    g_press_count++;
    LAMP_UI("button press %lu", (unsigned long)g_press_count);
    if (g_press_task) xTaskNotifyGive(g_press_task);
}

// ── Schedule boundary task ──────────────────────────────────────────────────

static void boundary_task(void *pv) {
    (void)pv;
    for (;;) {
        time_t now      = rtc_now();
        time_t next     = schedule_next_boundary(now);
        time_t delay_s  = next > now ? next - now : 1;
        LAMP_LOGI("boundary: next fire in %llds (epoch=%lld, night=%d)",
                  (long long)delay_s, (long long)next, schedule_is_night(now));
        vTaskDelay(pdMS_TO_TICKS((uint32_t)delay_s * 1000));
        state_handle_boundary_cross(&g_state, &g_bulb);
    }
}

// ── Boot ────────────────────────────────────────────────────────────────────

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    log_init(&g_log);
    log_drainer_start(&g_log);

    schedule_init();
    rtc_init();
    bulb_init(&g_bulb);
    button_init(&g_button);
    state_init(&g_state);
    commissioning_init(&g_commissioning, &g_button, &g_bulb, show_status_banner);

    show_status_banner();

    console_init(&g_log, &g_commissioning, &g_button);
    zigbee_start(&g_commissioning);

    xTaskCreate(boundary_task,       "boundary", 2048, NULL, 4, NULL);
    xTaskCreate(press_dispatch_task, "press",    4096, NULL, 6, &g_press_task);

    LAMP_LOGI("─── boot complete ───");
}
