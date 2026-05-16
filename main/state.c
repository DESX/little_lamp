#include "state.h"

#include "bulb.h"
#include "log.h"
#include "rtc.h"
#include "schedule.h"

// Transition durations, in 1/10 second (Zigbee ZCL convention). 0 = instant.
#define PRESS_TRANSITION_TENTHS    0   // instant — press feels snappy
#define BOUNDARY_TRANSITION_TENTHS 0   // instant — schedule rollover happens
                                       // while the child is asleep anyway

static lamp_state_t s_state = LAMP_STATE_OFF;

static const char *state_name(lamp_state_t s) {
    return s == LAMP_STATE_ON ? "ON" : "OFF";
}

// Send the bulb commands appropriate for the current state.
static void render(uint16_t transition_tenths) {
    if (s_state == LAMP_STATE_ON) {
        bulb_command_on_warm(transition_tenths);
        return;
    }
    if (schedule_is_night(rtc_now())) {
        bulb_command_dim_red(transition_tenths);
    } else {
        bulb_command_off(transition_tenths);
    }
}

void state_init(void) {
    s_state = LAMP_STATE_OFF;
    LAMP_LOGI("state: init -> %s (no command emitted)", state_name(s_state));
    // Per the user story: do not touch the bulb on boot. The next button
    // press is what wakes us up.
}

void state_handle_button_press(void) {
    lamp_state_t prev = s_state;
    s_state = (s_state == LAMP_STATE_ON) ? LAMP_STATE_OFF : LAMP_STATE_ON;
    LAMP_LOGI("state: %s -> %s (button)", state_name(prev), state_name(s_state));
    render(PRESS_TRANSITION_TENTHS);
}

void state_handle_boundary_cross(void) {
    LAMP_LOGI("state: schedule boundary fired (current=%s)", state_name(s_state));
    if (s_state == LAMP_STATE_OFF) {
        render(BOUNDARY_TRANSITION_TENTHS);
    }
}

lamp_state_t state_get(void) {
    return s_state;
}
