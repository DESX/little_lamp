# innr A19 Smart Bulb (AE 287 C-2)

Mains-powered, full-color RGBW Zigbee bulb. Acts as a Zigbee router (always
on, can serve as parent for sleepy children).

## Identification

| Field | Value |
|---|---|
| Model identifier | AE 287 C-2 (printed on the bulb base) |
| Vendor | innr |
| Form factor | A19 / E26 standard screw bulb |
| Chip | Telink Semiconductor — **OUI `a4:c1:38`** (first 3 bytes of IEEE, in big-endian display order). On the wire as little-endian: `... 38 c1 a4`. |
| Power | E26 mains, ~9 W typical |
| Logical type | Zigbee Router (mains-powered, mesh-extending) |

## Endpoints and clusters

Endpoint 1, profile `0x0104` (HA), device type `0x0102` (Color Dimmable Light)
or similar color-capable light type.

| Cluster | Role | Notes |
|---|---|---|
| 0x0000 Basic | Server | Manuf/model strings, ZCL version, etc. |
| 0x0003 Identify | Server | Used during commissioning |
| 0x0004 Groups | Server | |
| 0x0005 Scenes | Server | |
| 0x0006 On/Off | Server | Plain on/off |
| 0x0008 Level Control | Server | 1-254 brightness, supports transition time |
| 0x0300 Color Control | Server | Both color temp (mireds) AND xy color modes |
| 0x1000 Touchlink | Server | |

## Factory reset procedure

**The manual's documented sequence is WRONG for this specific bulb.** The
working sequence is **opposite to most online instructions**:

- **OFF for 2+ seconds**
- **Short ON burst (well under 1 s, basically a flick)**
- Repeat the OFF-then-flick cycle 6 times
- After the 6th cycle, leave it ON

Confirmation: bulb does a brief brightness sweep / flash within ~1 s of
the final ON. After that it scans for a network for ~3 minutes.

The "common" 5-second-ON / 2-second-OFF sequence that most innr docs
describe does **not** trigger reset on this specific unit. We verified
empirically — a YouTube tutorial described the working sequence.

## Pairing

Once factory-reset and searching, the bulb auto-joins any open Zigbee
network on any 802.15.4 channel. No special pairing gesture needed.

After joining it sends a Device Announce (ZDP cluster 0x0013) immediately
plus a Permit Joining piggyback. Then it goes quiet — routers don't send
periodic keep-alive frames like sleepy end devices do.

## Commanding the bulb

Three independent attribute groups. Each takes a separate Zigbee command,
and each has its own latency. Our firmware tracks last-sent state and
skips redundant commands.

### Turn on / off

`esp_zb_zcl_on_off_cmd_req()` with `ESP_ZB_ZCL_CMD_ON_OFF_ON_ID` or
`OFF_ID`. Instant — no transition time on this command.

### Set brightness

`esp_zb_zcl_level_move_to_level_cmd_req()`. Level 0-254. **Cmd 0x00
(MoveToLevel) only changes the level; cmd 0x04 (MoveToLevelWithOnOff) ALSO
changes the on/off state based on the level.** We use 0x00 and manage
on/off separately.

`transition_time = 0` means instant. We use 0 throughout.

### Set color

Two modes:
- **Color temperature** (mireds): `esp_zb_zcl_color_move_to_color_temperature_cmd_req()`. We use ~370 mireds (~2700 K, warm white).
- **xy chromaticity**: `esp_zb_zcl_color_move_to_color_cmd_req()`. xy values are 16-bit fixed point (0.0-1.0 mapped to 0-65535). We use (0.675, 0.322) for deep red.

The bulb supports either mode and switches automatically.

## Programming quirks specific to this bulb

### Default Response feedback loop

After every command we send, the bulb replies with a ZCL Default Response
(global cmd 0x0b) on the same cluster. **If the coordinator treats those
Default Responses as button presses, you get an infinite feedback loop**
that toggles state at ~10 Hz, looking like a strobing red/warm flicker on
the bulb. Filter: cluster-specific commands only (frame type bits = `01`),
not global (`00`).

This was a real bug we hit. The first time we tried to drive the bulb, our
APS-level press detector grabbed cluster 0x0006 / cmd 0x0b frames as
button presses and produced 600+ phantom presses in under a minute.

### Three commands per "warm white" or "dim red" state

The bulb has separate state for on/off, level, and color. To go from "off"
to "on, warm white, full brightness" requires three frames over the air:

1. ColorTemp (if last color wasn't warm)
2. Level (if last level wasn't 254)
3. OnOff (if not already on)

Each frame is ~50-100 ms airtime. Sending all 3 takes ~300-500 ms perceived.
**Cache last-sent state and skip redundant commands** — common-case toggles
become a single frame.

### Pre-stage color/level before OnOff for clean visual transitions

If you send OnOff(on) first, then change color, you see the bulb light up
in the previous color, then jump to the new color. Send color and level
FIRST (the bulb accepts these while off and remembers them), then OnOff
last — the bulb snaps to the right color/brightness immediately on power.

### Routes through the bulb if the button picks it as parent

Once paired, the bulb is a valid parent for sleepy children. If the button
joins later, it may select the bulb as its parent (the bulb has stronger
signal than the C6 in many setups). When that happens, button presses go
button → bulb (forwarded as router) → coordinator. The bulb handles the
indirect-tx queue for the button, which is usually more reliable than the
coordinator doing it itself.

### Network state is sticky

The bulb keeps its joined-network state even through brief mains outages
(< 5 min or so). If the coordinator is reformed, the bulb won't auto-rejoin
the new network — you have to factory-reset the bulb again.

If you `esp_zb_factory_reset()` the coordinator, the bulb's stored network
credentials become invalid; the bulb will try the old network for a while
before giving up and going dormant. The cleanest re-pair is: factory-reset
both coordinator and bulb together.

## Sources

- innr A19 product page — https://www.innr.com/
- ZCL Cluster Library Specification, Color Control cluster (8.0)
- ESP-Zigbee-SDK `esp_zigbee_zcl_color_control.h` and `esp_zigbee_zcl_level_control.h`
- Empirical: extensive bench testing during this project
