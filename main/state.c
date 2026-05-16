#include "state.h"

#include "log.h"
#include "rtc.h"
#include "schedule.h"

#define PRESS_TRANSITION_TENTHS    0
#define BOUNDARY_TRANSITION_TENTHS 0

static const char *state_name(lamp_state_t s) {
    return s == LAMP_STATE_ON ? "ON" : "OFF";
}

static void render(const state_machine_t *m, bulb_t *bulb, uint16_t transition_tenths) {
    if (m->current == LAMP_STATE_ON) {
        bulb_command_on_warm(bulb, transition_tenths);
        return;
    }
    if (schedule_is_night(rtc_now())) {
        bulb_command_dim_red(bulb, transition_tenths);
    } else {
        bulb_command_off(bulb, transition_tenths);
    }
}

void state_init(state_machine_t *m) {
    m->current = LAMP_STATE_OFF;
    LAMP_LOGI("state: init -> %s (no command emitted)", state_name(m->current));
}

void state_handle_button_press(state_machine_t *m, bulb_t *bulb) {
    lamp_state_t prev = m->current;
    m->current = (m->current == LAMP_STATE_ON) ? LAMP_STATE_OFF : LAMP_STATE_ON;
    LAMP_LOGI("state: %s -> %s (button)", state_name(prev), state_name(m->current));
    render(m, bulb, PRESS_TRANSITION_TENTHS);
}

void state_handle_boundary_cross(state_machine_t *m, bulb_t *bulb) {
    LAMP_LOGI("state: schedule boundary fired (current=%s)", state_name(m->current));
    if (m->current == LAMP_STATE_OFF) {
        render(m, bulb, BOUNDARY_TRANSITION_TENTHS);
    }
}

lamp_state_t state_get(const state_machine_t *m) { return m->current; }
