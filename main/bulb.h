#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BULB_COLOR_UNKNOWN = 0,
    BULB_COLOR_WARM,
    BULB_COLOR_RED,
} bulb_color_mode_t;

// All state the bulb subsystem owns. Allocated in main, mutated only by
// the bulb_* functions below. The last_* fields let bulb_command_*
// skip Zigbee frames that wouldn't change anything — the difference
// between sub-100 ms toggles and 300 ms multi-frame command bursts.
typedef struct {
    uint16_t           short_addr;
    uint8_t            endpoint;
    bool               known;
    bool               last_on;
    bulb_color_mode_t  last_color;
    uint8_t            last_level;
} bulb_t;

void bulb_init(bulb_t *b);
void bulb_set_address(bulb_t *b, uint16_t short_addr, uint8_t endpoint);
bool bulb_is_known(const bulb_t *b);

void bulb_command_on_warm(bulb_t *b, uint16_t transition_tenths);
void bulb_command_off    (bulb_t *b, uint16_t transition_tenths);
void bulb_command_dim_red(bulb_t *b, uint16_t transition_tenths);
