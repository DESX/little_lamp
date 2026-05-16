// Firmware entry point: brings up Zigbee, wires the modules together, and
// owns the long-lived background tasks.

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "aps/esp_zigbee_aps.h"
#include "test/esp_zigbee_test_utils.h"  // esp_zb_nwk_set_ed_timeout
#include "esp_zigbee_secur.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bulb.h"
#include "button.h"
#include "commissioning.h"
#include "console.h"
#include "log.h"
#include "rtc.h"
#include "schedule.h"
#include "state.h"

// Note: button.h still owns the press-event channel for completeness; we now
// call state_handle_button_press() directly from the OnOff attr callback.

#define COORDINATOR_ENDPOINT 1
#define JOIN_WINDOW_SECS     180

// Pin to Zigbee channel 25 (2475 MHz). Sits above Wi-Fi 2.4 GHz channels
// 1/6/11, so it avoids the worst of Wi-Fi interference. To scan all
// channels instead, use 0x07FFF800.
#define ESP_ZB_PRIMARY_CHANNEL_MASK (1u << 25)


// Wrapper to match esp_zb_callback_t (void return) when scheduling.
static void retry_form_network(uint8_t mode) {
    esp_zb_bdb_start_top_level_commissioning(mode);
}

// ── APS-level frame catcher ─────────────────────────────────────────────────
// Fires for every incoming Zigbee frame, regardless of whether higher-level
// SDK callbacks match. Used to (a) prove the button is reaching us, and (b)
// route OnOff cluster commands to the state machine even when they arrive
// in a form ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID doesn't catch (e.g. group-
// addressed Toggle commands).
// IAS Zone cluster (0x0500): the 3RSB022Z sends Zone Status Change
// Notifications (server-to-client cmd 0x00) on press. Used only in the
// button's "Echo mode" firmware path, which has a baked-in 1.5–2 s
// motion-sensor-style dwell. After the mode-switch gesture (5 quick
// taps on the reset button) the button uses Multistate Input instead.
#define IAS_ZONE_CLUSTER             0x0500
#define IAS_ZONE_ATTR_CIE_ADDR       0x0010
#define IAS_ZONE_CMD_STATUS_CHANGE   0x00
#define IAS_ZONE_CMD_ENROLL_REQUEST  0x01

// Multistate Input cluster (0x0012): the 3RSB022Z's standard (snappy) firmware
// mode reports button events as Report Attributes on this cluster's
// presentValue (attr 0x0055). Values per zigbee-herdsman-converters
// itcmdr_clicks: 1=single, 2=double, 0=hold, 255=release. We treat 1 as a
// press; 2/0 are ignored (we don't expose double/hold).
#define MULTISTATE_CLUSTER           0x0012
#define MULTISTATE_ATTR_PRESENT_VAL  0x0055
#define MULTISTATE_VALUE_SINGLE      0x0001

// OTA Upgrade cluster (0x0019). The 3RSB22BZ periodically asks "do you
// have an image for me?" If we don't respond it issues NWK_LEAVE within
// seconds. The proper response is Query Next Image Response with status
// 0x98 NO_IMAGE_AVAILABLE.
#define OTA_CLUSTER                  0x0019
#define OTA_CMD_QUERY_NEXT_IMAGE_REQ 0x01
#define OTA_CMD_QUERY_NEXT_IMAGE_RSP 0x02
#define OTA_STATUS_NO_IMAGE_AVAILABLE 0x98

// Frame dedup ring. APS retries re-use the original ZCL TSN, so a tuple of
// (src_short, cluster, cmd, tsn) uniquely identifies a logical frame
// regardless of how many physical-layer copies arrive.
#define DEDUP_RING_SIZE 8
typedef struct {
    bool     used;
    uint16_t src;
    uint16_t cluster;
    uint8_t  cmd;
    uint8_t  tsn;
} frame_id_t;
static frame_id_t s_dedup_ring[DEDUP_RING_SIZE];
static uint8_t    s_dedup_head = 0;

