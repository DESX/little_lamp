# THIRDREALITY Smart Button 3RSB22BZ

Battery-powered (coin cell), sleepy Zigbee end device. The physical button
the user mounts on the wall. **The model number alone does not identify the
device's behavior** — see "Firmware modes" below.

## Identification

| Field | Value |
|---|---|
| Model identifier (ZCL Basic 0x0005) | `"3RSB22BZ"` (exact, no padding) |
| Marking on back plate | `3RSB22BZ` |
| Manufacturer string (ZCL Basic 0x0004) | `"Third Reality, Inc"` — **comma, period after Reality, no period after Inc** |
| Manufacturer code (node descriptor) | `0x1233` (4659 decimal) — Telink Semi |
| Logical type | End Device, sleepy (RxOffWhenIdle) |
| Power source | **2× AAA batteries** (not a coin cell — earlier 3RSB015BZ revisions used CR2032 but this revision is AAA) |
| Physical form | White plastic puck, single round front button. Battery cover slides off the back; inside is the PCB. |
| OUI (IEEE first 3 bytes, MSB first) | varies; the lower 8-byte IEEE for our unit is `b4:0e:06:06:56:46:ff:ff` reverse-encoded as `ffffb40e06065646` on the wire (little-endian) |

## Firmware modes — THIS IS THE BIG ONE

The same Amazon SKU ships in two distinct firmware modes. The visible
button is identical. Behavior is completely different:

### Echo mode (DEFAULT factory state, slow)

- Marketed as "Amazon Echo Smart Button compatible."
- Emulates a Third Reality motion sensor on the IAS Zone protocol.
- Endpoint 1, profile `0x0104` (HA), **device type `0x0402` (IAS Zone Sensor)**.
- Input clusters: `0x0000` Basic, `0x0001` Power Configuration, `0x0500` IAS Zone.
- Output cluster: `0x0019` OTA Upgrade.
- No `0xFF01` cluster exists — writing `cancelDoubleClick` returns ZCL status `0x86 UNSUPPORTED_ATTRIBUTE`.
- **Press latency: ~1.5–2 seconds.** Built into the motion-sensor firmware path. Not adjustable.
- Pairing-mode LED: **alternating blue + red blink.**
- Press-time LED: brief **red** flash.
- Sends Zone Status Change Notification (cluster 0x0500, cmd 0x00) on press-down (alarm bit 1) and release (alarm bit 0).
- Useless for a snappy room button.

### Standard mode (snappy, what we want)

- Endpoint 1, profile `0x0104` (HA), **device type `0x0006` (On/Off Switch)**.
- Input clusters: `0x0000` Basic, `0x0001` Power Configuration, `0x0012` Multistate Input, sometimes `0xFF01` Third Reality private.
- Output clusters: `0x0006` OnOff, `0x0008` Level Control, `0x0019` OTA.
- **Press latency: tens of milliseconds.**
- Pairing-mode LED: **blue blink only** (no red).
- Press-time LED: brief **blue** flash.
- Sends **two frames per physical press**:
  1. OnOff cluster-specific command `On`/`Off`/`Toggle` (cmd 0x00/0x01/0x02 on cluster 0x0006) — intended for any bound smart bulbs to react directly.
  2. Multistate Input "Report Attributes" (global cmd 0x0a) on cluster 0x0012 with attr 0x0055 (`presentValue`) set to one of:
     - `1` = single click
     - `2` = double click
     - `0` = hold
     - `255` = release
- **You must handle exactly one of the two** or every press toggles state twice (cancels out). Recommended: ignore the OnOff frames, parse the Multistate report. That lets us distinguish single / double / hold cleanly.

### Switching from Echo mode to Standard mode

This is a **physical gesture on the button**, not a software command. Required exactly once per unit.

**Where the "reset button" is**: slide off the back battery cover. The PCB
inside has a small tactile button next to the AAA holders — that's the
reset button. The front face of the device only has the one big primary
button; the reset is hidden inside.

| Button firmware (Basic attr 0x4000) | Gesture |
|---|---|
| ≥ 1.00.22 | Press the **PCB reset button 5 times within 5 seconds** |
| < 1.00.22 | Hold the **front button + PCB reset button simultaneously for 5 seconds** |

You'll see the pairing-mode LED change from blue+red to blue-only. Then re-pair the button — it'll come up as device_type 0x0006 with Multistate Input.

To switch back to Echo mode: repeat the same gesture.

## Pairing procedure

Long-press the front button for ~10 seconds until the LED starts blinking
rapidly. Open the coordinator's join window first (180 s window). Button
joins within a few seconds.

If long-press of 10 s doesn't trigger pairing mode, try a triple-quick-press
of the front button.

## Battery

2× AAA. Slide off the back cover to access. The Power Configuration
cluster (0x0001) reports `batteryPercentageRemaining` (attr 0x0021)
periodically; we get those reports in the verbose log as
`rx frame: cluster=0x0001 cmd=0x0a`.

