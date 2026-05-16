#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "bulb.h"
#include "button.h"
#include "commissioning.h"
#include "rtc.h"
#include "schedule.h"
#include "state.h"

#define NVS_NAMESPACE  "lamp"
#define NVS_KEY_VERBOSE "verbose"

// LAMP_UI ring: producers (any task) format a line and enqueue. The drainer
// task pulls from the queue and writes to stdout. If the queue is full, the
// line is dropped — the Zigbee task must never block on USB-Serial-JTAG
// back-pressure.
#define UI_LINE_MAX     128
#define UI_QUEUE_DEPTH  32
static QueueHandle_t s_ui_queue = NULL;

void lamp_ui_print(const char *fmt, ...) {
    char buf[UI_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    if (s_ui_queue == NULL) {
        // Drainer task not running yet (early boot). Fall back to direct
        // blocking write so early-boot banners still appear.
        fputs(buf, stdout);
        fflush(stdout);
        return;
    }
    // 0 timeout = non-blocking. Drops line if queue is full.
    xQueueSend(s_ui_queue, buf, 0);
}

static void log_drainer_task(void *pv) {
    (void)pv;
    char buf[UI_LINE_MAX];
    for (;;) {
        if (xQueueReceive(s_ui_queue, buf, portMAX_DELAY) == pdTRUE) {
            fputs(buf, stdout);
            fflush(stdout);
        }
    }
}

void log_drainer_start(void) {
    if (s_ui_queue != NULL) return;  // already started
    s_ui_queue = xQueueCreate(UI_QUEUE_DEPTH, UI_LINE_MAX);
    xTaskCreate(log_drainer_task, "lamp_ui", 2560, NULL, 1, NULL);
}

static bool s_verbose = false;

static void apply_log_filter(void) {
    // Non-verbose: everything below ERROR is silenced so the only thing
    // that hits the console is LAMP_UI lines (printf, bypasses esp_log)
    // and real errors. The ESP-IDF + ESP-Zigbee WARN-level chatter
    // (sleepy-end-device retries, neighbor-table churn, etc.) was
    // drowning the button-press output during rapid-tap tests.
    // Verbose: keep WARN for the rest of the system so you can see what
    // the Zigbee stack is complaining about, and surface our own INFO.
    esp_log_level_set("*", s_verbose ? ESP_LOG_WARN : ESP_LOG_ERROR);
    esp_log_level_set(LAMP_LOG_TAG, s_verbose ? ESP_LOG_INFO : ESP_LOG_NONE);
}

void log_init(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_VERBOSE, &v) == ESP_OK) {
            s_verbose = (v == 1);
        }
        nvs_close(h);
    }
    apply_log_filter();
}

void log_set_verbose(bool v) {
    s_verbose = v;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_VERBOSE, v ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    apply_log_filter();
    LAMP_UI("Verbose mode: %s", v ? "ON" : "OFF");
}

bool log_is_verbose(void) {
    return s_verbose;
}

void ui_banner(void) {
    const char *night_str = "?";
    if (rtc_is_set()) {
        night_str = schedule_is_night(rtc_now()) ? "yes" : "no";
    }
    LAMP_UI("Button: %s | Bulb: %s | State: %s | Night: %s",
            button_is_known() ? "PAIRED" : "no",
            bulb_is_known()   ? "PAIRED" : "no",
            state_get() == LAMP_STATE_ON ? "ON" : "OFF",
            night_str);
    if (!button_is_known()) {
        LAMP_UI("Waiting for button pairing...");
    } else if (!bulb_is_known()) {
        LAMP_UI("Waiting for bulb pairing...");
    }
}