static bool frame_is_duplicate(uint16_t src, uint16_t cluster, uint8_t cmd, uint8_t tsn) {
    for (int i = 0; i < DEDUP_RING_SIZE; i++) {
        const frame_id_t *r = &s_dedup_ring[i];
        if (r->used && r->src == src && r->cluster == cluster &&
            r->cmd == cmd && r->tsn == tsn) {
            return true;
        }
    }
    s_dedup_ring[s_dedup_head] = (frame_id_t){
        .used = true, .src = src, .cluster = cluster, .cmd = cmd, .tsn = tsn,
    };
    s_dedup_head = (s_dedup_head + 1) % DEDUP_RING_SIZE;
    return false;
}

// ── IAS Zone auto-enrollment ────────────────────────────────────────────────
// Per zigbee-herdsman's canonical flow (controller/model/device.ts:1049-1093):
// after the device joins, the coordinator writes its own IEEE into the
// device's IAS_CIE_Address attribute, waits ~500 ms, then sends an
// unsolicited Zone Enroll Response. We additionally still reply to any
// runtime Zone Enroll Request frames (Trip-to-Pair flow).

static uint16_t s_enroll_target_short = 0xFFFE;
static esp_zb_ieee_addr_t s_coord_ieee_storage;

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

// Raw-byte Enroll Response. We construct the ZCL frame manually because
// every higher-level SDK helper we tried for IAS Zone Enroll Response
// produces a frame the 3RSB22BZ rejects (zoneState stays 0 even when CIE
// address verifies as ours). Bytes per ZCL §8.2 Enroll Response (cmd 0x00):
//
//   Byte 0: Frame Control = 0x19
//             bits 1:0 = 01 (cluster-specific command)
//             bit  2   = 0  (not manufacturer-specific)
//             bit  3   = 1  (server-to-client direction)
//             bit  4   = 1  (disable default response)
//   Byte 1: TSN
//   Byte 2: Command ID = 0x00 (Enroll Response)
//   Byte 3: enroll_response_code = 0x00 (SUCCESS)
//   Byte 4: zone_id = 0x17 (23, matches zigbee-herdsman)
static uint8_t s_enroll_tsn = 0x42;

