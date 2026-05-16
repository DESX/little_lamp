// Owns the join window and the persistence of which IEEE addresses play
// which role (button vs bulb). On boot: opens the join window iff NVS does
// not have both a button and a bulb bound.

#pragma once

#include <stdbool.h>
#include <stdint.h>

void commissioning_init(void);

// Called by the Zigbee signal handler when a new device announces itself.
void commissioning_on_device_announce(uint16_t short_addr, const uint8_t ieee[8]);

// True once we have both a button and a bulb persistently bound.
bool commissioning_complete(void);

// Force a re-pair: clears NVS bindings. Currently invoked via `make erase-nvs`
// which wipes the whole flash; provided here for completeness.
void commissioning_reset(void);
