#include "commissioning.h"

#include <inttypes.h>
#include <string.h>

#include "esp_zigbee_core.h"

#include "bulb.h"
#include "button.h"
#include "log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "enrollment.h"

#define NVS_NAMESPACE     "lamp"
#define NVS_KEY_BTN_IEEE  "btn_ieee"
#define NVS_KEY_BTN_EP    "btn_ep"
#define NVS_KEY_BULB_IEEE "bulb_ieee"
#define NVS_KEY_BULB_EP   "bulb_ep"
#define NVS_KEY_BTN_SHORT  "btn_short"
#define NVS_KEY_BULB_SHORT "bulb_short"

#define JOIN_WINDOW_S 180

typedef struct {
    bool      known;
    uint64_t  ieee;
    uint8_t   endpoint;
    uint16_t  short_addr;  // refreshed on every device announce; may be stale
} role_binding_t;

static role_binding_t s_button = {0};
static role_binding_t s_bulb   = {0};

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
        nvs_get_u16(h, key_short, &sh);  // best-effort; may be missing
        r->ieee       = ieee;
        r->endpoint   = ep;
        r->short_addr = sh;
        r->known      = (ieee != 0);
    }
    nvs_close(h);
}

static void save_role(const char *key_ieee, const char *key_ep, const char *key_short,
                      role_binding_t *r) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u64(h, key_ieee,  r->ieee);
    nvs_set_u8 (h, key_ep,    r->endpoint);
    nvs_set_u16(h, key_short, r->short_addr);
    nvs_commit(h);
    nvs_close(h);
}

void commissioning_init(void) {
    load_role(NVS_KEY_BTN_IEEE,  NVS_KEY_BTN_EP,  NVS_KEY_BTN_SHORT,  &s_button);
    load_role(NVS_KEY_BULB_IEEE, NVS_KEY_BULB_EP, NVS_KEY_BULB_SHORT, &s_bulb);
    LAMP_LOGI("commissioning: button known=%d short=0x%04x | bulb known=%d short=0x%04x",
              s_button.known, s_button.short_addr,
              s_bulb.known,   s_bulb.short_addr);
    // Push the persisted short addresses into button.c / bulb.c so commands
    // can be issued before either device next sends a device-announce.
    if (s_button.known && s_button.short_addr != 0xFFFE) {
        button_set_address(s_button.short_addr, s_button.endpoint);
    }
    if (s_bulb.known && s_bulb.short_addr != 0xFFFE) {
        bulb_set_address(s_bulb.short_addr, s_bulb.endpoint);
    }
}

bool commissioning_complete(void) {
    return s_button.known && s_bulb.known;
}

void commissioning_reset(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY_BTN_IEEE);
        nvs_erase_key(h, NVS_KEY_BTN_EP);
        nvs_erase_key(h, NVS_KEY_BULB_IEEE);
        nvs_erase_key(h, NVS_KEY_BULB_EP);
        nvs_commit(h);
        nvs_close(h);
    }
    s_button.known = false;
    s_bulb.known = false;
    LAMP_LOGI("commissioning: bindings cleared");
}

// Role assignment by join order: 1st = button (paired first), 2nd = bulb.
// On re-announce of an already-known device, refresh its short address only.
void commissioning_on_device_announce(uint16_t short_addr, const uint8_t ieee[8]) {
    uint64_t ieee_u = bytes_to_ieee(ieee);
    LAMP_LOGI("commissioning: device announce short=0x%04x ieee=%016" PRIx64,
              short_addr, ieee_u);

    // Re-announce of the known button — refresh its short address and
    // re-trigger IAS enrollment in case the button forgot its CIE.
    if (s_button.known && s_button.ieee == ieee_u) {
        s_button.short_addr = short_addr;
        save_role(NVS_KEY_BTN_IEEE, NVS_KEY_BTN_EP, NVS_KEY_BTN_SHORT, &s_button);
        button_set_address(short_addr, s_button.endpoint);
        LAMP_LOGI("commissioning: refreshed button short=0x%04x", short_addr);
        start_ias_enrollment(short_addr);
        return;
    }
    // Re-announce of the known bulb — refresh its short address.
    if (s_bulb.known && s_bulb.ieee == ieee_u) {
        s_bulb.short_addr = short_addr;
        save_role(NVS_KEY_BULB_IEEE, NVS_KEY_BULB_EP, NVS_KEY_BULB_SHORT, &s_bulb);
        bulb_set_address(short_addr, s_bulb.endpoint);
        LAMP_LOGI("commissioning: refreshed bulb short=0x%04x", short_addr);
        return;
    }

    // New device. First unknown → button. Second → bulb.
    if (!s_button.known) {
        s_button.ieee       = ieee_u;
        s_button.endpoint   = 1;
        s_button.short_addr = short_addr;
        s_button.known      = true;
        save_role(NVS_KEY_BTN_IEEE, NVS_KEY_BTN_EP, NVS_KEY_BTN_SHORT, &s_button);
        button_set_address(short_addr, s_button.endpoint);
        LAMP_LOGI("commissioning: assigned button 0x%04x", short_addr);
        start_ias_enrollment(short_addr);
        LAMP_UI("Button paired");
        ui_banner();
    } else if (!s_bulb.known) {
        s_bulb.ieee       = ieee_u;
        s_bulb.endpoint   = 1;
        s_bulb.short_addr = short_addr;
        s_bulb.known      = true;
        save_role(NVS_KEY_BULB_IEEE, NVS_KEY_BULB_EP, NVS_KEY_BULB_SHORT, &s_bulb);
        bulb_set_address(short_addr, s_bulb.endpoint);
        LAMP_LOGI("commissioning: assigned bulb 0x%04x", short_addr);
        LAMP_UI("Bulb paired");
        ui_banner();
    } else {
        LAMP_LOGW("commissioning: extra device joined, ignoring 0x%04x", short_addr);
    }
}

