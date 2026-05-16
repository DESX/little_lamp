# Zigbee + ESP-Zigbee-SDK Protocol Notes

Device-agnostic findings from building this coordinator. For device-specific
quirks see `devices/`.

## 1. ZBOSS / ESP-Zigbee-SDK setup that must be right

### End-device aging timeout

**Default is `ESP_ZB_ED_AGING_TIMEOUT_10SEC` (10 seconds).** If you don't
override this, a sleepy end device gets evicted from the coordinator's
child table 10 seconds after its last useful frame. The child then has to
rejoin — looking from outside like a 14-second-cycle "the device keeps
leaving the network" loop.

```
#include "test/esp_zigbee_test_utils.h"
esp_zb_nwk_set_ed_timeout(ESP_ZB_ED_AGING_TIMEOUT_64MIN);
```

Must be called before `esp_zb_start()`. The header is in `test/` but the
API is production-callable. Enum values: 10SEC, 2MIN, 4MIN, 8MIN, 16MIN,
32MIN, 64MIN, 128MIN, 256MIN, 512MIN.

This was the single largest bug we hit in the project. Symptom: sleepy
device explicitly sends NWK_LEAVE_INDICATION (signal 0x13) every ~14
seconds, then rejoins via a fresh device announce with a new short address.
Looks like the device is misbehaving; it's actually our parent-side child
management timing out.

### Trust Center Link Key exchange

```
esp_zb_secur_link_key_exchange_required_set(false);
```

Set to `false` for compatibility with older Zigbee-3.0 devices that don't
complete the modern TCLK exchange cleanly. Per ESP-Zigbee-SDK issue #21,
some sensors send a network LEAVE shortly after joining if you leave this
at the default. The fix is essentially a downgrade-for-compatibility flag.

### Pin a single channel away from Wi-Fi

```
#define ESP_ZB_PRIMARY_CHANNEL_MASK (1u << 25)   // not 0x07FFF800
```

Zigbee channels 11-24 overlap with Wi-Fi channels 1, 6, 11. Channel 25
(2475 MHz) and 26 (2480 MHz) sit above where Wi-Fi typically operates.
Pinning to 25 avoids the common scenario where a coordinator next to a
Wi-Fi router has poor range. Forms a fresh network on that channel only;
existing networks are unaffected.

### Required NVS partition layout

ZBOSS persists its network state in a partition called `zb_storage`. If
this partition doesn't exist, **the coordinator does not retain its
network across reboots and you have to re-pair everything on every boot**.
The partition must be `data, fat` type, 16 KB minimum. Plus an optional
`zb_fct` for factory commissioning data.

See `partitions.csv` in this project.

### Don't register OTA Upgrade server cluster unless you have an image

`esp_zb_ota_cluster_create(&cfg)` with a zeroed cfg, added as
`ESP_ZB_ZCL_CLUSTER_SERVER_ROLE`, crashes the firmware on the first OTA
Query Next Image Request from any device: `Zigbee stack assertion failed
zcl/zcl_ota_upgrade_srv_commands.c:132`. Either:

1. Don't register OTA. Intercept Query Next Image Request frames in the
   APS callback (cluster 0x0019, cluster-specific cmd 0x01) and return
   `true` to suppress the default response. Optionally send back your own
   Query Next Image Response with status `0x98 NO_IMAGE_AVAILABLE`.
2. Implement a real OTA server with image data.

We do (1).

### Registering IAS Zone CLIENT cluster

If you act as a CIE (Control & Indicating Equipment — i.e. the receiver of
IAS Zone Status Change Notifications), **you must register cluster `0x0500`
as `ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE` on your endpoint.** Without it,
ZBOSS's ZCL dispatcher emits an UNSUP_CLUSTER_COMMAND Default Response
to every Zone Status Change Notification, which some devices interpret as
"wrong CIE" and trigger a leave.

```
esp_zb_cluster_list_add_ias_zone_cluster(
    clusters,
    esp_zb_ias_zone_cluster_create(NULL),
    ESP_ZB_ZCL_CLUSTER_CLIENT_ROLE);
```

Note: client role. Not server. The IAS Zone server lives on the sensor;
the client lives on the CIE.

## 2. ZCL frame parsing

The APS data indication callback hands you `esp_zb_apsde_data_ind_t` which
contains the raw ZCL frame payload (`ind.asdu` / `ind.asdu_length`). To
classify what kind of frame it is:

