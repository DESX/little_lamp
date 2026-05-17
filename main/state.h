#pragma once

#include <stdint.h>

#include "bulb.h"

typedef enum {
    LAMP_STATE_OFF = 0,
    LAMP_STATE_ON  = 1,
} lamp_state_t;

typedef struct {
    lamp_state_t current;
    uint16_t     press_transition_tenths;
    uint16_t     boundary_transition_tenths;
} state_machine_t;

void          state_init(state_machine_t *m,
                         uint16_t press_transition_tenths,
                         uint16_t boundary_transition_tenths);
void          state_handle_button_press(state_machine_t *m, bulb_t *bulb);
void          state_handle_boundary_cross(state_machine_t *m, bulb_t *bulb);
lamp_state_t  state_get(const state_machine_t *m);
