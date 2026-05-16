#include "log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <string.h>

#include "nvs.h"

#define NVS_NAMESPACE     "lamp"
#define NVS_KEY_VERBOSE   "verbose"
#define UI_LINE_MAX       128
#define UI_QUEUE_DEPTH    32

// Module-internal pointer to the log_t main allocated. The LAMP_UI macro
// has no convenient way to thread a handle through hundreds of call
// sites, so we keep this single indirection. The actual storage lives
// in main; this is just a cached pointer.
static log_t *s_active_log = NULL;

void lamp_ui_print(const char *fmt, ...) {
    char buf[UI_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    if (s_active_log == NULL || s_active_log->ui_queue == NULL) {
        fputs(buf, stdout);
        fflush(stdout);
        return;
    }
    xQueueSend(s_active_log->ui_queue, buf, 0);
}

static void apply_log_filter(const log_t *log) {
    esp_log_level_set("*", log->verbose ? ESP_LOG_WARN : ESP_LOG_ERROR);
    esp_log_level_set(LAMP_LOG_TAG, log->verbose ? ESP_LOG_INFO : ESP_LOG_NONE);
}

void log_init(log_t *log) {
    log->verbose      = false;
    log->ui_queue     = NULL;
    log->drainer_task = NULL;
    s_active_log = log;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_VERBOSE, &v) == ESP_OK) {
            log->verbose = (v == 1);
        }
        nvs_close(h);
    }
    apply_log_filter(log);
}

static void drainer_task(void *pv) {
    log_t *log = (log_t *)pv;
    char buf[UI_LINE_MAX];
    for (;;) {
        if (xQueueReceive(log->ui_queue, buf, portMAX_DELAY) == pdTRUE) {
            fputs(buf, stdout);
            fflush(stdout);
        }
    }
}

void log_drainer_start(log_t *log) {
    if (log->ui_queue != NULL) return;
    log->ui_queue = xQueueCreate(UI_QUEUE_DEPTH, UI_LINE_MAX);
    xTaskCreate(drainer_task, "lamp_ui", 2560, log, 1, &log->drainer_task);
}

void log_set_verbose(log_t *log, bool v) {
    log->verbose = v;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_VERBOSE, v ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    apply_log_filter(log);
    LAMP_UI("Verbose mode: %s", v ? "ON" : "OFF");
}

bool log_is_verbose(const log_t *log) { return log->verbose; }
