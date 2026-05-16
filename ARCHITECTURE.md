# Architecture

This document describes how the system in [USER_STORY.md](USER_STORY.md)
is built. It is written against the principles in
[PHILOSOPHY.md](PHILOSOPHY.md) and is the source of truth from which
tests and implementation are derived.

## System overview

Three physical devices:

| Role | Hardware | Power | Network role |
|---|---|---|---|
| Coordinator + brains | Seeed Studio XIAO ESP32-C6 with soldered LiPo backup battery | USB-C (mains) + battery fallback | Zigbee coordinator, runs the application |
| Input | THIRDREALITY Smart Button 3RSB022Z | Coin cell | Sleepy Zigbee end device |
| Output | innr A19 Smart Bulb AE 287 C-2 | Mains (E26 socket) | Zigbee router |

All logic lives on the ESP32-C6. The PC is **only** used to compile,
flash, and debug firmware — it is not part of the runtime system. When
the daughter is using the room, the PC is off.

```
            ┌──────────────────────┐
            │  XIAO ESP32-C6       │
   USB-C ──▶│  (coordinator +      │
            │   battery backup)    │
            └─────────┬────────────┘
                Zigbee│ 2.4 GHz
                 ┌────┴────┐
                 │         │
            ┌────▼──┐  ┌───▼──────────┐
            │Button │  │  A19 bulb    │
            │(3RSB) │  │  (innr)      │
            └───────┘  └──────────────┘
```

## Software stack

