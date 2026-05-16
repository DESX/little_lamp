// Sole module allowed to send Zigbee commands to the bulb.

#pragma once

#include <stdbool.h>
#include <stdint.h>

void bulb_init(void);

// Called by commissioning.c when the bulb is identified and bound.
void bulb_set_address(uint16_t short_addr, uint8_t endpoint);
bool bulb_is_known(void);

// transition_tenths is Zigbee ZCL "transition time" in 1/10 seconds.
void bulb_command_on_warm(uint16_t transition_tenths);
void bulb_command_off(uint16_t transition_tenths);
void bulb_command_dim_red(uint16_t transition_tenths);
