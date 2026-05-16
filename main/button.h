// Receives OnOff cluster commands from the wall button and translates them
// into a single "press" event to the state machine.

#pragma once

#include <stdbool.h>
#include <stdint.h>

void button_init(void);

void button_set_address(uint16_t short_addr, uint8_t endpoint);
bool button_is_known(void);

// Returns true if the given source matched our paired button. Called by
// the global Zigbee action handler when an OnOff cluster command arrives.
bool button_dispatch_on_off(uint16_t src_addr, uint8_t src_endpoint, uint8_t cmd_id);