| Layer | Choice | Rationale |
|---|---|---|
| Toolchain & framework | ESP-IDF (Espressif's official SDK) | First-party, actively maintained, supports ESP32-C6 Zigbee directly. Owning the Zigbee stack ourselves would be a vast multiplication of managed complexity. |
| Zigbee stack | ESP-Zigbee SDK (ZBOSS under the hood) | Bundled with ESP-IDF for the C6. Lets us write a coordinator in a few hundred lines. |
| Application language | C | What the SDK is in. No translation layer. |
| Build orchestration | GNU Make + `graft.mk` from `../graft` | One command interface, dependency-graph driven. Graft fetches and pins ESP-IDF locally so a CDN outage doesn't break our build. |
| Host runtime | None | The ESP32 is the entire runtime. |

ESP-IDF is fetched and cached via `graft`'s `FETCH` macro, pinned to a
specific commit. Patches (if any) live in `patches/` and are applied
deterministically by graft.

## Dependency graph

Every action goes through `make` at the project root. Targets:

| Target | What it does | Prerequisites |
|---|---|---|
| `make` (default: `build`) | Compile firmware | ESP-IDF fetched, sources |
| `make flash` | Build then flash over USB | `build`, device connected |
| `make monitor` | Open USB serial monitor | (none) |
| `make flash-monitor` | Flash then immediately monitor | `flash` |
| `make set-time` | Write current host wall-clock time into the device's RTC over USB serial | device connected |
| `make clean` | Remove build artifacts | (none) |
| `make distclean` | `clean` + drop cached ESP-IDF + erase device NVS (forces re-pair on next boot) | (none) |

Every prerequisite is declared. Running `make flash` will (in order)
fetch ESP-IDF if missing, build the firmware if sources changed, and
then flash — without the user needing to know that ordering.

If a step requires a manual action the build cannot perform (e.g.
"hold the BOOT button"), it fails immediately with a message telling
the user exactly what to do and which target to re-run.

## Firmware structure

Files under `src/`:

| File | Responsibility |
|---|---|
| `main.c` | App entry point. Bring up Zigbee, register callbacks, idle loop. |
| `state.c` / `state.h` | The ON ↔ OFF state machine. Pure logic, no I/O. |
| `schedule.c` / `schedule.h` | "Is it night-mode right now?" Given a `time_t`, returns a bool. |
| `bulb.c` / `bulb.h` | Send Zigbee Level Control / Color Control commands to the bulb's IEEE address. |
| `button.c` / `button.h` | Subscribe to On/Off cluster reports from the button's IEEE address; emit `BUTTON_PRESSED` events to the state machine. |
| `commissioning.c` | Open/close the join window, persist learned device addresses to NVS. |
| `rtc.c` | Read/set the on-chip RTC. Survives reset because the soldered battery keeps the SoC powered. |
| `log.c` | Single logging surface — every interesting event lands here, timestamped, on USB serial. |

No file is allowed to read the clock except `schedule.c` (via `rtc.c`).
No file is allowed to send Zigbee commands to the bulb except
`bulb.c`. `grep` finds every caller.

## State model

The state machine has exactly two states and one input:

```
            press
       ON ─────────▶ OFF─effective ◀──┐
       ▲                │              │ schedule boundary
       └────────────────┘              │ (re-render bulb)
            press                      │
                        └──────────────┘
```

Boot state: `OFF_effective`, with no command emitted on entry.

On entering `ON`:

- Send to bulb: on, level=100%, color temperature ~2700 K.

On entering `OFF_effective`, or whenever the schedule boundary is
crossed *while already in `OFF_effective`*, the bulb is re-rendered:

- If `schedule.is_night(now)`: send to bulb: on, level=1%, color=red.
- Else: send to bulb: off.

Boot-time: the state machine starts in `OFF_effective` and sends no
command on entry (per the user story, the first press is what wakes
the system up; the bulb's retained state from before the reboot is
left alone). The next button press toggles to `ON`. This is the
simplest first-press behavior the user asked for.

The schedule boundary is watched by a single one-shot timer scheduled
to fire at the next boundary (19:30 or 06:40, whichever is closer):

- If state is `OFF_effective` when it fires, re-render the bulb.
- If state is `ON` when it fires, do nothing to the bulb.
- Either way, reschedule the timer for the next boundary.

This is one timer for the lifetime of the firmware. No polling.

## Time keeping

The on-chip RTC is the only clock. It is kept alive across mains
outages by the soldered LiPo battery, which powers the C6 directly.

Initial time is written by the developer running `make set-time` over
USB serial. The firmware exposes a single ASCII command
(`set-time <epoch>`) on its USB serial console; `make set-time` simply
pipes the host's `date +%s` to it.

If the RTC is ever observed to be at epoch 0 (never set), the firmware
logs a loud warning and treats `is_night` as **false** (day mode), so
the worst-case unsynced behavior is "off button means actually off"
— never an inappropriate red glow at noon.

No NTP. No Wi-Fi. The C6 has a 2.4 GHz radio but we use it for Zigbee
only. Adding Wi-Fi for time sync would multiply managed complexity
for a feature whose worst-case failure (drift) is recoverable with a
single USB command once a year.

## Commissioning

Simplest possible flow: **the join window is open whenever the
expected device set is incomplete, and closed once both devices are
known.**

1. On boot, firmware reads the binding table from NVS. If it does not
   yet have *both* a button and a bulb bound, it opens the Zigbee
   join window.
2. User triggers each device's pairing sequence (per its manual);
   power-cycling the bulb is usually enough.
3. As each device joins, firmware logs the IEEE address and role,
   persists it to NVS, and (once both roles are filled) closes the
   join window.
4. Subsequent boots find both bindings in NVS and never open the
   window. There is no `make pair` target and no manual trigger
   needed.

To re-pair from scratch, run `make distclean` (or hold the BOOT button
during reset — same effect): firmware wipes the binding NVS partition
on next boot and the join window opens again.

## Responsiveness

Target: ≤ 250 ms from button click to visible light change.

Path: button radio → coordinator radio (~30 ms) → state-machine
handler (microseconds) → coordinator radio → bulb radio (~30 ms) →
bulb command execution (~50 ms). Headroom is comfortable.

No sleeps, no polling delays, no retry loops anywhere in this path.
Every step waits for a condition (radio frame received, ZCL ack), not
a duration. If the bulb does not ack a command within a reasonable
window, we log and drop — the next press is a fresh start.

## Idempotency

- **Build**: skips unchanged files (Make's job).
- **ESP-IDF fetch**: graft skips if the cached archive is intact.
- **Flash**: same image flashed twice is a no-op of consequence.
- **Pair**: a device already in the binding table is left alone.
- **Set-time**: each invocation overwrites the RTC; no accumulation.
- **State transitions**: each transition sends a command set; sending
  "on at 100%, warm white" twice in a row is harmless (the bulb is
  idempotent under repeated commands).

## Traceability

All interesting events land on the USB serial log with millisecond
timestamps:

- `BTN press   src=<ieee>`
- `STATE  ON → OFF (night-mode=true)`
- `BULB   off, dim-red 1%`
- `BULB   ack 12ms`
- `JOIN   <ieee> role=button`
- `RTC    set epoch=...`

If something goes wrong, plug in USB, run `make monitor`, press the
button. The log tells the story.

## Failure modes

| Failure | Behavior |
|---|---|
| Mains lost, battery holds | System keeps running. Bulb is dead until mains returns. Button presses are logged; commands fail with no ack and are dropped. |
| Battery and mains both lost | System reboots when power returns. RTC is lost — until `make set-time` runs again, day mode is assumed (safe default). |
| Bulb unplugged / unreachable | Press → command sent → no ack → logged and dropped. Next press tries fresh. |
| Button battery dies | No events arrive. State machine sits idle. Replacing the coin cell does not require re-pairing. |
| Firmware crash | ESP-IDF watchdog reboots the chip. Bindings restored from NVS, state machine restarts in `UNKNOWN`. |

There is no "limp along" mode anywhere. Anything unexpected fails
loudly to the log and waits for the next clean input.

## Tests

Per the philosophy, tests are derivable from this document. Three
tiers:

1. **Unit (`tests/unit/`)** — Pure logic, no hardware.
   - `schedule.is_night` boundary tests at 06:40, 19:30, midnight,
     noon, and the minute on either side.
   - State machine: press from ON → OFF_effective; press from OFF
     → ON; OFF_effective branches correctly on `is_night`.
2. **On-target (`tests/device/`)** — Flashed to a dev C6, exercises
   the Zigbee stack against simulated peer devices.
3. **End-to-end (`tests/e2e/`)** — Real button, real bulb, real C6.
   Press the button, observe the bulb. Run at known times-of-day or
   by temporarily overriding the RTC via `set-time`.

The end-to-end test is the one that proves the system works.

## Proposed defaults (awaiting confirmation)

Concrete values picked so implementation can start. Each is a small,
local change to flip later.

1. **Night-light look.** xy = (0.675, 0.322), level = 5/254 (~2%).
   Deep red inside the typical consumer-RGB gamut, dim enough for
   sleep but visible across a dark room.
2. **ON look.** Level = 254/254, color temperature = 2700 K
   (~370 mireds).
3. **Transitions.** Button-driven changes use a 2 s fade; scheduled
   boundary changes use a 5 s fade.
4. **Timezone.** Hard-coded POSIX TZ string
   `EST5EDT,M3.2.0,M11.1.0` (US Eastern, auto-DST). No tzdata files
   needed; libc handles the parse.
5. **LED indicator.** Solid on while the Zigbee join window is open,
   off otherwise. No press-feedback blinking.
6. **Button events.** Single-press only. The 3RSB022Z also reports
   double-press and long-press; those events are dropped at the
   button handler and never reach the state machine.
7. **Schedule editability.** Compile-time constants in
   `schedule.c`. No runtime `set-schedule` command. Schedule changes
   are rare and require a reflash.
8. **ESP-Zigbee SDK patches.** None expected. If one becomes
   necessary it goes in `patches/` per the graft workflow — not a
   fork.