static void send_enroll_response(uint16_t short_addr, uint8_t endpoint) {
    static uint8_t frame[5];
    frame[0] = 0x19;          // FC
    frame[1] = s_enroll_tsn++;
    frame[2] = 0x00;          // cmd: Enroll Response
    frame[3] = 0x00;          // enroll_response_code: SUCCESS
    frame[4] = 0x17;          // zone_id: 23
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

static void do_unsolicited_enroll(uint8_t arg) {
    (void)arg;
    if (s_enroll_target_short == 0xFFFE) return;
    send_enroll_response(s_enroll_target_short, 1);
}

// Read back zoneState (0x0000) AND iasCieAddr (0x0010) from the button's
// IAS Zone cluster. Result arrives via the action handler as
// ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID. If iasCieAddr doesn't equal our
// own IEEE, our CIE write never stuck and that's why enrollment fails.
static void do_read_zone_state(uint8_t arg) {
    (void)arg;
    if (s_enroll_target_short == 0xFFFE) return;
    static uint16_t attr_ids[2] = { 0x0000, 0x0010 };  // ZoneState, IAS_CIE_Address
    esp_zb_zcl_read_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = s_enroll_target_short;
    cmd.zcl_basic_cmd.dst_endpoint = 1;
    cmd.zcl_basic_cmd.src_endpoint = COORDINATOR_ENDPOINT;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID    = IAS_ZONE_CLUSTER;
    cmd.attr_number  = 2;
    cmd.attr_field   = attr_ids;
    esp_zb_zcl_read_attr_cmd_req(&cmd);
    LAMP_LOGI("ias zone: reading zoneState + CIE addr from 0x%04x",
              s_enroll_target_short);
}

// Disable the 3RSB22BZ's double-click detection. The button defaults to a
// ~1.5 s window after each press to see if a second press follows, which is
// what makes a single tap take ~2 s end-to-end. Writing cancelDoubleClick=1
// to the manufacturer-specific cluster 0xFF01 (mfg code 0x1233) tells the
// button to fire single presses instantly. Discovered in the user-story
// 3RSB22BZ Third Reality attribute table (also at zigbee-herdsman-converters
// src/devices/third_reality.ts:737-746).
#define TR_PRIVATE_CLUSTER     0xFF01
#define TR_PRIVATE_MFG_CODE    0x1233
#define TR_CANCEL_DOUBLECLICK  0x0000
static uint8_t s_cancel_dbl_value = 1;

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

// Read back the cancelDoubleClick attribute to confirm the button actually
// supports it. If status != 0, the cluster/attribute don't exist on this
// firmware revision — meaning the 2-second press latency is baked in.
static void do_verify_double_click(uint8_t arg) {
    (void)arg;
    if (s_enroll_target_short == 0xFFFE) return;
    static uint16_t attr_id = TR_CANCEL_DOUBLECLICK;
    esp_zb_zcl_read_attr_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = s_enroll_target_short;
    cmd.zcl_basic_cmd.dst_endpoint = 1;
    cmd.zcl_basic_cmd.src_endpoint = COORDINATOR_ENDPOINT;
    cmd.address_mode  = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.clusterID     = TR_PRIVATE_CLUSTER;
    cmd.manuf_specific = 1;
    cmd.manuf_code    = TR_PRIVATE_MFG_CODE;
    cmd.attr_number   = 1;
    cmd.attr_field    = &attr_id;
    esp_zb_zcl_read_attr_cmd_req(&cmd);
    LAMP_LOGI("3R: reading cancelDoubleClick back from 0x%04x", s_enroll_target_short);
}

// Manually triggered (`discover` console command) attribute scan of the
// button's cluster 0xFF01. Used to find what timing-related attributes the
// firmware actually supports, when our blind cancelDoubleClick write returns
// UNSUPPORTED_ATTRIBUTE.
extern void discover_button_attrs(void);

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

// Read the button's Basic cluster (0x0000) for ZCL version (0x0000),
// applicationVersion (0x0001), softwareBuildID (0x4000), manuf (0x0004),
// model (0x0005). The softwareBuildID is what tells us whether an OTA
// upgrade is even possible (compare to latest known image).
extern void read_button_basic(void);
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
    cmd.clusterID    = 0x0000;  // Basic
    cmd.attr_number  = sizeof(attrs) / sizeof(attrs[0]);
    cmd.attr_field   = attrs;
    esp_zb_zcl_read_attr_cmd_req(&cmd);
    LAMP_UI("read-basic: sent (ZCL ver, app ver, manuf, model, swBuildID)");
}

// Public entry. Call once on each device-announce from the button.
void start_ias_enrollment(uint16_t short_addr) {
    s_enroll_target_short = short_addr;
    LAMP_LOGI("ias zone: kicking off auto-enrollment for 0x%04x", short_addr);
    esp_zb_scheduler_alarm(do_cie_address_write,       0,    0);
    esp_zb_scheduler_alarm(do_unsolicited_enroll,      0,  500);
    esp_zb_scheduler_alarm(do_disable_double_click,    0, 1000);
    esp_zb_scheduler_alarm(do_verify_double_click,     0, 1500);
    // Read zoneState at two checkpoints to disambiguate timing vs format:
    //   t=2s  — right after our unsolicited response (early window)
    //   t=8s  — well after the button's own enroll-request flow had time
    //           to complete (late window). If both read 0, format is wrong.
    esp_zb_scheduler_alarm(do_read_zone_state,     0, 2000);
    esp_zb_scheduler_alarm(do_read_zone_state,     0, 8000);
}

