#include "state.h"

#include "log.h"

static const char *state_name(lamp_state_t s) {
    return s == LAMP_STATE_ON ? "ON" : "OFF";
}

void state_init(state_machine_t *m) {
    m->current = LAMP_STATE_OFF;
    LAMP_LOGI("state: init -> %s", state_name(m->current));
}

void state_toggle(state_machine_t *m) {
    lamp_state_t prev = m->current;
    m->current = (m->current == LAMP_STATE_ON) ? LAMP_STATE_OFF : LAMP_STATE_ON;
    LAMP_LOGI("state: %s -> %s", state_name(prev), state_name(m->current));
}

lamp_state_t state_get(const state_machine_t *m) { return m->current; }
