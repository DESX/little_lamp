#pragma once

#include <stdint.h>

// Every behavior knob in the system. Edit a value here and reflash —
// no other source file needs to know about your change. Cluster IDs,
// frame layouts, and other protocol mechanics stay in the helper that
// uses them; this file is only for values an observer of the running
// system would notice.

// ── Schedule ────────────────────────────────────────────────────────────────

// POSIX TZ string. Format: STDoffsetDST[offset][,startrule,endrule]
//   M3.2.0  = month 3 (March), week 2, day 0 (Sunday)  ← 2nd Sunday of March
//   M11.1.0 = month 11 (Nov),  week 1, day 0 (Sunday)  ← 1st Sunday of November
// Default transition time is 02:00 local. DST handled automatically by
// newlib's localtime_r.
#define LAMP_TIMEZONE             "EST5EDT,M3.2.0,M11.1.0"

// Night mode = OFF state lights up as dim red instead of being fully off.
// Times are local-time minutes-of-day.
#define LAMP_NIGHT_START_MIN      (19*60 + 30)   // 19:30
#define LAMP_NIGHT_END_MIN        ( 6*60 + 40)   // 06:40

// ── Bulb (the innr A19) ────────────────────────────────────────────────────

#define LAMP_BULB_LEVEL_ON        254                          // 100% brightness in the ON state
#define LAMP_BULB_LEVEL_NIGHT     5                            //  ~2% brightness for night-light dim red
#define LAMP_BULB_WARM_MIREDS     370                          // ~2700 K warm white in the ON state
#define LAMP_BULB_RED_X           ((uint16_t)(0.675 * 65535))  // CIE 1931 chromaticity x for dim red
#define LAMP_BULB_RED_Y           ((uint16_t)(0.322 * 65535))  // CIE 1931 chromaticity y for dim red

// ── State transitions ──────────────────────────────────────────────────────

// Tenths of a second. 0 = instant snap. Increase for a fade.
#define LAMP_PRESS_TRANSITION_TENTHS     0
#define LAMP_BOUNDARY_TRANSITION_TENTHS  0

// ── Pairing ────────────────────────────────────────────────────────────────

// How long the Zigbee join window stays open after a fresh boot or a
// `pair` console command. Bump if you need more time to walk to the
// button after pressing its pairing gesture.
#define LAMP_JOIN_WINDOW_SECS    180
