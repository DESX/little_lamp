#include "zb.h"

#include <inttypes.h>
#include <string.h>

#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "aps/esp_zigbee_aps.h"
#include "test/esp_zigbee_test_utils.h"  // esp_zb_nwk_set_ed_timeout
#include "esp_zigbee_secur.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "commissioning.h"
#include "enrollment.h"
#include "log.h"

// SDK exception: ESP-Zigbee SDK invokes esp_zb_app_signal_handler and
// the APS/action callbacks by symbol name; no user-data pointer is
// threaded through. We stash the application's commissioning_t at
// zigbee_start time and reach into it from those callbacks. Storage
// lives in main; this is a cached pointer.
static commissioning_t *s_commissioning = NULL;

// ── Tuning knobs ────────────────────────────────────────────────────────────

#define COORDINATOR_ENDPOINT    1
#define JOIN_WINDOW_SECS        180

// Pin to Zigbee channel 25 (2475 MHz). Sits above Wi-Fi 2.4 GHz channels
// 1/6/11, so it avoids the worst of Wi-Fi interference. To scan all
// channels instead, use 0x07FFF800.
#define PRIMARY_CHANNEL_MASK    (1u << 25)

// ── Cluster IDs relevant to incoming frames ─────────────────────────────────

#define IAS_ZONE_CLUSTER             0x0500
#define IAS_ZONE_CMD_STATUS_CHANGE   0x00
#define IAS_ZONE_CMD_ENROLL_REQUEST  0x01

#define OTA_CLUSTER                  0x0019
#define OTA_CMD_QUERY_NEXT_IMAGE_REQ 0x01

// ── Frame dedup ring ────────────────────────────────────────────────────────
// APS retries re-use the original ZCL TSN, so a tuple of
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

// ── APS-level frame classifier ──────────────────────────────────────────────
// Decides whether each frame is a button press, an IAS Zone enroll request,
// an OTA query, or something to ignore. Calls into on_button_press_event()
// (in main.c) for the only event the application cares about.

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

    if (frame_is_duplicate(ind.src_short_addr, ind.cluster_id, cmd_id, tsn)) {
        LAMP_LOGI("rx frame: dropped duplicate (tsn=%u)", tsn);
        return false;
    }

    // IAS Zone status change: rising edge of the alarm bit = press. Filters
    // out releases (1→0), retransmissions of the same edge, and frames where
    // only non-alarm bits flipped (supervision pings, etc.).
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
        LAMP_LOGI("ias zone status=0x%04x alarm=%d", zone_status, alarm);
        is_press = alarm && !s_prev_alarm;
        s_prev_alarm = alarm;
    } else if (cluster_specific &&
               ind.cluster_id == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
               ind.profile_id == 0x0104 &&
               (cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_ON_ID ||
                cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID ||
                cmd_id == ESP_ZB_ZCL_CMD_ON_OFF_TOGGLE_ID)) {
        // Standard-mode button fires this immediately on each press —
        // before the ~300 ms single-vs-double click discrimination wait
        // it would do on the Multistate Input report path. We listen here
        // for absolute minimum latency. Tradeoff: single == double == toggle
        // (we cannot distinguish them).
        is_press = true;
    }
    // The Multistate Input report path (cluster 0x0012) is intentionally
    // unhandled — it carries the same logical event ~300 ms later. If
    // future use cases need to distinguish double-press / hold, listen
    // there instead.

    if (is_press) {
        on_button_press_event();
    }
    if (cluster_specific &&
        ind.cluster_id == IAS_ZONE_CLUSTER &&
        cmd_id == IAS_ZONE_CMD_ENROLL_REQUEST) {
        enrollment_handle_enroll_request(ind.src_short_addr, ind.src_endpoint);
    }
    if (cluster_specific &&
        ind.cluster_id == OTA_CLUSTER &&
        cmd_id == OTA_CMD_QUERY_NEXT_IMAGE_REQ) {
        // Suppress ZBOSS's default UNSUP_CLUSTER_COMMAND reply.
        LAMP_LOGI("ota: query suppressed");
        return true;
    }
    return false;  // let the stack process normally otherwise
}

// ── Signal handler — Zigbee BDB / NWK / ZDO lifecycle events ────────────────

