// Two distinct output channels:
//
//   LAMP_UI(...)   — clean one-liners shown unconditionally. Use for status
//                    banners and the curated events a watching human cares
//                    about (boot, pair, press).
//   LAMP_LOGI(...) — detailed diagnostic logs. Default-suppressed; the user
//                    enables them with `set-verbose 1` on the serial console.

#pragma once

#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"

#define LAMP_LOG_TAG "lamp"

#define LAMP_LOGI(...) ESP_LOGI(LAMP_LOG_TAG, __VA_ARGS__)
#define LAMP_LOGW(...) ESP_LOGW(LAMP_LOG_TAG, __VA_ARGS__)
#define LAMP_LOGE(...) ESP_LOGE(LAMP_LOG_TAG, __VA_ARGS__)
#define LAMP_LOGD(...) ESP_LOGD(LAMP_LOG_TAG, __VA_ARGS__)

// LAMP_UI is the curated-event channel. It is **non-blocking**: the caller
// formats the line, enqueues it for the drainer task, and returns. If the
// queue is full the line is dropped. This is intentional — we never want a
// printf-stalls-on-USB-Serial-JTAG-backpressure scenario to block the
// Zigbee task and cause us to lose RX frames.
#define LAMP_UI(fmt, ...) lamp_ui_print(fmt "\n", ##__VA_ARGS__)

void lamp_ui_print(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Starts the background task that drains LAMP_UI lines to stdout. Must be
// called once at boot. Before this call, lamp_ui_print() falls back to
// direct (blocking) stdout — needed so that early boot UI lines still come
// out before the task is up.
void log_drainer_start(void);

// Reads NVS-persisted verbosity and applies it to the esp_log filter.
// Must be called once at boot AFTER nvs_flash_init().
void log_init(void);

// Toggles verbose mode at runtime; persists to NVS so it survives reboot.
void log_set_verbose(bool v);

// True iff verbose mode is currently on.
bool log_is_verbose(void);

// Prints the curated status banner (button paired, bulb paired, state, night).
// Followed by a "waiting for…" line if pairing is incomplete.
void ui_banner(void);
