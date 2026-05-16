#include "button.h"

#include "esp_zigbee_core.h"

#include "log.h"
#include "state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint16_t s_short_addr = 0xFFFE;
static uint8_t  s_endpoint   = 0;
static bool     s_known      = false;

// De-bounce against the button's own retransmissions of the same press.
#define DEBOUNCE_MS 200
static TickType_t s_last_press_tick = 0;

void button_init(void) {
    s_known = false;
}

void button_set_address(uint16_t short_addr, uint8_t endpoint) {
    s_short_addr = short_addr;
    s_endpoint   = endpoint;
    s_known      = true;
    LAMP_LOGI("button: address set short=0x%04x ep=%u", s_short_addr, s_endpoint);
}

bool button_is_known(void) {
    return s_known;
}

uint16_t button_short_addr(void) {
    return s_short_addr;
}

uint8_t button_endpoint(void) {
    return s_endpoint;
}

bool button_dispatch_on_off(uint16_t src_addr, uint8_t src_endpoint, uint8_t cmd_id) {
    if (!s_known) {
        LAMP_LOGW("button: on/off from 0x%04x ep=%u cmd=0x%02x (no button paired yet)",
                  src_addr, src_endpoint, cmd_id);
        return false;
    }
    if (src_addr != s_short_addr) {
        return false;
    }
    // Treat ON, OFF, and TOGGLE all as "single press."
    if (cmd_id != ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID &&
        cmd_id != ESP_ZB_ZCL_CMD_ON_OFF_ON_ID &&
        cmd_id != ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID) {
        return false;
    }
    TickType_t now = xTaskGetTickCount();
    TickType_t since = (now - s_last_press_tick) * portTICK_PERIOD_MS;
    if (s_last_press_tick != 0 && since < DEBOUNCE_MS) {
        LAMP_LOGD("button: press ignored (debounce, %ums since last)", (unsigned)since);
        return true;
    }
    s_last_press_tick = now;
    LAMP_LOGI("button: press src=0x%04x cmd=0x%02x", src_addr, cmd_id);
    state_handle_button_press();
    return true;
}
