#pragma once

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

// log_t is allocated in main. log.c keeps a module-internal pointer to
// the active instance — set by log_init — so the LAMP_UI / LAMP_LOG*
// macros stay zero-argument at hundreds of call sites. The struct is
// the source of truth; the pointer is a convenience for the macros.
typedef struct {
    bool          verbose;
    QueueHandle_t ui_queue;
    TaskHandle_t  drainer_task;
} log_t;

#define LAMP_LOG_TAG "lamp"

#define LAMP_LOGI(...) ESP_LOGI(LAMP_LOG_TAG, __VA_ARGS__)
#define LAMP_LOGW(...) ESP_LOGW(LAMP_LOG_TAG, __VA_ARGS__)
#define LAMP_LOGE(...) ESP_LOGE(LAMP_LOG_TAG, __VA_ARGS__)
#define LAMP_LOGD(...) ESP_LOGD(LAMP_LOG_TAG, __VA_ARGS__)

#define LAMP_UI(fmt, ...) lamp_ui_print(fmt "\n", ##__VA_ARGS__)

void lamp_ui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void log_init           (log_t *log);
void log_drainer_start  (log_t *log);
void log_set_verbose    (log_t *log, bool v);
bool log_is_verbose     (const log_t *log);
