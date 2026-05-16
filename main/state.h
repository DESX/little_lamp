// The two-state state machine. ON ↔ OFF_effective, driven by:
//   - button press         → toggle
//   - schedule boundary    → re-render bulb iff currently OFF_effective

#pragma once

typedef enum {
    LAMP_STATE_OFF = 0,   // boot state; "off" means dim-red at night, dark by day
    LAMP_STATE_ON  = 1,
} lamp_state_t;

void state_init(void);
void state_handle_button_press(void);
void state_handle_boundary_cross(void);
lamp_state_t state_get(void);
