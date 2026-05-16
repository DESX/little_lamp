// Unit tests for schedule.c. Verifies the night-mode window boundaries
// at 06:40 and 19:30 in the configured timezone (EST5EDT,M3.2.0,M11.1.0).
//
// Build with: cd tests/unit && make
// (Stubs in stubs/log.h satisfy schedule.c's only external dependency.)

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "schedule.h"

static int failures = 0;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

// Compose a local-time epoch for the configured TZ.
static time_t local_epoch(int year, int month, int day, int hour, int minute) {
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    struct tm t = {
        .tm_year = year - 1900,
        .tm_mon  = month - 1,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min  = minute,
        .tm_isdst = -1,
    };
    return mktime(&t);
}

static void test_is_night_boundaries(void) {
    schedule_init();
    // Pick a winter date (no DST ambiguity) for stable boundaries.

    // 06:39 = night (still inside the dark window).
    ASSERT(schedule_is_night(local_epoch(2026, 1, 15, 6, 39)), "06:39 should be night");
    // 06:40 = end of night → day.
    ASSERT(!schedule_is_night(local_epoch(2026, 1, 15, 6, 40)), "06:40 should be day");
    // 12:00 = clearly day.
    ASSERT(!schedule_is_night(local_epoch(2026, 1, 15, 12, 0)), "noon should be day");
    // 19:29 = day.
    ASSERT(!schedule_is_night(local_epoch(2026, 1, 15, 19, 29)), "19:29 should be day");
    // 19:30 = start of night.
    ASSERT(schedule_is_night(local_epoch(2026, 1, 15, 19, 30)), "19:30 should be night");
    // 23:59 = night.
    ASSERT(schedule_is_night(local_epoch(2026, 1, 15, 23, 59)), "23:59 should be night");
    // 00:00 = night (just past midnight).
    ASSERT(schedule_is_night(local_epoch(2026, 1, 15, 0, 0)), "midnight should be night");
}

static void test_next_boundary_during_day(void) {
    schedule_init();
    time_t now = local_epoch(2026, 1, 15, 12, 0);  // noon
    time_t next = schedule_next_boundary(now);
    // Next boundary is today at 19:30.
    time_t expected = local_epoch(2026, 1, 15, 19, 30);
    ASSERT(next == expected, "noon next-boundary should be 19:30 today");
}

static void test_next_boundary_during_evening_night(void) {
    schedule_init();
    time_t now = local_epoch(2026, 1, 15, 20, 0);  // 20:00, in night
    time_t next = schedule_next_boundary(now);
    // Next boundary is tomorrow at 06:40.
    time_t expected = local_epoch(2026, 1, 16, 6, 40);
    ASSERT(next == expected, "20:00 next-boundary should be tomorrow's 06:40");
}

static void test_next_boundary_during_early_morning_night(void) {
    schedule_init();
    time_t now = local_epoch(2026, 1, 15, 3, 0);  // 03:00, still night
    time_t next = schedule_next_boundary(now);
    // Next boundary is today at 06:40.
    time_t expected = local_epoch(2026, 1, 15, 6, 40);
    ASSERT(next == expected, "03:00 next-boundary should be 06:40 today");
}

int main(void) {
    test_is_night_boundaries();
    test_next_boundary_during_day();
    test_next_boundary_during_evening_night();
    test_next_boundary_during_early_morning_night();
    if (failures == 0) {
        printf("test_schedule: PASS\n");
        return 0;
    }
    printf("test_schedule: %d FAIL\n", failures);
    return 1;
}