// Press dispatch:
//   1) UI print and counter happen synchronously from the APS callback so the
//      user-perceived latency is just RF + USB-serial — no scheduler hop.
//   2) The state transition (and any bulb commands it triggers) is deferred
//      onto the Zigbee scheduler so the APS callback returns immediately and
//      we don't risk a lock against the Zigbee stack.
//   3) Dedup happens upstream of this function via the (src, cluster, cmd,
//      tsn) ring in zb_aps_indication — that's the correct fix for APS
//      retransmissions, which re-use the original TSN.
static uint32_t s_press_count = 0;
static TaskHandle_t s_press_task = NULL;

// Press dispatch task. Runs at high priority (above the Zigbee task) so it
// processes a notification as soon as the Zigbee task drops its lock. Uses
// xTaskNotify with pdFALSE so back-to-back gives accumulate — every press
// is processed in order even if they arrive faster than the task drains.
static void press_dispatch_task(void *pv) {
    (void)pv;
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        state_handle_button_press();
    }
}

// Called from the APS callback. Must never block: anything that could stall
// (state machine, bulb commands, printf) is moved off-thread via the press
// task and the LAMP_UI drainer queue.
static void on_button_press(void) {
    s_press_count++;
    LAMP_UI("button press %lu", (unsigned long)s_press_count);  // non-blocking
    if (s_press_task != NULL) {
        xTaskNotifyGive(s_press_task);
    }
}

// Runtime reply to incoming Zone Enroll Request frames (Trip-to-Pair flow).
// Stashed because send_enroll_response calls into the Zigbee stack and we
// want to do it from the scheduler thread, not the APS callback.
static uint16_t s_pending_enroll_addr = 0xFFFE;
static uint8_t  s_pending_enroll_ep   = 1;

static void deferred_enroll_response(uint8_t arg) {
    (void)arg;
    if (s_pending_enroll_addr == 0xFFFE) return;
    send_enroll_response(s_pending_enroll_addr, s_pending_enroll_ep);
    s_pending_enroll_addr = 0xFFFE;
}

// Reply to OTA Query Next Image Request with NO_IMAGE_AVAILABLE so the
// button doesn't interpret the (otherwise UNSUP_CLUSTER) response as a
// broken-network signal and issue NWK_LEAVE.
static uint16_t s_pending_ota_addr = 0xFFFE;
static uint8_t  s_pending_ota_ep   = 1;
static uint8_t  s_ota_no_image_status = OTA_STATUS_NO_IMAGE_AVAILABLE;

static void deferred_ota_no_image(uint8_t arg) {
    (void)arg;
    if (s_pending_ota_addr == 0xFFFE) return;
    esp_zb_zcl_custom_cluster_cmd_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = s_pending_ota_addr;
    cmd.zcl_basic_cmd.dst_endpoint = s_pending_ota_ep;
    cmd.zcl_basic_cmd.src_endpoint = COORDINATOR_ENDPOINT;
    cmd.address_mode               = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.profile_id                 = 0x0104;
    cmd.cluster_id                 = OTA_CLUSTER;
    cmd.direction                  = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd.dis_default_resp           = 1;
    cmd.custom_cmd_id              = OTA_CMD_QUERY_NEXT_IMAGE_RSP;
    cmd.data.type                  = ESP_ZB_ZCL_ATTR_TYPE_U8;
    cmd.data.size                  = 1;
    cmd.data.value                 = &s_ota_no_image_status;
    esp_zb_zcl_custom_cluster_cmd_resp(&cmd);
    LAMP_LOGI("ota: replied NO_IMAGE_AVAILABLE to 0x%04x ep=%u",
              s_pending_ota_addr, s_pending_ota_ep);
    s_pending_ota_addr = 0xFFFE;
}

