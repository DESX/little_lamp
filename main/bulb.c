#include "bulb.h"

#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "log.h"

// Constants from USER_STORY.md
#define LEVEL_ON         254   // 100%
#define LEVEL_NIGHT      5     // ~2%
#define COLOR_TEMP_WARM_MIREDS 370   // ~2700 K

// xy = (0.675, 0.322) deep red, in Zigbee 16-bit fixed-point (value / 65536).
#define COLOR_X_RED      ((uint16_t)(0.675 * 65535))
#define COLOR_Y_RED      ((uint16_t)(0.322 * 65535))

static uint16_t s_short_addr = 0xFFFE;   // unset
static uint8_t  s_endpoint   = 0;
static bool     s_known      = false;

// Bulb state cache. Each bulb command compares against this and skips frames
// that wouldn't change anything. Three Zigbee commands per press is the bulk
// of the latency; collapsing to one when only the on/off bit changes is the
// difference between a snappy and a sluggish-feeling button.
typedef enum {
    BULB_COLOR_UNKNOWN = 0,
    BULB_COLOR_WARM,
    BULB_COLOR_RED,
} bulb_color_mode_t;
static bool              s_last_on    = false;
static bulb_color_mode_t s_last_color = BULB_COLOR_UNKNOWN;
static uint8_t           s_last_level = 0xFF;  // sentinel "unknown"

#define COORDINATOR_EP 1

void bulb_init(void) {
    s_known = false;
}

void bulb_set_address(uint16_t short_addr, uint8_t endpoint) {
    s_short_addr = short_addr;
    s_endpoint   = endpoint;
    s_known      = true;
    LAMP_LOGI("bulb: address set short=0x%04x ep=%u", s_short_addr, s_endpoint);
}

bool bulb_is_known(void) {
    return s_known;
}

static esp_zb_zcl_basic_cmd_t mk_addr(void) {
    esp_zb_zcl_basic_cmd_t a = {0};
    a.dst_addr_u.addr_short = s_short_addr;
    a.dst_endpoint = s_endpoint;
    a.src_endpoint = COORDINATOR_EP;
    return a;
}

static void send_on_off(bool on) {
    esp_zb_zcl_on_off_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr();
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.on_off_cmd_id = on ? ESP_ZB_ZCL_CMD_ON_OFF_ON_ID
                           : ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_on_off_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void send_level(uint8_t level, uint16_t transition_tenths) {
    esp_zb_zcl_move_to_level_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr();
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.level         = level;
    cmd.transition_time = transition_tenths;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_level_move_to_level_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void send_color_temp(uint16_t mireds, uint16_t transition_tenths) {
    esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr();
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.color_temperature = mireds;
    cmd.transition_time   = transition_tenths;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void send_color_xy(uint16_t x, uint16_t y, uint16_t transition_tenths) {
    esp_zb_zcl_color_move_to_color_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr();
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.color_x       = x;
    cmd.color_y       = y;
    cmd.transition_time = transition_tenths;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_color_move_to_color_cmd_req(&cmd);
    esp_zb_lock_release();
}

void bulb_command_on_warm(uint16_t transition_tenths) {
    if (!s_known) { LAMP_LOGW("bulb: ON skipped — bulb not paired"); return; }
    int sent = 0;
    if (s_last_color != BULB_COLOR_WARM) {
        send_color_temp(COLOR_TEMP_WARM_MIREDS, transition_tenths);
        s_last_color = BULB_COLOR_WARM;
        sent++;
    }
    if (s_last_level != LEVEL_ON) {
        send_level(LEVEL_ON, transition_tenths);
        s_last_level = LEVEL_ON;
        sent++;
    }
    if (!s_last_on) {
        send_on_off(true);
        s_last_on = true;
        sent++;
    }
    LAMP_LOGI("bulb: ON warm-white, level=%u (sent %d cmd%s)",
              LEVEL_ON, sent, sent == 1 ? "" : "s");
}

void bulb_command_off(uint16_t transition_tenths) {
    (void)transition_tenths;
    if (!s_known) { LAMP_LOGW("bulb: OFF skipped — bulb not paired"); return; }
    if (s_last_on) {
        send_on_off(false);
        s_last_on = false;
        LAMP_LOGI("bulb: OFF (sent 1 cmd)");
    } else {
        LAMP_LOGI("bulb: OFF (already off, no-op)");
    }
}

void bulb_command_dim_red(uint16_t transition_tenths) {
    if (!s_known) { LAMP_LOGW("bulb: DIM-RED skipped — bulb not paired"); return; }
    int sent = 0;
    if (s_last_color != BULB_COLOR_RED) {
        send_color_xy(COLOR_X_RED, COLOR_Y_RED, transition_tenths);
        s_last_color = BULB_COLOR_RED;
        sent++;
    }
    if (s_last_level != LEVEL_NIGHT) {
        send_level(LEVEL_NIGHT, transition_tenths);
        s_last_level = LEVEL_NIGHT;
        sent++;
    }
    if (!s_last_on) {
        send_on_off(true);
        s_last_on = true;
        sent++;
    }
    LAMP_LOGI("bulb: dim red, level=%u (sent %d cmd%s)",
              LEVEL_NIGHT, sent, sent == 1 ? "" : "s");
}
