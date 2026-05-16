// main.c — the architectural overview of the lamp.
//
// What this system does, in one diagram:
//
//     ┌──────────┐  press   ┌──────────────┐  notify  ┌──────────────────┐
//     │  Button  ├─────────▶│ Zigbee stack │─────────▶│ press_dispatch   │
//     └──────────┘   RF     │   (zb.c)     │  queue   │     _task        │
//                           └──────────────┘          └────────┬─────────┘
//                                                              │
//                                                  state_handle_button_press()
//                                                              │
//                                                              ▼
//                                                   ┌────────────────────┐
//     ┌──────────┐  ZCL cmd  ┌────────────┐ render() │  state machine    │
//     │   Bulb   │◀──────────│   bulb.c   │◀─────────│  (state.c)        │
//     └──────────┘     RF    └────────────┘          └────────────────────┘
//
// Three long-lived tasks make up the system:
//
//   zigbee_task            (priority 5)  — ZBOSS event loop. Receives radio
//                                          frames; invokes the APS handler
//                                          in zb.c, which fires
//                                          on_button_press_event() for any
//                                          frame we classify as a press.
//   press_dispatch_task    (priority 6)  — woken by a FreeRTOS task notify
//                                          from on_button_press_event().
//                                          Runs the state machine and sends
//                                          bulb commands. Above the Zigbee
//                                          task in priority so it grabs the
//                                          stack lock the moment ZBOSS
//                                          releases it.
//   boundary_task          (priority 4)  — sleeps until the next schedule
//                                          boundary (06:40 / 19:30) and
//                                          re-renders the bulb if the lamp
//                                          is currently OFF.
//
// LAMP_UI(...) prints curated user-facing lines through a ring buffer drained
// by yet another low-priority task (log.c). No producer ever blocks on the
// USB serial port.
//
// To change WHAT the system does, edit this file. To change HOW it speaks
// Zigbee, edit zb.c. To change device-specific behaviors, edit the
// appropriate file in devices/ + the matching module.

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

// ── Press dispatch ──────────────────────────────────────────────────────────
// on_button_press_event is the only callback zb.c needs from us. It runs
// inside the Zigbee task's APS context, so it must do almost nothing.
// We count the press, print a non-blocking UI line, and wake the dispatch
// task — which is what actually drives the state machine.

static uint32_t      s_press_count = 0;
static TaskHandle_t  s_press_task  = NULL;

static void press_dispatch_task(void *pv) {
    (void)pv;
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        state_handle_button_press();
    }
}

void on_button_press_event(void) {
    s_press_count++;
    LAMP_UI("button press %lu", (unsigned long)s_press_count);
    if (s_press_task != NULL) {
        xTaskNotifyGive(s_press_task);
    }
}

// ── Schedule boundary task ──────────────────────────────────────────────────
// Sleeps until the next 06:40 or 19:30 boundary, then nudges the state
// machine. The state machine itself decides whether to re-render the bulb
// (only when the lamp is OFF — ON is never disturbed by the clock).

static void boundary_task(void *pv) {
    (void)pv;
    for (;;) {
        time_t now = rtc_now();
        time_t next = schedule_next_boundary(now);
        time_t delay_s = next - now;
        if (delay_s < 1) delay_s = 1;
        LAMP_LOGI("boundary: next fire in %llds (epoch=%lld, night=%d)",
                  (long long)delay_s, (long long)next, schedule_is_night(now));
        vTaskDelay(pdMS_TO_TICKS((uint32_t)delay_s * 1000));
        state_handle_boundary_cross();
    }
}

// ── Boot ────────────────────────────────────────────────────────────────────

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());

    log_init();
    log_drainer_start();   // LAMP_UI now flows through a queue, not direct printf.

    schedule_init();
    rtc_init();
    bulb_init();
    button_init();
    state_init();
    commissioning_init();

    ui_banner();

    console_init();
    zigbee_start();

    xTaskCreate(boundary_task,       "boundary", 2048, NULL, 4, NULL);
    xTaskCreate(press_dispatch_task, "press",    4096, NULL, 6, &s_press_task);

    LAMP_LOGI("─── boot complete ───");
}
