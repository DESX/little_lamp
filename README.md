# lamp

Standalone Zigbee firmware for a XIAO ESP32-C6 that turns a wall button into
a one-press toggle for a smart bulb, with a dim-red night-light "off" state
between 19:30 and 06:40.

| Doc | What's in it |
|---|---|
| [USER_STORY.md](USER_STORY.md) | What it should do (the requirements). |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the firmware is structured. |
| [PHILOSOPHY.md](PHILOSOPHY.md) | The principles the design follows. |
| [PROTOCOL.md](PROTOCOL.md) | Zigbee / ESP-Zigbee-SDK / ZCL findings that aren't tied to specific hardware. |
| [devices/xiao-esp32-c6.md](devices/xiao-esp32-c6.md) | Coordinator hardware quirks, build setup, sdkconfig must-haves. |
| [devices/thirdreality-3rsb22bz.md](devices/thirdreality-3rsb22bz.md) | Button — including the critical Echo-mode-to-standard-mode gesture. |
| [devices/innr-a19-ae287c2.md](devices/innr-a19-ae287c2.md) | Bulb — including the non-obvious factory-reset sequence. |

A future-you (or future LLM) should be able to rebuild this project from
those documents without other context.

## Build & flash

```
make setup           # one-time: apt prereqs + dialout group + serial perms
make flash-monitor   # build, flash, open serial log
```

First run downloads ESP-IDF v5.4.4 and the C6 toolchain (~1 GB, one-time).

## Pairing

The first device to join becomes the **button**, the second becomes the
**bulb**. Order matters — pair the button first.

1. `make erase-nvs && make flash-monitor` — wipes any prior bindings, flashes,
   forms a fresh Zigbee network, opens a 180 s join window in F&B target mode.
   Watch the log for `formed network ch=… pan=…`.
2. **Button (3RSB022Z)** — press and hold for ~10 s (or quick triple-press)
   until its LED blinks rapidly. The log should show
   `commissioning: assigned button 0x…`.
3. **Bulb (innr A19)** — power-cycle 6 times: ON for 5 s, OFF for 2 s,
   repeated. After the 6th ON it flashes to confirm the factory reset, then
   joins. The log should show `commissioning: assigned bulb 0x…`.

Bindings persist across reboots; subsequent boots skip pairing.

To start over: `make erase-nvs && make flash`.

## Operation

Press the button — light toggles. During night-mode hours, "off" is a dim red
glow instead of fully dark. See [ARCHITECTURE.md](ARCHITECTURE.md) for the
state machine and all configurable values.

## Logging

By default the serial output is curated to a handful of lines: pairing
events, button presses, and a status banner. The full SDK and diagnostic
stream is hidden.

At the `lamp>` console prompt over `make monitor`:

```
set-verbose 1          # turn on detailed LAMP_LOGI / SDK logs
set-verbose 0          # back to quiet (default)
status                 # reprint the status banner
reset                  # reboot the device
reset-commissioning    # wipe pairings, reboot, re-pair fresh
set-time <epoch>       # set the RTC
```

The verbose setting persists in NVS across reboots.
