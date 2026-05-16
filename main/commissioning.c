#include "commissioning.h"

#include <inttypes.h>
#include <string.h>

#include "enrollment.h"
#include "log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define NVS_NAMESPACE      "lamp"
#define NVS_KEY_BTN_IEEE   "btn_ieee"
#define NVS_KEY_BTN_EP     "btn_ep"
#define NVS_KEY_BTN_SHORT  "btn_short"
#define NVS_KEY_BULB_IEEE  "bulb_ieee"
#define NVS_KEY_BULB_EP    "bulb_ep"
#define NVS_KEY_BULB_SHORT "bulb_short"

static uint64_t bytes_to_ieee(const uint8_t b[8]) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}

static void load_role(const char *key_ieee, const char *key_ep, const char *key_short,
                      role_binding_t *r) {
    nvs_handle_t h;
    r->known = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    uint64_t ieee = 0;
    uint8_t  ep = 0;
    uint16_t sh = 0xFFFE;
    if (nvs_get_u64(h, key_ieee,  &ieee) == ESP_OK &&
        nvs_get_u8 (h, key_ep,    &ep)   == ESP_OK) {
        nvs_get_u16(h, key_short, &sh);
        r->ieee       = ieee;
        r->endpoint   = ep;
        r->short_addr = sh;
        r->known      = (ieee != 0);
    }
    nvs_close(h);
}

static void save_role(const char *key_ieee, const char *key_ep, const char *key_short,
                      const role_binding_t *r) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u64(h, key_ieee,  r->ieee);
    nvs_set_u8 (h, key_ep,    r->endpoint);
    nvs_set_u16(h, key_short, r->short_addr);
    nvs_commit(h);
    nvs_close(h);
}

void commissioning_init(commissioning_t *c, button_t *btn, bulb_t *blb,
                        commissioning_event_cb_t on_change) {
    c->button    = btn;
    c->bulb      = blb;
    c->on_change = on_change;

    load_role(NVS_KEY_BTN_IEEE,  NVS_KEY_BTN_EP,  NVS_KEY_BTN_SHORT,  &c->button_role);
    load_role(NVS_KEY_BULB_IEEE, NVS_KEY_BULB_EP, NVS_KEY_BULB_SHORT, &c->bulb_role);
    LAMP_LOGI("commissioning: button known=%d short=0x%04x | bulb known=%d short=0x%04x",
              c->button_role.known, c->button_role.short_addr,
              c->bulb_role.known,   c->bulb_role.short_addr);

    if (c->button_role.known && c->button_role.short_addr != 0xFFFE) {
        button_set_address(btn, c->button_role.short_addr, c->button_role.endpoint);
    }
    if (c->bulb_role.known && c->bulb_role.short_addr != 0xFFFE) {
        bulb_set_address(blb, c->bulb_role.short_addr, c->bulb_role.endpoint);
    }
}

bool commissioning_complete(const commissioning_t *c) {
    return c->button_role.known && c->bulb_role.known;
}

void commissioning_reset(commissioning_t *c) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_BTN_IEEE);
        nvs_erase_key(h, NVS_KEY_BTN_EP);
        nvs_erase_key(h, NVS_KEY_BULB_IEEE);
        nvs_erase_key(h, NVS_KEY_BULB_EP);
        nvs_commit(h);
        nvs_close(h);
    }
    c->button_role.known = false;
    c->bulb_role.known   = false;
    LAMP_LOGI("commissioning: bindings cleared");
}

void commissioning_on_device_announce(commissioning_t *c,
                                      uint16_t short_addr,
                                      const uint8_t ieee[8]) {
    uint64_t ieee_u = bytes_to_ieee(ieee);
    LAMP_LOGI("commissioning: device announce short=0x%04x ieee=%016" PRIx64,
              short_addr, ieee_u);

    if (c->button_role.known && c->button_role.ieee == ieee_u) {
        c->button_role.short_addr = short_addr;
        save_role(NVS_KEY_BTN_IEEE, NVS_KEY_BTN_EP, NVS_KEY_BTN_SHORT, &c->button_role);
        button_set_address(c->button, short_addr, c->button_role.endpoint);
        LAMP_LOGI("commissioning: refreshed button short=0x%04x", short_addr);
        start_ias_enrollment(short_addr);
        if (c->on_change) c->on_change();
        return;
    }
    if (c->bulb_role.known && c->bulb_role.ieee == ieee_u) {
        c->bulb_role.short_addr = short_addr;
        save_role(NVS_KEY_BULB_IEEE, NVS_KEY_BULB_EP, NVS_KEY_BULB_SHORT, &c->bulb_role);
        bulb_set_address(c->bulb, short_addr, c->bulb_role.endpoint);
        LAMP_LOGI("commissioning: refreshed bulb short=0x%04x", short_addr);
        if (c->on_change) c->on_change();
        return;
    }

    if (!c->button_role.known) {
        c->button_role.ieee       = ieee_u;
        c->button_role.endpoint   = 1;
        c->button_role.short_addr = short_addr;
        c->button_role.known      = true;
        save_role(NVS_KEY_BTN_IEEE, NVS_KEY_BTN_EP, NVS_KEY_BTN_SHORT, &c->button_role);
        button_set_address(c->button, short_addr, c->button_role.endpoint);
        LAMP_LOGI("commissioning: assigned button 0x%04x", short_addr);
        start_ias_enrollment(short_addr);
        LAMP_UI("Button paired");
        if (c->on_change) c->on_change();
    } else if (!c->bulb_role.known) {
        c->bulb_role.ieee       = ieee_u;
        c->bulb_role.endpoint   = 1;
        c->bulb_role.short_addr = short_addr;
        c->bulb_role.known      = true;
        save_role(NVS_KEY_BULB_IEEE, NVS_KEY_BULB_EP, NVS_KEY_BULB_SHORT, &c->bulb_role);
        bulb_set_address(c->bulb, short_addr, c->bulb_role.endpoint);
        LAMP_LOGI("commissioning: assigned bulb 0x%04x", short_addr);
        LAMP_UI("Bulb paired");
        if (c->on_change) c->on_change();
    } else {
        LAMP_LOGW("commissioning: extra device joined, ignoring 0x%04x", short_addr);
    }
}
