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

// ── Behaviour configuration ─────────────────────────────────────────────────

static const struct {
    const char *tz;                 // POSIX TZ string (newlib handles DST)
    uint16_t    night_start_min;    // local minutes-of-day where night begins
    uint16_t    night_end_min;      // local minutes-of-day where night ends
} g_schedule_cfg = {
    .tz              = "EST5EDT,M3.2.0,M11.1.0",
    .night_start_min = 19*60 + 30,  // 19:30
    .night_end_min   =  6*60 + 40,  // 06:40
};

static const struct {
    uint16_t join_window_secs;      // how long pairing stays open
} g_runtime_cfg = {
    .join_window_secs = 180,
};

// The three visual states the lamp can be in. The bulb knows how to
// reach any of them from any other; main only needs to pick which one
// matches the current user state and time of day.
static const bulb_target_t BULB_DARK = {
    .kind = BULB_TARGET_OFF,
};
static const bulb_target_t BULB_DIM_RED = {
    .kind        = BULB_TARGET_XY,
    .level       = 5,                              // ~2%
    .color_x     = (uint16_t)(0.675 * 65535),      // CIE 1931 deep red
    .color_y     = (uint16_t)(0.322 * 65535),
};
static const bulb_target_t BULB_WARM_BRIGHT = {
    .kind        = BULB_TARGET_TEMP,
    .level       = 254,                            // 100%
    .temp_mireds = 370,                            // ~2700 K
};

// ── The system's long-lived state ───────────────────────────────────────────

static log_t            g_log;
static button_t         g_button;
static bulb_t           g_bulb;
static state_machine_t  g_state;
static commissioning_t  g_commissioning;

static uint32_t      g_press_count = 0;
static TaskHandle_t  g_press_task  = NULL;

// ── The user story ──────────────────────────────────────────────────────────
//
//   ON              → warm bright
//   OFF, daytime    → dark
//   OFF, nighttime  → dim red night-light
//
// The bulb owns the "how to get there" sequencing.

static void render(lamp_state_t s) {
    const bulb_target_t *target;
    if (s == LAMP_STATE_ON) {
        target = &BULB_WARM_BRIGHT;
    } else if (schedule_is_night(rtc_now())) {
        target = &BULB_DIM_RED;
    } else {
        target = &BULB_DARK;
    }
    bulb_apply(&g_bulb, target);
}

static void on_button_press(void) {
    state_toggle(&g_state);
    render(state_get(&g_state));
}

static void on_schedule_boundary(void) {
    if (state_get(&g_state) == LAMP_STATE_OFF) {
        render(state_get(&g_state));
    }
}

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

// ── Hub plumbing (agnostic of the policy above) ─────────────────────────────

static void press_dispatch_task(void *pv) {
    (void)pv;
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        on_button_press();
    }
}

// Called from the zb stack handler. Keep short.
void on_button_press_event(void) {
    g_press_count++;
    LAMP_UI("button press %lu", (unsigned long)g_press_count);
    if (g_press_task) xTaskNotifyGive(g_press_task);
}

static void boundary_task(void *pv) {
    (void)pv;
    for (;;) {
        time_t now      = rtc_now();
        time_t next     = schedule_next_boundary(now);
        time_t delay_s  = next > now ? next - now : 1;
        LAMP_LOGI("boundary: next fire in %llds (epoch=%lld, night=%d)",
                  (long long)delay_s, (long long)next, schedule_is_night(now));
        vTaskDelay(pdMS_TO_TICKS((uint32_t)delay_s * 1000));
        on_schedule_boundary();
    }
}

// ── Boot ────────────────────────────────────────────────────────────────────

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    log_init(&g_log);
    log_drainer_start(&g_log);

    schedule_init(g_schedule_cfg.tz,
                  g_schedule_cfg.night_start_min,
                  g_schedule_cfg.night_end_min);
    rtc_init();
    bulb_init(&g_bulb);
    button_init(&g_button);
    state_init(&g_state);
    commissioning_init(&g_commissioning, &g_button, &g_bulb, show_status_banner);

    show_status_banner();

    console_init(&g_log, &g_commissioning, &g_button);
    zigbee_start(&g_commissioning, g_runtime_cfg.join_window_secs);

    xTaskCreate(boundary_task,       "boundary", 2048, NULL, 4, NULL);
    xTaskCreate(press_dispatch_task, "press",    4096, NULL, 6, &g_press_task);

    LAMP_LOGI("─── boot complete ───");
}