```
byte 0: Frame Control
  bits 0-1: Frame Type
    00 = Global ZCL command (Read Attr, Write Attr, Report Attr, Default Response, ...)
    01 = Cluster-specific command
  bit  2:  Manufacturer-specific (extends header with manuf code)
  bit  3:  Direction (0 = client→server, 1 = server→client)
  bit  4:  Disable Default Response
  bits 5-7: reserved

if manuf-specific:
  byte 1-2: Manufacturer Code (uint16 LE)
  byte 3:   TSN
  byte 4:   Command ID
  byte 5+:  Payload
else:
  byte 1:   TSN
  byte 2:   Command ID
  byte 3+:  Payload
```

**You must check Frame Type (cluster-specific vs global) before dispatching
on cmd_id.** A global Read Attributes Response (cmd 0x01) and a
cluster-specific Zone Enroll Request (cmd 0x01) are different things on the
same cluster. We hit this bug: every time we read an attribute from a
device, we mistakenly sent back a Zone Enroll Response treating the
attribute response as an enroll request.

## 3. APS retries and deduplication

APS-layer retransmits **re-use the original ZCL TSN.** If your coordinator
is slow to APS-ACK, the device sends the same frame again with the same
TSN.

**Dedup by `(src_short_addr, cluster_id, cmd_id, tsn)`** in a small ring
buffer (~8 entries is plenty). Do NOT dedup by time-window alone — slow
retries can arrive 3-4 seconds after the original, well outside any
reasonable debounce window, and you'd treat the retry as a fresh event.

Cluster-specific notifications (like Zone Status Change) often request
APS-ACK. The MAC-layer ACK happens automatically, but the SDK's higher-
level processing (and your callback) determine APS-ACK timing.

## 4. Cluster-ID collisions across profiles

Cluster IDs are not unique across profiles. In particular:

| Cluster ID | HA profile (0x0104) | ZDP profile (0x0000) |
|---|---|---|
| 0x0006 | OnOff | Match Descriptor Request |
| 0x0013 | (unused) | Device Announce |

**Always filter on `profile_id`** before dispatching on `cluster_id`. We
had a bug where every time a device sent a ZDP Match Descriptor Request
(part of network discovery), we registered a phantom button press because
we matched on cluster 0x0006 without checking profile.

## 5. IAS Zone enrollment

Canonical procedure (from `zigbee-herdsman/src/controller/model/device.ts`
lines 1049-1093):

1. After the device interview completes, on every endpoint that declares
   `ssIasZone` as input cluster:
