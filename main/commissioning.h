#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bulb.h"
#include "button.h"

typedef struct {
    bool      known;
    uint64_t  ieee;
    uint8_t   endpoint;
    uint16_t  short_addr;
} role_binding_t;

// Callback fired whenever the paired-device set changes (a new device
// pairs, or a known one re-announces with a fresh short address).
// main implements this so it can re-render the status banner without
// commissioning.c needing to know about the banner.
typedef void (*commissioning_event_cb_t)(void);

typedef struct {
    role_binding_t button_role;
    role_binding_t bulb_role;
    button_t      *button;
    bulb_t        *bulb;
    commissioning_event_cb_t  on_change;
} commissioning_t;

void commissioning_init(commissioning_t *c,
                        button_t *btn,
                        bulb_t   *blb,
                        commissioning_event_cb_t on_change);
void commissioning_on_device_announce(commissioning_t *c,
                                      uint16_t short_addr,
                                      const uint8_t ieee[8]);
bool commissioning_complete(const commissioning_t *c);
void commissioning_reset(commissioning_t *c);
