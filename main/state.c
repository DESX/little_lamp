#include "state.h"

#include "log.h"
#include "rtc.h"
#include "schedule.h"

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

void state_init(state_machine_t *m,
                uint16_t press_transition_tenths,
                uint16_t boundary_transition_tenths) {
    m->current                    = LAMP_STATE_OFF;
    m->press_transition_tenths    = press_transition_tenths;
    m->boundary_transition_tenths = boundary_transition_tenths;
    LAMP_LOGI("state: init -> %s (press fade=%u/10s, boundary fade=%u/10s)",
              state_name(m->current),
              m->press_transition_tenths,
              m->boundary_transition_tenths);
}

void state_handle_button_press(state_machine_t *m, bulb_t *bulb) {
    lamp_state_t prev = m->current;
    m->current = (m->current == LAMP_STATE_ON) ? LAMP_STATE_OFF : LAMP_STATE_ON;
    LAMP_LOGI("state: %s -> %s (button)", state_name(prev), state_name(m->current));
    render(m, bulb, m->press_transition_tenths);
}

void state_handle_boundary_cross(state_machine_t *m, bulb_t *bulb) {
    LAMP_LOGI("state: schedule boundary fired (current=%s)", state_name(m->current));
    if (m->current == LAMP_STATE_OFF) {
        render(m, bulb, m->boundary_transition_tenths);
    }
}

lamp_state_t state_get(const state_machine_t *m) { return m->current; }