static bool zb_aps_indication(esp_zb_apsde_data_ind_t ind) {
    // ZCL frame layout: byte 0 = frame control; if bit 2 (manuf_spec) is set,
    // bytes 1-2 are the manufacturer code and the TSN/cmd_id are pushed by 2.
    //
    // Frame control bits 0-1 are the frame type:
    //   00 = global ZCL command (Read Attr Resp, etc.) — applies to ALL clusters
    //   01 = cluster-specific command — only valid for that cluster
    // A cluster-specific cmd_id 0x01 (e.g. IAS Zone Enroll Request) and a
    // global cmd_id 0x01 (Read Attributes Response) are different things;
    // we must not conflate them.
    uint8_t cmd_id = 0xff;
    uint8_t tsn    = 0;
    uint8_t hdr_len = 3;
    bool cluster_specific = false;
    if (ind.asdu_length >= 3 && ind.asdu) {
        uint8_t fc = ind.asdu[0];
        cluster_specific = ((fc & 0x03) == 0x01);
        bool manuf = (fc & 0x04) != 0;
        hdr_len = manuf ? 5 : 3;
        tsn    = ind.asdu[manuf ? 3 : 1];
        cmd_id = ind.asdu[manuf ? 4 : 2];
    }
    LAMP_LOGI("rx frame: src=0x%04x ep=%u cluster=0x%04x prof=0x%04x cmd=0x%02x%s tsn=%u len=%lu",
              ind.src_short_addr, ind.src_endpoint, ind.cluster_id,
              ind.profile_id, cmd_id, cluster_specific ? "(cs)" : "(g)",
              tsn, (unsigned long)ind.asdu_length);

    // APS retries re-use the original TSN. Dedup on (src, cluster, cmd, tsn)
    // so we don't double-count retransmissions, while still allowing distinct
    // logical events through.
    if (frame_is_duplicate(ind.src_short_addr, ind.cluster_id, cmd_id, tsn)) {
        LAMP_LOGI("rx frame: dropped duplicate (tsn=%u)", tsn);
        return false;
    }

    // IAS Zone status-change. The zone_status word has up to 10 distinct
    // bits; we treat only the rising edge of bit 0 (alarm1) as "press." A
    // status with only the higher bits set (supervision, battery, test) is
    // explicitly not a press, even though the upper bits make the word
    // non-zero.
    static bool s_prev_alarm = false;
    bool is_press = false;
    if (cluster_specific &&
        ind.cluster_id == IAS_ZONE_CLUSTER &&
        cmd_id == IAS_ZONE_CMD_STATUS_CHANGE &&
        ind.asdu_length >= hdr_len + 2) {
        uint16_t zone_status =
            (uint16_t)ind.asdu[hdr_len] |
            ((uint16_t)ind.asdu[hdr_len + 1] << 8);
        bool alarm = (zone_status & 0x0001) != 0;
        bool tamper = (zone_status & 0x0004) != 0;
        bool battery = (zone_status & 0x0008) != 0;
        bool supervision = (zone_status & 0x0010) != 0;
        bool restore = (zone_status & 0x0020) != 0;
        LAMP_LOGI("ias zone status=0x%04x alarm=%d tamper=%d batt=%d sup=%d rst=%d",
                  zone_status, alarm, tamper, battery, supervision, restore);
        // Press = transition 0→1 on alarm1. Filters out:
        //   - the release frame (alarm 1→0)
        //   - repeated alarm=1 retransmissions
        //   - frames where only non-alarm bits flipped (supervision pings etc.)
        is_press = alarm && !s_prev_alarm;
        s_prev_alarm = alarm;
    } else if (cluster_specific &&
               ind.cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
               ind.profile_id == 0x0104 &&
               (cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_ON_ID ||
                cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID ||
                cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID)) {
        // Standard-mode button: the 3RSB22BZ sends a cluster-specific OnOff
        // command (On / Off / Toggle) IMMEDIATELY on each press — before
        // it does the ~300 ms single/double-click discrimination wait that
        // it does on the Multistate Input report path. We listen here for
        // absolute minimum latency. Cluster-specific + profile guards keep
        // us safe from:
        //   - global Default Responses (cmd 0x0b) from the bulb (cluster
        //     specific=false → filtered)
        //   - ZDP Match Descriptor Request (profile=0x0000 → filtered)
        // Tradeoff: we cannot distinguish single from double press. Single
        // press = toggle. Double press = toggle twice (no net change). For
        // our user story (single press toggles light) this is exactly what
        // we want.
        is_press = true;
    }
    // The Multistate Input report path (cluster 0x0012) is intentionally
    // not handled — it carries the same logical event ~300 ms later. If
    // future use cases need to distinguish double-press / hold, re-enable
    // the Multistate path and pick which channel wins.

    if (is_press) {
        on_button_press();
    }
    if (cluster_specific &&
        ind.cluster_id == IAS_ZONE_CLUSTER &&
        cmd_id == IAS_ZONE_CMD_ENROLL_REQUEST) {
        s_pending_enroll_addr = ind.src_short_addr;
        s_pending_enroll_ep   = ind.src_endpoint;
        esp_zb_scheduler_alarm(deferred_enroll_response, 0, 0);
    }
    if (cluster_specific &&
        ind.cluster_id == OTA_CLUSTER &&
        cmd_id == OTA_CMD_QUERY_NEXT_IMAGE_REQ) {
        // TEST A: do NOT proactively respond. Just suppress ZBOSS's
        // UNSUP_CLUSTER default response. If the button still leaves at
        // +4s, OTA reply isn't the cause.
        LAMP_LOGI("ota: query received, no response sent (Test A)");
        return true;
    }
    return false;  // let the stack process normally otherwise
}

