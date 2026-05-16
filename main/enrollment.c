#include "enrollment.h"

#include "esp_zigbee_core.h"
#include "aps/esp_zigbee_aps.h"

#include "button.h"
#include "log.h"

#define COORDINATOR_ENDPOINT      1
#define IAS_ZONE_CLUSTER          0x0500
#define IAS_ZONE_ATTR_CIE_ADDR    0x0010
#define TR_PRIVATE_CLUSTER        0xFF01
#define TR_PRIVATE_MFG_CODE       0x1233
#define TR_CANCEL_DOUBLECLICK     0x0000
#define BASIC_CLUSTER             0x0000

// ── State for the enrollment dance ──────────────────────────────────────────
// s_enroll_target_short: short addr of the button being enrolled (set by
// start_ias_enrollment). The do_* callbacks pick this up because they're
// scheduled with no payload.
//
// s_pending_enroll_addr/ep: stashed by enrollment_handle_enroll_request so
// the scheduler callback knows where to reply. 0xFFFE = no pending reply.

static uint16_t            s_enroll_target_short = 0xFFFE;
static esp_zb_ieee_addr_t  s_coord_ieee_storage;
static uint8_t             s_enroll_tsn = 0x42;
static uint8_t             s_cancel_dbl_value = 1;
static uint16_t            s_pending_enroll_addr = 0xFFFE;
static uint8_t             s_pending_enroll_ep   = 1;

// ── Raw-byte Enroll Response ───────────────────────────────────────────────
// We construct the ZCL frame manually because every higher-level SDK helper
// we tried for IAS Zone Enroll Response produces a frame the 3RSB22BZ
// rejects (zoneState stays 0 even when CIE address verifies as ours).
//
// Bytes per ZCL §8.2 Enroll Response (cmd 0x00):
//   Byte 0: Frame Control = 0x19
//             bits 1:0 = 01 (cluster-specific command)
//             bit  2   = 0  (not manufacturer-specific)
//             bit  3   = 1  (server-to-client direction)
//             bit  4   = 1  (disable default response)
//   Byte 1: TSN
//   Byte 2: Command ID = 0x00 (Enroll Response)
//   Byte 3: enroll_response_code = 0x00 (SUCCESS)
//   Byte 4: zone_id = 0x17 (23 — matches zigbee-herdsman)
static void send_enroll_response(uint16_t short_addr, uint8_t endpoint) {
    static uint8_t frame[5];
    frame[0] = 0x19;
    frame[1] = s_enroll_tsn++;
    frame[2] = 0x00;
    frame[3] = 0x00;
    frame[4] = 0x17;
    esp_zb_apsde_data_req_t req = {0};
    req.dst_addr_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.dst_addr.addr_short = short_addr;
    req.dst_endpoint   = endpoint;
    req.profile_id     = 0x0104;
    req.cluster_id     = IAS_ZONE_CLUSTER;
    req.src_endpoint   = COORDINATOR_ENDPOINT;
    req.asdu_length    = sizeof(frame);
    req.asdu           = frame;
    req.tx_options     = ESP_ZB_APSDE_TX_OPT_ACK_TX;
    req.radius         = 0;
    esp_zb_aps_data_request(&req);
    LAMP_LOGI("ias zone: sent enroll response (raw 5B) to 0x%04x ep=%u tsn=%u",
              short_addr, endpoint, (unsigned)frame[1]);
}

// ── Scheduler callbacks (run on the Zigbee task) ────────────────────────────

static void do_cie_address_write(uint8_t arg) {
    (void)arg;
    if (s_enroll_target_short == 0xFFFE) return;
    esp_zb_get_long_address(s_coord_ieee_storage);
    esp_zb_zcl_attribute_t attr = {
        .id = IAS_ZONE_ATTR_CIE_ADDR,
        .data = {
            .type  = ESP_ZB_ZCL_ATTR_TYPE_IEEE_ADDR,
            .size  = 8,
            .value = s_coord_ieee_storage,
        },
    };
    esp_zb_zcl_write_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = s_enroll_target_short;
    cmd.zcl_basic_cmd.dst_endpoint = 1;
    cmd.zcl_basic_cmd.src_endpoint = COORDINATOR_ENDPOINT;
    cmd.address_mode               = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID                  = IAS_ZONE_CLUSTER;
    cmd.attr_number                = 1;
    cmd.attr_field                 = &attr;
    esp_zb_zcl_write_attr_cmd_req(&cmd);
    LAMP_LOGI("ias zone: wrote CIE address to 0x%04x", s_enroll_target_short);
}

