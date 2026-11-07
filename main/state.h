#pragma once

// The system's user-facing state: did the user ask for the lamp to be
// on or off? Just one bit. No knowledge of bulbs, schedules, or what
// "on" looks like — those are policy decisions main owns.

typedef enum {
    LAMP_STATE_OFF = 0,
    LAMP_STATE_ON  = 1,
} lamp_state_t;

typedef struct {
    lamp_state_t current;
} state_machine_t;

void          state_init(state_machine_t *m);
void          state_toggle(state_machine_t *m);
lamp_state_t  state_get(const state_machine_t *m);
