// Unit test for state.c. Just the toggle — the policy that consumes
// the state (warm/red/off) lives in main.c and is exercised on-device,
// not here.

#include <stdio.h>

#include "state.h"

static int failures = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

int main(void) {
    state_machine_t s;

    state_init(&s);
    ASSERT(state_get(&s) == LAMP_STATE_OFF, "init should be OFF");

    state_toggle(&s);
    ASSERT(state_get(&s) == LAMP_STATE_ON, "first toggle should be ON");

    state_toggle(&s);
    ASSERT(state_get(&s) == LAMP_STATE_OFF, "second toggle should be OFF");

    state_toggle(&s);
    ASSERT(state_get(&s) == LAMP_STATE_ON, "third toggle should be ON");

    if (failures == 0) { printf("test_state: PASS\n"); return 0; }
    printf("test_state: %d FAIL\n", failures);
    return 1;
}