2. Read attributes `iasCieAddr` (0x0010) and `zoneState` (0x0000).
3. If `iasCieAddr != coordinator.ieeeAddr` OR `zoneState != 1`, proceed.
4. Write attribute `iasCieAddr` with the coordinator's own IEEE.
5. Wait 500 ms (some devices need time to commit the CIE address before
   accepting an enroll response — z2m issue #4569).
6. Send unsolicited Zone Enroll Response (cmd 0x00, status SUCCESS,
   zone_id 23) with `disableDefaultResponse = true`.
7. Poll `zoneState` until it reads 1, up to 20 attempts × 500 ms.

Plus separately at runtime: respond to every incoming Zone Enroll Request
(cmd 0x01) with the same Enroll Response, matching the request's TSN.

**For some buttons (Third Reality 3RSB22BZ Echo mode), step 7 never
succeeds — zoneState stays 0 forever. The device sends Zone Status Change
Notifications anyway.** Don't gate functionality on successful enrollment.
Receive the notifications and treat them as events. See
`devices/thirdreality-3rsb22bz.md`.

### Manually constructing the Enroll Response frame

If the SDK's typed wrapper `esp_zb_zcl_ias_zone_enroll_cmd_resp()` doesn't
produce a frame your device accepts, drop down to raw APS data:

```
ASDU bytes:
  0x19          frame control = 0001 1001 (cluster-spec, server→client, dis_default_resp)
  <tsn>         transaction sequence number
  0x00          command id = Enroll Response
  0x00          enroll_response_code = SUCCESS
  0x17          zone_id = 23 (matches zigbee-herdsman)

esp_zb_apsde_data_req_t with:
  dst_addr_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT
  cluster_id = 0x0500
  profile_id = 0x0104
  tx_options = ESP_ZB_APSDE_TX_OPT_ACK_TX
```

## 6. Multistate Input — the snappy press-event cluster

Cluster 0x0012 is the cleaner way for a button to signal events. The
device sends ZCL Report Attributes (global cmd 0x0a) with attr 0x0055
(`presentValue`, uint16) set to a value-encoded action. Vendors map values
differently. Third Reality / `itcmdr_clicks` mapping:

- 0 = hold
- 1 = single click
- 2 = double click
- 255 = release

No enrollment, no debouncing on the coordinator side — just parse the
report and dispatch. This is what z2m's standard 3RSB22BZ handler uses.

## 7. Sleepy end devices and parent management

A sleepy end device (`RxOffWhenIdle`) cannot receive frames at arbitrary
times. It polls its parent on a long-poll interval (typically 7.5 s),
plus a short-poll burst (~1 s) right after sending its own frame. The
parent must:

1. Queue outgoing frames to the child in the **indirect transmission
   queue**. ZBOSS does this automatically.
2. Deliver the queued frame on the next data-poll from the child.
3. Hold the queued frame for at most the **indirect transmission timeout**
   (7.68 s default per spec). After that the frame is dropped.
4. Reset the **child aging timer** on every poll. ESP-Zigbee-SDK does this
   automatically IF the aging timeout is non-default — see "End-device
   aging timeout" above.

If the coordinator gets in the way of any of this — too aggressive child
aging, missed polls, dropped indirect-tx — the child rejoins.

## 8. Coordinator-side ZCL command APIs

For sending commands TO devices we have:

- `esp_zb_zcl_on_off_cmd_req()` — OnOff cluster
- `esp_zb_zcl_level_move_to_level_cmd_req()` — Level Control
- `esp_zb_zcl_color_move_to_color_cmd_req()` — Color (xy)
- `esp_zb_zcl_color_move_to_color_temperature_cmd_req()` — Color (mireds)
- `esp_zb_zcl_write_attr_cmd_req()` — generic Write Attributes
- `esp_zb_zcl_read_attr_cmd_req()` — generic Read Attributes
- `esp_zb_zcl_custom_cluster_cmd_req/resp()` — arbitrary cluster command
- `esp_zb_aps_data_request()` — raw APS frame, full byte control

All of them must be called between `esp_zb_lock_acquire(portMAX_DELAY)` and
`esp_zb_lock_release()` if called from outside the Zigbee task. Inside the
Zigbee scheduler (e.g. via `esp_zb_scheduler_alarm()`), the lock is already
held — don't re-acquire.

### The custom_cluster_cmd path has typed-data wrapping

`esp_zb_zcl_custom_cluster_cmd_t.data` has a `type`, `size`, `value`
triple. The SDK serializes based on type. **For sending raw command
payloads (no ZCL attribute encoding), don't use this — use
`esp_zb_aps_data_request` directly**. The custom-cluster path always
wraps with type info, which most cluster-specific commands don't want.

We use the custom path for OTA Query Next Image Response (with
ATTR_TYPE_U8 to send a single status byte) and raw APS for IAS Zone
Enroll Response.

## 9. Pairing flow on the coordinator side

```
ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP          → start_top_level_commissioning(INITIALIZATION)
ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START    → start_top_level_commissioning(NETWORK_FORMATION)
ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT (after persisted state restored)
                                        → if bindings incomplete:
                                            esp_zb_bdb_open_network(180)
                                            esp_zb_bdb_finding_binding_start_target(ep, 180)
ESP_ZB_BDB_SIGNAL_FORMATION (after fresh formation)
                                        → start_top_level_commissioning(NETWORK_STEERING)
ESP_ZB_BDB_SIGNAL_STEERING
                                        → open_network(180)
                                          finding_binding_start_target(ep, 180)
ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE          → a new device joined; assign role, kick off enrollment
```

F&B target mode advertises our endpoint's clusters via Identify so smart
buttons can complete their finding-and-binding handshake.

## 10. Debugging tips

- `set-verbose 1` console command toggles between WARN/INFO log levels.
- Every APS frame received gets a "rx frame" log with src, ep, cluster,
  profile, cmd, tsn, length — invaluable diagnostic.
- `idf.py monitor` strips terminal control codes nicely; raw `cat
  /dev/ttyACM0` does not.
- Sniff with a CC2531 + Wireshark OR an nRF52840 + nRF Sniffer when frame
  contents matter — Wireshark decodes ZCL natively.

## Sources

- ESP-Zigbee-SDK source (managed_components in this project)
- ZBOSS docs — https://ncsdoc.z6.web.core.windows.net/zboss/3.11.2.1/
- zigbee-herdsman — https://github.com/Koenkk/zigbee-herdsman
- ZCL Cluster Library spec (rev 8) — https://zigbeealliance.org/wp-content/uploads/2021/10/07-5123-08-Zigbee-Cluster-Library.pdf
- Silicon Labs Zigbee Application Framework Developer's Guide — sleepy devices
- ESP-Zigbee-SDK issues: #21, #115, #215, #462, #469, #568, #632, #673, #754, #771
- This project's `research.md` and `research2.md` (now archived — content distributed to this doc + `devices/`)
