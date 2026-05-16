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

#define LAMP_UI(fmt, ...) do { \
    printf(fmt "\n", ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)

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