static void do_unsolicited_enroll(uint8_t arg) {
    (void)arg;
    if (s_enroll_target_short == 0xFFFE) return;
    send_enroll_response(s_enroll_target_short, 1);
}

// Disable the 3RSB22BZ's double-click detection. Empirically returns
// UNSUPPORTED_ATTRIBUTE on the IAS-variant firmware; included for the
// multistate-variant firmware where it does work.
static void do_disable_double_click(uint8_t arg) {
    (void)arg;
    if (s_enroll_target_short == 0xFFFE) return;
    esp_zb_zcl_attribute_t attr = {
        .id = TR_CANCEL_DOUBLECLICK,
        .data = {
            .type  = ESP_ZB_ZCL_ATTR_TYPE_U8,
            .size  = 1,
            .value = &s_cancel_dbl_value,
        },
    };
    esp_zb_zcl_write_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = s_enroll_target_short;
    cmd.zcl_basic_cmd.dst_endpoint = 1;
    cmd.zcl_basic_cmd.src_endpoint = COORDINATOR_ENDPOINT;
    cmd.address_mode               = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID                  = TR_PRIVATE_CLUSTER;
    cmd.manuf_specific             = 1;
    cmd.manuf_code                 = TR_PRIVATE_MFG_CODE;
    cmd.attr_number                = 1;
    cmd.attr_field                 = &attr;
    esp_zb_zcl_write_attr_cmd_req(&cmd);
    LAMP_LOGI("3R: disabled double-click on 0x%04x", s_enroll_target_short);
}

static void deferred_enroll_response(uint8_t arg) {
    (void)arg;
    if (s_pending_enroll_addr == 0xFFFE) return;
    send_enroll_response(s_pending_enroll_addr, s_pending_enroll_ep);
    s_pending_enroll_addr = 0xFFFE;
}

// ── Public API ──────────────────────────────────────────────────────────────

void start_ias_enrollment(uint16_t short_addr) {
    s_enroll_target_short = short_addr;
    LAMP_LOGI("ias zone: kicking off auto-enrollment for 0x%04x", short_addr);
    esp_zb_scheduler_alarm(do_cie_address_write,    0,    0);
    esp_zb_scheduler_alarm(do_unsolicited_enroll,   0,  500);
    esp_zb_scheduler_alarm(do_disable_double_click, 0, 1000);
}

void enrollment_handle_enroll_request(uint16_t src, uint8_t ep) {
    s_pending_enroll_addr = src;
    s_pending_enroll_ep   = ep;
    esp_zb_scheduler_alarm(deferred_enroll_response, 0, 0);
}

void discover_button_attrs(void) {
    uint16_t target = button_short_addr();
    if (!button_is_known() || target == 0xFFFE) {
        LAMP_UI("discover: no button known (button_known=%d short=0x%04x)",
                (int)button_is_known(), target);
        return;
    }
    esp_zb_zcl_disc_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = target;
    cmd.zcl_basic_cmd.dst_endpoint = 1;
    cmd.zcl_basic_cmd.src_endpoint = COORDINATOR_ENDPOINT;
    cmd.address_mode      = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.cluster_id        = TR_PRIVATE_CLUSTER;
    cmd.manuf_specific    = 1;
    cmd.manuf_code        = TR_PRIVATE_MFG_CODE;
    cmd.start_attr_id     = 0x0000;
    cmd.max_attr_number   = 32;
    esp_zb_zcl_disc_attr_cmd_req(&cmd);
    LAMP_UI("discover: sent (cluster=0xFF01 mfg=0x1233 start=0x0000 max=32)");
}

void read_button_basic(void) {
    uint16_t target = button_short_addr();
    if (!button_is_known() || target == 0xFFFE) {
        LAMP_UI("read-basic: no button known");
        return;
    }
    static uint16_t attrs[] = {0x0000, 0x0001, 0x0004, 0x0005, 0x4000};
    esp_zb_zcl_read_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = target;
    cmd.zcl_basic_cmd.dst_endpoint = 1;
    cmd.zcl_basic_cmd.src_endpoint = COORDINATOR_ENDPOINT;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID    = BASIC_CLUSTER;
    cmd.attr_number  = sizeof(attrs) / sizeof(attrs[0]);
    cmd.attr_field   = attrs;
    esp_zb_zcl_read_attr_cmd_req(&cmd);
    LAMP_UI("read-basic: sent (ZCL ver, app ver, manuf, model, swBuildID)");
}
