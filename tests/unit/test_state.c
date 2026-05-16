// Unit tests for state.c. Each test allocates its own state_machine_t
// and bulb_t on the stack — the same pattern main.c uses, just with
// shorter lifetimes. The bulb stub records what render() invoked.

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "state.h"
#include "schedule.h"
#include "rtc.h"
#include "bulb.h"
#include "bulb_stub.h"

static int failures = 0;
#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

static time_t local_epoch(int year, int mo, int d, int h, int mi) {
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    struct tm t = { .tm_year = year-1900, .tm_mon = mo-1, .tm_mday = d,
                    .tm_hour = h, .tm_min = mi, .tm_isdst = -1 };
    return mktime(&t);
}

static void fresh(state_machine_t *s, bulb_t *b) {
    state_init(s);
    bulb_init(b);
}

static void test_first_press_during_day(void) {
    state_machine_t s; bulb_t b; fresh(&s, &b);
    rtc_set(local_epoch(2026, 1, 15, 12, 0));
    ASSERT(state_get(&s) == LAMP_STATE_OFF, "init state must be OFF");
    state_handle_button_press(&s, &b);
    ASSERT(state_get(&s) == LAMP_STATE_ON, "first press should go to ON");
    ASSERT(bulb_stub_last_call == BULB_STUB_ON_WARM, "ON state should emit on-warm");
}

static void test_press_toggles_off_during_day(void) {
    state_machine_t s; bulb_t b; fresh(&s, &b);
    rtc_set(local_epoch(2026, 1, 15, 12, 0));
    state_handle_button_press(&s, &b);   // -> ON
    state_handle_button_press(&s, &b);   // -> OFF (day → fully off)
    ASSERT(state_get(&s) == LAMP_STATE_OFF, "second press should return to OFF");
    ASSERT(bulb_stub_last_call == BULB_STUB_OFF, "day-mode OFF should emit off");
}

static void test_press_toggles_off_during_night(void) {
    state_machine_t s; bulb_t b; fresh(&s, &b);
    rtc_set(local_epoch(2026, 1, 15, 22, 0));
    state_handle_button_press(&s, &b);   // -> ON
    state_handle_button_press(&s, &b);   // -> OFF (night → dim red)
    ASSERT(state_get(&s) == LAMP_STATE_OFF, "press should toggle to OFF");
    ASSERT(bulb_stub_last_call == BULB_STUB_DIM_RED, "night-mode OFF should emit dim-red");
}

static void test_boundary_when_off_in_night(void) {
    state_machine_t s; bulb_t b; fresh(&s, &b);
    rtc_set(local_epoch(2026, 1, 15, 22, 0));
    state_handle_boundary_cross(&s, &b);
    ASSERT(bulb_stub_last_call == BULB_STUB_DIM_RED, "boundary in OFF/night → dim-red");
}

static void test_boundary_when_off_in_day_is_off(void) {
    state_machine_t s; bulb_t b; fresh(&s, &b);
    rtc_set(local_epoch(2026, 1, 15, 12, 0));
    state_handle_boundary_cross(&s, &b);
    ASSERT(bulb_stub_last_call == BULB_STUB_OFF, "boundary in OFF/day → off");
}

static void test_boundary_when_on_does_nothing(void) {
    state_machine_t s; bulb_t b; fresh(&s, &b);
    rtc_set(local_epoch(2026, 1, 15, 22, 0));
    state_handle_button_press(&s, &b);          // -> ON, on-warm
    ASSERT(bulb_stub_last_call == BULB_STUB_ON_WARM, "ON should emit on-warm");
    bulb_init(&b);                              // reset call tracking
    state_handle_boundary_cross(&s, &b);
    ASSERT(bulb_stub_call_count == 0, "boundary while ON must not touch the bulb");
}

int main(void) {
    test_first_press_during_day();
    test_press_toggles_off_during_day();
    test_press_toggles_off_during_night();
    test_boundary_when_off_in_night();
    test_boundary_when_off_in_day_is_off();
    test_boundary_when_on_does_nothing();
    if (failures == 0) {
        printf("test_state: PASS\n");
        return 0;
    }
    printf("test_state: %d FAIL\n", failures);
    return 1;
}
