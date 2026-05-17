#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BULB_COLOR_UNKNOWN = 0,
    BULB_COLOR_WARM,
    BULB_COLOR_RED,
} bulb_color_mode_t;

// What "on" and "dim red" look like on the wire. Allocated in main /
// configuration.h, copied into bulb_t by bulb_init so the per-instance
// behavior is self-contained.
typedef struct {
    uint8_t  level_on;               // ZCL Level (0..254). 254 = full.
    uint8_t  level_night;            // ZCL Level for the night-light dim red.
    uint16_t color_temp_warm_mireds; // ZCL Color Temperature in mireds.
    uint16_t color_x_red;            // CIE 1931 x (0..65535) for night-light red.
    uint16_t color_y_red;            // CIE 1931 y (0..65535) for night-light red.
} bulb_config_t;

// All state the bulb subsystem owns. Allocated in main, mutated only by
// the bulb_* functions below. The last_* fields let bulb_command_*
// skip Zigbee frames that wouldn't change anything — the difference
// between sub-100 ms toggles and 300 ms multi-frame command bursts.
typedef struct {
    bulb_config_t      config;
    uint16_t           short_addr;
    uint8_t            endpoint;
    bool               known;
    bool               last_on;
    bulb_color_mode_t  last_color;
    uint8_t            last_level;
} bulb_t;

void bulb_init(bulb_t *b, const bulb_config_t *config);
void bulb_set_address(bulb_t *b, uint16_t short_addr, uint8_t endpoint);
bool bulb_is_known(const bulb_t *b);

void bulb_command_on_warm(bulb_t *b, uint16_t transition_tenths);
void bulb_command_off    (bulb_t *b, uint16_t transition_tenths);
void bulb_command_dim_red(bulb_t *b, uint16_t transition_tenths);