// ── Background tasks ────────────────────────────────────────────────────────

static void boundary_task(void *pv) {
    (void)pv;
    for (;;) {
        time_t now = rtc_now();
        time_t next = schedule_next_boundary(now);
        time_t delay_s = next - now;
        if (delay_s < 1) delay_s = 1;
        LAMP_LOGI("boundary: next fire in %llds (epoch=%lld, night=%d)",
                  (long long)delay_s, (long long)next, schedule_is_night(now));
        vTaskDelay(pdMS_TO_TICKS((uint32_t)delay_s * 1000));
        state_handle_boundary_cross();
    }
}

// ── Zigbee signal handler ───────────────────────────────────────────────────

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct) {
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t sig_type = *p_sg_p;

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            LAMP_LOGI("zb: stack initialized");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
            LAMP_LOGI("zb: first-start, forming network");
            esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            if (err_status == ESP_OK) {
                esp_zb_ieee_addr_t my_ieee;
                esp_zb_get_long_address(my_ieee);
                LAMP_UI("coordinator IEEE: %02x%02x%02x%02x%02x%02x%02x%02x",
                        my_ieee[7], my_ieee[6], my_ieee[5], my_ieee[4],
                        my_ieee[3], my_ieee[2], my_ieee[1], my_ieee[0]);
                LAMP_LOGI("zb: rebooted into existing network");
                if (!commissioning_complete()) {
                    LAMP_LOGI("zb: opening join window (%ds) + F&B target — bindings incomplete",
                              JOIN_WINDOW_SECS);
                    esp_zb_bdb_open_network(JOIN_WINDOW_SECS);
                    esp_zb_bdb_finding_binding_start_target(COORDINATOR_ENDPOINT,
                                                            JOIN_WINDOW_SECS);
                }
            } else {
                LAMP_LOGE("zb: reboot failed (0x%x), restarting commissioning", err_status);
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_FORMATION);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_FORMATION:
            if (err_status == ESP_OK) {
                esp_zb_ieee_addr_t extended_pan_id;
                esp_zb_get_extended_pan_id(extended_pan_id);
                LAMP_LOGI("zb: formed network ch=%d short=0x%04x pan=0x%04x",
                          esp_zb_get_current_channel(),
                          esp_zb_get_short_address(),
                          esp_zb_get_pan_id());
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                LAMP_LOGE("zb: formation failed (0x%x), retrying", err_status);
                esp_zb_scheduler_alarm(retry_form_network,
                                       ESP_ZB_BDB_MODE_NETWORK_FORMATION, 1000);
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            LAMP_LOGI("zb: steering done, opening join window (%ds) + F&B target mode",
                      JOIN_WINDOW_SECS);
            esp_zb_bdb_open_network(JOIN_WINDOW_SECS);
            // Advertise ourselves as a Finding & Binding target so a smart
            // button can bind its OnOff client cluster to our OnOff server.
            esp_zb_bdb_finding_binding_start_target(COORDINATOR_ENDPOINT,
                                                   JOIN_WINDOW_SECS);
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
            esp_zb_zdo_signal_device_annce_params_t *anc =
                (esp_zb_zdo_signal_device_annce_params_t *)
                    esp_zb_app_signal_get_params(p_sg_p);
            commissioning_on_device_announce(anc->device_short_addr, anc->ieee_addr);
            break;
        }

        case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
            LAMP_LOGI("zb: permit-join status changed");
            break;

        default:
            // Log every signal during commissioning so we can see what the
            // button/bulb are doing during pairing handshakes.
            LAMP_LOGI("zb: signal 0x%x status=0x%x", sig_type, err_status);
            break;
    }
}

