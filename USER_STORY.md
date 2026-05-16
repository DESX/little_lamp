# User Story

## Setting

A C19[^1] smart bulb in my daughter's bedroom. A stick-on wall button
beside the door. The button is the only thing she touches; the bulb is
out of reach.

[^1]: Likely "A19" — the form factor of the innr AE 287 C-2.

## Behavior

The button is the **sole** controller of light state. Each press
toggles between two states:

- **ON** — full brightness, warm white. Intended as room lighting.
- **OFF** — meaning depends on time of day:
  - **Day mode** (06:40 – 19:30): bulb is fully off.
  - **Night mode** (19:30 – 06:40): bulb shows a dim red glow,
    acting as a night light.

There is **no way to fully turn the bulb off during night-mode hours**
using the button. "Off" during the night means "dim red." This is
intentional — the room always has at least a night light at night.

**Boundary crossings while the bulb is OFF do change the bulb.** If
the bulb is off (state = OFF) and the clock crosses 19:30, the bulb
fades up to the dim red night light. If it is glowing red and the
clock crosses 06:40, the bulb fades back to fully off. The ON state
is never disturbed by the clock — only the button can leave ON.

## Quality goals

- **Responsive.** A button press should produce a visible change at the
  bulb within ~250 ms. No noticeable delay between click and light.
- **Reliable.** The system must keep working when the PC is off, when
  the internet is out, and through brief wall-power blips.
- **Quiet.** No beeps, no flashes, no app notifications. Press →
  light. That is the entire interface.

## Specific values

| Setting | Value | Notes |
|---|---|---|
| Night mode start | 19:30 | Local wall-clock time |
| Night mode end | 06:40 | Local wall-clock time |
| Timezone | `EST5EDT,M3.2.0,M11.1.0` (US Eastern, DST-aware) | POSIX TZ string compiled into firmware |
| ON brightness | 254/254 (100%) | Zigbee Level Control max |
| ON color temperature | 2700 K (warm white) | ~370 mireds |
| Night-light brightness | 5/254 (~2%) | Visible in a dark room but doesn't disturb sleep |
| Night-light color | xy = (0.675, 0.322) — deep red | Inside the typical consumer-RGB gamut; close to 640 nm dominant wavelength |
| ON ↔ OFF transition (button press) | 2 s fade | Zigbee `transition_time` = 20 |
| Schedule-boundary transition | 5 s fade | Less startling for a sleeping child |
| Button events used | Single-press only | Double-press and long-press are ignored |

## Out of scope (for now)

- Remote control (phone app, voice assistant)
- Multiple bulbs or multiple buttons
- Schedule changes from the daughter's side (no in-room configuration)
- Daylight savings handling — the schedule is in wall-clock time and
  shifts naturally with the system clock