static void retry_form_network(uint8_t mode) {
    esp_zb_bdb_start_top_level_commissioning(mode);
}

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
                if (!commissioning_complete(s_commissioning)) {
                    LAMP_LOGI("zb: opening join window (%ds) + F&B target",
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
            LAMP_LOGI("zb: steering done, opening join window (%ds) + F&B target",
                      JOIN_WINDOW_SECS);
            esp_zb_bdb_open_network(JOIN_WINDOW_SECS);
            esp_zb_bdb_finding_binding_start_target(COORDINATOR_ENDPOINT,
                                                   JOIN_WINDOW_SECS);
            break;

        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: {
            esp_zb_zdo_signal_device_annce_params_t *anc =
                (esp_zb_zdo_signal_device_annce_params_t *)
                    esp_zb_app_signal_get_params(p_sg_p);
            commissioning_on_device_announce(s_commissioning,
                                             anc->device_short_addr,
                                             anc->ieee_addr);
            break;
        }

        case ESP_ZB_NWK_SIGNAL_PERMIT_JOIN_STATUS:
            LAMP_LOGI("zb: permit-join status changed");
            break;

        default:
            LAMP_LOGI("zb: signal 0x%x status=0x%x", sig_type, err_status);
            break;
    }
}

// ── Action handler — for ZCL response/diagnostic callbacks ──────────────────

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t cb_id,
                                   const void *message) {
    switch (cb_id) {
        case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID: {
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
            LAMP_LOGI("zb action: cb_id=0x%x (unhandled)", cb_id);
            break;
    }
    return ESP_OK;
}

// ── Coordinator endpoint cluster registration ───────────────────────────────

static esp_zb_cluster_list_t *make_clusters(void) {
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version  = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x04,   // mains
    };
    esp_zb_cluster_list_add_basic_cluster(
        clusters, esp_zb_basic_cluster_create(&basic_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_cluster_list_add_identify_cluster(
        clusters, esp_zb_identify_cluster_create(&id_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // OnOff server — what the button binds to and sends commands at.
    esp_zb_on_off_cluster_cfg_t on_off_cfg = { .on_off = 0 };
    esp_zb_cluster_list_add_on_off_cluster(
        clusters, esp_zb_on_off_cluster_create(&on_off_cfg),
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // OnOff client — what we use to send commands to the bulb.
    esp_zb_cluster_list_add_on_off_cluster(
        clusters, esp_zb_on_off_cluster_create(NULL),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    // IAS Zone CLIENT cluster (the CIE side). Mandatory: without it, ZBOSS
    // emits a Default Response with UNSUP_CLUSTER_COMMAND for every Zone
    // Status Change Notification we receive — which Third Reality buttons
    // interpret as "wrong CIE" and trigger a network rejoin.
    esp_zb_cluster_list_add_ias_zone_cluster(
        clusters, esp_zb_ias_zone_cluster_create(NULL),
        ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);

    // NOTE: we deliberately do NOT register the OTA Upgrade server cluster.
    // The ESP-Zigbee-SDK's OTA server asserts on the first Query Next Image
    // Request when no file is configured. We intercept OTA frames in the
    // APS handler above and suppress the default response instead.
    return clusters;
}

// ── Zigbee main loop task ───────────────────────────────────────────────────

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

    // End-device aging timeout: default is 10 SECONDS, which silently
    // evicts sleepy children. 64 minutes is the right "house-scale" choice.
    esp_zb_nwk_set_ed_timeout(ESP_ZB_ED_AGING_TIMEOUT_64MIN);

    // Disable the TCLK exchange requirement so older ZB3.0 devices can
    // join without sending a leave shortly after.
    esp_zb_secur_link_key_exchange_required_set(false);

    ESP_ERROR_CHECK(esp_zb_set_primary_network_channel_set(PRIMARY_CHANNEL_MASK));
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void zigbee_start(commissioning_t *commissioning) {
    s_commissioning = commissioning;
    esp_zb_platform_config_t platform = {
        .radio_config = { .radio_mode = ZB_RADIO_MODE_NATIVE },
        .host_config  = { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE },
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform));
    xTaskCreate(zigbee_task, "zb", 4096, NULL, 5, NULL);
}