// ── Zigbee action handler — for OnOff commands FROM the button ──────────────

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id,
                                   const void *message) {
    switch (cb_id) {
        case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
            // Diagnostic only — press detection happens in the APS handler
            // (which sees frames the OnOff command-vs-attribute path misses).
            const esp_zb_zcl_set_attr_value_message_t *msg =
                (const esp_zb_zcl_set_attr_value_message_t *)message;
            LAMP_LOGI("zb: SET_ATTR cluster=0x%04x ep=%u attr=0x%04x",
                      msg->info.cluster, msg->info.dst_endpoint, msg->attribute.id);
            break;
        }
        case ESP_ZB_CORE_CMD_DISC_ATTR_RESP_CB_ID: {
            const esp_zb_zcl_cmd_discover_attributes_resp_message_t *m =
                (const esp_zb_zcl_cmd_discover_attributes_resp_message_t *)message;
            LAMP_UI("disc-resp cluster=0x%04x complete=%d:",
                    m->info.cluster, m->is_completed);
            for (const esp_zb_zcl_disc_attr_variable_t *v = m->variables;
                 v != NULL; v = v->next) {
                LAMP_UI("  attr=0x%04x type=0x%02x", v->attr_id, v->data_type);
            }
            break;
        }
        case ESP_ZB_CORE_CMD_READ_ATTR_RESP_CB_ID: {
            const esp_zb_zcl_cmd_read_attr_resp_message_t *m =
                (const esp_zb_zcl_cmd_read_attr_resp_message_t *)message;
            esp_zb_zcl_read_attr_resp_variable_t *v = m->variables;
            for (; v != NULL; v = v->next) {
                char hex[33] = "(empty)";
                if (v->attribute.data.value != NULL && v->attribute.data.size > 0) {
                    uint16_t n = v->attribute.data.size;
                    if (n > 16) n = 16;
                    for (uint16_t i = 0; i < n; i++) {
                        snprintf(&hex[i*2], 3, "%02x",
                                 ((uint8_t *)v->attribute.data.value)[i]);
                    }
                }
                LAMP_UI("read-attr-resp cluster=0x%04x attr=0x%04x status=%u data=%s",
                        m->info.cluster, v->attribute.id, v->status, hex);
            }
            break;
        }
        default:
            // Log every action callback we don't handle, so a paired-but-
            // -unbound button still shows up in the log when pressed.
            LAMP_LOGI("zb action: cb_id=0x%x (unhandled)", cb_id);
            break;
    }
    return ESP_OK;
}

// ── Coordinator endpoint setup ──────────────────────────────────────────────