## Programming quirks specific to this button

### Multistate-mode dual-frame deduplication

In standard mode, each press fires both an OnOff command AND a Multistate
report. If the coordinator handles both as "press," every tap toggles state
twice. Our firmware filters: only Multistate Input (cluster 0x0012, global
cmd 0x0a, attr 0x0055, value=1) counts as a press; OnOff frames from the
button are ignored.

### Multistate Report payload layout

Standard ZCL Report Attributes:
- byte 0: frame control (typically `0x18` — global, server→client, disable default response)
- byte 1: TSN
- byte 2: cmd_id = `0x0a` (Report Attributes)
- byte 3-4: attribute_id (LE) = `0x55 0x00` for presentValue
- byte 5: data type = `0x21` (uint16)
- byte 6-7: value (LE uint16): `0x0001` = single, `0x0002` = double, `0x0000` = hold, `0x00FF` = release

### Echo-mode IAS Zone enrollment never completes (and it doesn't matter)

In Echo mode the button repeatedly sends Zone Enroll Requests (cluster
0x0500, cmd 0x01) and never accepts our Enroll Responses no matter how
we frame them — typed wrapper, custom cluster cmd, raw APS data, all
verified rejected by reading back `zoneState` (always 0x00). The IAS Zone
auto-enrollment dance from zigbee-herdsman doesn't work on this firmware.

**But this doesn't break anything.** The button sends Zone Status Change
Notifications regardless of enrollment state. Just receive them and treat
alarm-bit transitions 0→1 as presses. Don't waste time getting enrollment
to succeed — it won't.

### Periodic OTA Query Next Image Request

Every ~10-20 seconds the button sends OTA cmd 0x01 on cluster 0x0019. If
the coordinator doesn't have an OTA server registered, ZBOSS replies with
`UNSUP_CLUSTER_COMMAND` Default Response. **The button does NOT leave the
network over this** (we confirmed — leaves were caused by the 10 s aging
timeout, not OTA). Just suppress the default response by returning `true`
from the APS callback for cluster 0x0019 cmd 0x01.

### Echo-mode rapid-rejoin loop

In Echo mode the button used to disappear from the network every ~14.5 s.
**This is NOT the button's fault — it's the coordinator's default `ESP_ZB_ED_AGING_TIMEOUT_10SEC`.**
Set the coordinator's `esp_zb_nwk_set_ed_timeout(ESP_ZB_ED_AGING_TIMEOUT_64MIN)`
before `esp_zb_start()` and the loop goes away. See `devices/xiao-esp32-c6.md`.

### Phantom presses from ZDP Match Descriptor

When the button rejoins it sends a Match Descriptor Request (cluster 0x0006
on profile 0x0000 / ZDP). Cluster ID 0x0006 in ZDP profile collides with
OnOff cluster ID 0x0006 in HA profile. **Filter on `profile_id == 0x0104`**
before treating cluster 0x0006 as a button event.

### Frame-type filter (global vs cluster-specific)

The button's Read Attributes Response (global ZCL cmd 0x01) on cluster
0x0500 looks identical in cmd_id to a cluster-specific Zone Enroll Request
(also cmd 0x01). **Check frame control bits 0-1**: `00` = global, `01` =
cluster-specific. Without this filter you'll send bogus Enroll Responses
to your own attribute-read responses.

### APS retries reuse the original TSN

If our coordinator is slow to APS-ACK, the button retransmits with the same
ZCL TSN. Dedup by `(src_short, cluster_id, cmd_id, tsn)` in a small ring
buffer, not by time-window.

## Sources

- zigbee-herdsman-converters `src/devices/third_reality.ts` lines 611-748 (definitions for 3RSB015BZ, 3RSB22BZ, 3RSB01085Z) — https://github.com/Koenkk/zigbee-herdsman-converters/blob/master/src/devices/third_reality.ts
- zigbee-herdsman-converters `src/converters/fromZigbee.ts` `itcmdr_clicks` — the canonical Multistate Input parser for this device family
- zha-device-handlers `zhaquirks/thirdreality/button.py` — ZHA quirk
- 3RSB22BZ user manual — https://manuals.plus/thirdreality/3rsb22bz-smart-button-manual
- ThirdReality release notes — https://thirdreality.com/release-note/
- z2m issue #29612 — IAS variant doesn't pass correct triggers
- ZHA issue #2576 — IAS variant closed "not planned"
- HA Community: "Two Identical Third Reality 3RSB22BZ Smart Buttons. Only one gets the correct Quirk" — https://community.home-assistant.io/t/.../806999 (revealed the two-variants situation)
- Hubitat thread (2022, Mike Maxwell) — confirmed Echo-mode → standard-mode gesture for firmware ≥ 1.00.22
- SmartThings community (2023, Corey Stup) — same gesture, independent confirmation
- HA Community (2025, Djnotleks) — Echo mode → standard mode procedure
