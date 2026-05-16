#include "log.h"

#include <stdbool.h>

#include "nvs.h"

#include "bulb.h"
#include "button.h"
#include "commissioning.h"
#include "rtc.h"
#include "schedule.h"
#include "state.h"

#define NVS_NAMESPACE  "lamp"
#define NVS_KEY_VERBOSE "verbose"

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
