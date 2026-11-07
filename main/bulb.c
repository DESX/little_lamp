#include "bulb.h"

#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

#include "log.h"

// Protocol mechanic — which endpoint on us speaks to the bulb. Stays
// in the helper because changing it doesn't change observable behavior.
#define COORDINATOR_EP  1

// Transition time on the wire. 0 = snap. We rely on the off/on cycle
// (not a fade) to hide intermediate frames during color changes.
#define WIRE_TRANSITION_TENTHS  0

void bulb_init(bulb_t *b) {
    b->short_addr       = 0xFFFE;
    b->endpoint         = 0;
    b->known            = false;
    b->last_kind        = BULB_TARGET_OFF;
    b->last_on          = false;
    b->last_level       = 0xFF;
    b->last_temp_mireds = 0;
    b->last_color_x     = 0;
    b->last_color_y     = 0;
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

static void send_level(const bulb_t *b, uint8_t level) {
    esp_zb_zcl_move_to_level_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr(b);
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.level         = level;
    cmd.transition_time = WIRE_TRANSITION_TENTHS;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_level_move_to_level_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void send_color_temp(const bulb_t *b, uint16_t mireds) {
    esp_zb_zcl_color_move_to_color_temperature_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr(b);
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.color_temperature = mireds;
    cmd.transition_time   = WIRE_TRANSITION_TENTHS;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_color_move_to_color_temperature_cmd_req(&cmd);
    esp_zb_lock_release();
}

static void send_color_xy(const bulb_t *b, uint16_t x, uint16_t y) {
    esp_zb_zcl_color_move_to_color_cmd_t cmd = {0};
    cmd.zcl_basic_cmd = mk_addr(b);
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.color_x       = x;
    cmd.color_y       = y;
    cmd.transition_time = WIRE_TRANSITION_TENTHS;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_color_move_to_color_cmd_req(&cmd);
    esp_zb_lock_release();
}

// True iff the requested color setting differs from the last applied
// color setting. Comparison is by kind first, then by the kind-specific
// parameters.
static bool color_differs(const bulb_t *b, const bulb_target_t *t) {
    if (b->last_kind != t->kind) return true;
    if (t->kind == BULB_TARGET_TEMP) return b->last_temp_mireds != t->temp_mireds;
    if (t->kind == BULB_TARGET_XY)   return b->last_color_x != t->color_x ||
                                            b->last_color_y != t->color_y;
    return false;  // both OFF
}

void bulb_apply(bulb_t *b, const bulb_target_t *target) {
    if (!b->known) { LAMP_UI("bulb: skipped — bulb not paired"); return; }

    if (target->kind == BULB_TARGET_OFF) {
        if (b->last_on) {
            send_on_off(b, false);
            b->last_on = false;
            LAMP_LOGI("bulb: OFF (sent 1 cmd)");
        } else {
            LAMP_LOGI("bulb: OFF (already off, no-op)");
        }
        b->last_kind = BULB_TARGET_OFF;
        return;
    }

    bool color_changing = color_differs(b, target);
    bool level_changing = b->last_level != target->level;

    // The bulb processes color and level as separate ZCL frames. If
    // both are needed while the bulb is on, the gap between them
    // shows the new color at the old level (e.g. warm at level=5 =
    // "low white"). Off-cycling makes the bulb dark during the gap
    // so the intermediate frame never appears. The innr A19 stores
    // color/level commands received while off and applies them on
    // the subsequent "on".
    bool needs_off_cycle = color_changing && b->last_on;
    int sent = 0;

    if (needs_off_cycle) {
        send_on_off(b, false);
        b->last_on = false;
        sent++;
    }

    if (color_changing) {
        if (target->kind == BULB_TARGET_TEMP) {
            send_color_temp(b, target->temp_mireds);
            b->last_temp_mireds = target->temp_mireds;
        } else {  // BULB_TARGET_XY
            send_color_xy(b, target->color_x, target->color_y);
            b->last_color_x = target->color_x;
            b->last_color_y = target->color_y;
        }
        b->last_kind = target->kind;
        sent++;
    }

    if (level_changing) {
        send_level(b, target->level);
        b->last_level = target->level;
        sent++;
    }

    if (!b->last_on) {
        send_on_off(b, true);
        b->last_on = true;
        sent++;
    }

    if (target->kind == BULB_TARGET_TEMP) {
        LAMP_LOGI("bulb: ON temp=%u level=%u (sent %d cmd%s%s)",
                  target->temp_mireds, target->level, sent,
                  sent == 1 ? "" : "s",
                  needs_off_cycle ? ", off-cycled" : "");
    } else {
        LAMP_LOGI("bulb: ON xy=(0x%04x,0x%04x) level=%u (sent %d cmd%s%s)",
                  target->color_x, target->color_y, target->level, sent,
                  sent == 1 ? "" : "s",
                  needs_off_cycle ? ", off-cycled" : "");
    }
}
