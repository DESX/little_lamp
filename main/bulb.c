#include "bulb.h"

#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "log.h"

// Constants from USER_STORY.md
#define LEVEL_ON                254
#define LEVEL_NIGHT             5
#define COLOR_TEMP_WARM_MIREDS  370
#define COLOR_X_RED             ((uint16_t)(0.675 * 65535))
#define COLOR_Y_RED             ((uint16_t)(0.322 * 65535))

#define COORDINATOR_EP          1

void bulb_init(bulb_t *b) {
    b->short_addr = 0xFFFE;
    b->endpoint   = 0;
    b->known      = false;
    b->last_on    = false;
    b->last_color = BULB_COLOR_UNKNOWN;
    b->last_level = 0xFF;
}

void bulb_set_address(bulb_t *b, uint16_t short_addr, uint8_t endpoint) {
    b->short_addr = short_addr;
    b->endpoint   = endpoint;
    b->known      = true;
    LAMP_LOGI("bulb: address set short=0x%04x ep=%u", b->short_addr, b->endpoint);
}

bool bulb_is_known(const bulb_t *b) { return b->known; }

static esp_zb_zcl_basic_cmd_t mk_addr(const bulb_t *b) {
    esp_zb_zcl_basic_cmd_t a = {0};
    a.dst_addr_u.addr_short = b->short_addr;
    a.dst_endpoint = b->endpoint;
    a.src_endpoint = COORDINATOR_EP;
    return a;
}

static void send_on_off(const bulb_t *b, bool on) {
    esp_zb_zcl_on_off_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr(b);
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
                           : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void send_level(const bulb_t *b, uint8_t level, uint16_t transition_tenths) {
    esp_zb_zcl_move_to_level_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr(b);
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.level         = level;
    cmd.transition_time = transition_tenths;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_level_move_to_level_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void send_color_temp(const bulb_t *b, uint16_t mireds, uint16_t transition_tenths) {
    esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr(b);
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.color_temperature = mireds;
    cmd.transition_time   = transition_tenths;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void send_color_xy(const bulb_t *b, uint16_t x, uint16_t y, uint16_t transition_tenths) {
    esp_zb_zcl_color_move_to_color_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr(b);
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.color_x       = x;
    cmd.color_y       = y;
    cmd.transition_time = transition_tenths;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_color_move_to_color_cmd_req(&cmd);
    esp_zb_lock_release();
}

void bulb_command_on_warm(bulb_t *b, uint16_t transition_tenths) {
    if (!b->known) { LAMP_UI("bulb: ON skipped — bulb not paired"); return; }
    int sent = 0;
    if (b->last_color != BULB_COLOR_WARM) {
        send_color_temp(b, COLOR_TEMP_WARM_MIREDS, transition_tenths);
        b->last_color = BULB_COLOR_WARM;
        sent++;
    }
    if (b->last_level != LEVEL_ON) {
        send_level(b, LEVEL_ON, transition_tenths);
        b->last_level = LEVEL_ON;
        sent++;
    }
    if (!b->last_on) {
        send_on_off(b, true);
        b->last_on = true;
        sent++;
    }
    LAMP_LOGI("bulb: ON warm-white, level=%u (sent %d cmd%s)",
              LEVEL_ON, sent, sent == 1 ? "" : "s");
}

void bulb_command_off(bulb_t *b, uint16_t transition_tenths) {
    (void)transition_tenths;
    if (!b->known) { LAMP_UI("bulb: OFF skipped — bulb not paired"); return; }
    if (b->last_on) {
        send_on_off(b, false);
        b->last_on = false;
        LAMP_LOGI("bulb: OFF (sent 1 cmd)");
    } else {
        LAMP_LOGI("bulb: OFF (already off, no-op)");
    }
}

void bulb_command_dim_red(bulb_t *b, uint16_t transition_tenths) {
    if (!b->known) { LAMP_UI("bulb: DIM-RED skipped — bulb not paired"); return; }
    int sent = 0;
    if (b->last_color != BULB_COLOR_RED) {
        send_color_xy(b, COLOR_X_RED, COLOR_Y_RED, transition_tenths);
        b->last_color = BULB_COLOR_RED;
        sent++;
    }
    if (b->last_level != LEVEL_NIGHT) {
        send_level(b, LEVEL_NIGHT, transition_tenths);
        b->last_level = LEVEL_NIGHT;
        sent++;
    }
    if (!b->last_on) {
        send_on_off(b, true);
        b->last_on = true;
        sent++;
    }
    LAMP_LOGI("bulb: dim red, level=%u (sent %d cmd%s)",
              LEVEL_NIGHT, sent, sent == 1 ? "" : "s");
}