static esp_zb_cluster_list_t *make_clusters(void) {
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x04,   // mains
    };
    esp_zb_cluster_list_add_basic_cluster(
        clusters, esp_zb_basic_cluster_create(&basic_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t id_cfg = {
        .identify_time = 0,
    };
    esp_zb_cluster_list_add_identify_cluster(
        clusters, esp_zb_identify_cluster_create(&id_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // OnOff server — what the button binds to and sends commands at.
    esp_zb_on_off_cluster_cfg_t on_off_cfg = {
        .on_off = 0,
    };
    esp_zb_cluster_list_add_on_off_cluster(
        clusters, esp_zb_on_off_cluster_create(&on_off_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // We also want OnOff/Level/Color client clusters so we can send to the bulb.
    esp_zb_cluster_list_add_on_off_cluster(
        clusters, esp_zb_on_off_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    // IAS Zone CLIENT cluster (the CIE side). This is mandatory: without it,
    // ZBOSS's ZCL dispatcher walks the endpoint cluster list looking for
    // 0x0500, fails to find it, and emits a Default Response with status
    // UNSUP_CLUSTER_COMMAND for every Zone Status Change Notification we
    // receive — which the 3RSB22BZ interprets as "wrong CIE" and triggers
    // a network rejoin. See research.md §URGENT Finding 1.
    esp_zb_cluster_list_add_ias_zone_cluster(
        clusters, esp_zb_ias_zone_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    // NOTE: we deliberately do NOT register the OTA Upgrade server cluster.
    // The ESP-Zigbee-SDK's OTA server implementation asserts when a Query
    // Next Image Request arrives if no file is configured (zboss
    // zcl_ota_upgrade_srv_commands.c:132). Instead we intercept OTA frames
    // in zb_aps_indication and respond manually with NO_IMAGE_AVAILABLE.
    return clusters;
}

static void zigbee_task(void *pv) {
    (void)pv;

    esp_zb_cfg_t zb_nwk_cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_COORDINATOR,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = { .max_children = 10 },
    };
    esp_zb_init(&zb_nwk_cfg);

    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = COORDINATOR_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_SWITCH_DEVICE_ID,
        .app_device_version = 0,
    };

    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    esp_zb_ep_list_add_ep(ep_list, make_clusters(), ep_cfg);
    esp_zb_device_register(ep_list);

    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_aps_data_indication_handler_register(zb_aps_indication);

    // ── CRITICAL fixes per research2.md ────────────────────────────────────
    // Default end-device aging timeout on the coordinator side is 10 SECONDS
    // (ESP_ZB_ED_AGING_TIMEOUT_10SEC). With nothing else changed, the
    // coordinator silently evicts a sleepy child after 10 s of no useful
    // keepalive, which is exactly what causes the 14.5 s leave loop we
    // were chasing. Bump to 64 minutes.
    esp_zb_nwk_set_ed_timeout(ESP_ZB_ED_AGING_TIMEOUT_64MIN);

    // Disable the requirement that joining devices complete a Trust Center
    // link-key exchange. ESP-Zigbee SDK issue #21 identified this as the
    // cause of "coordinator sends Leave after association" for non-Z3
    // devices. Many older ZB3.0 sensors (including some Third Reality
    // firmware revisions) don't complete the new TCLK exchange cleanly.
    esp_zb_secur_link_key_exchange_required_set(false);

    ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(ESP_ZB_PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

// ── Boot ────────────────────────────────────────────────────────────────────

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    log_init();
    log_drainer_start();   // LAMP_UI now flows through a queue, not direct printf

    LAMP_LOGI("─── lamp boot ───");

    schedule_init();
    rtc_init();
    bulb_init();
    button_init();
    state_init();
    commissioning_init();

    ui_banner();

    console_init();

    // Zigbee platform config — native radio on the C6.
    esp_zb_platform_config_t platform = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform));

    xTaskCreate(zigbee_task,        "zb",       4096, NULL, 5, NULL);
    xTaskCreate(boundary_task,      "boundary", 2048, NULL, 4, NULL);
    // Press dispatch task at priority 6 (above the Zigbee task) so a press
    // pulls the Zigbee lock as soon as ZBOSS releases it. State machine and
    // bulb commands run here, off the APS callback's critical path.
    xTaskCreate(press_dispatch_task, "press",   4096, NULL, 6, &s_press_task);

    LAMP_LOGI("─── boot complete ───");
}
