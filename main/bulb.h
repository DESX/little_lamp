#pragma once

#include <stdbool.h>
#include <stdint.h>

// User-story-agnostic bulb driver. The caller describes a target visual
// state as a bulb_target_t; bulb_apply gets the bulb there in whatever
// ZCL command sequence is required, including an off/on cycle when
// changing color while lit so the user never sees an intermediate
// "wrong color at wrong level" frame.

typedef enum {
    BULB_TARGET_OFF = 0,    // bulb fully off (other fields ignored)
    BULB_TARGET_TEMP,       // on at color temperature: temp_mireds + level
    BULB_TARGET_XY,         // on at xy chromaticity: color_x + color_y + level
} bulb_target_kind_t;

typedef struct {
    bulb_target_kind_t kind;
    uint8_t            level;        // 0..254 (ignored when kind == OFF)
    uint16_t           temp_mireds;  // valid when kind == TEMP
    uint16_t           color_x;      // CIE 1931 x * 65535, valid when kind == XY
    uint16_t           color_y;      // CIE 1931 y * 65535, valid when kind == XY
} bulb_target_t;

// All state the bulb subsystem owns. Allocated in main, mutated only by
// the bulb_* functions below. The last_* fields let bulb_apply skip
// Zigbee frames that wouldn't change anything — the difference between
// sub-100 ms toggles and 300 ms multi-frame command bursts.
typedef struct {
    uint16_t            short_addr;
    uint8_t             endpoint;
    bool                known;
    bulb_target_kind_t  last_kind;
    bool                last_on;
    uint8_t             last_level;
    uint16_t            last_temp_mireds;   // valid when last_kind == TEMP
    uint16_t            last_color_x;       // valid when last_kind == XY
    uint16_t            last_color_y;       // valid when last_kind == XY
} bulb_t;

void bulb_init(bulb_t *b);
void bulb_set_address(bulb_t *b, uint16_t short_addr, uint8_t endpoint);
bool bulb_is_known(const bulb_t *b);

// Drive the bulb to the target state. Internally handles whatever ZCL
// command sequence is required, including an off/on cycle when going
// between two lit states with different colors (so the intermediate
// frame, where the color has updated but the level hasn't, never
// appears to the user).
void bulb_apply(bulb_t *b, const bulb_target_t *target);
