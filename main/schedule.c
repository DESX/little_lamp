#include "schedule.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"

// US Eastern with DST. Change here + reflash to relocate.
static const char k_tz[] = "EST5EDT,M3.2.0,M11.1.0";

// Night-mode window, in minutes-since-midnight wall-clock time.
#define NIGHT_START_MIN  (19 * 60 + 30)   // 19:30
#define NIGHT_END_MIN    ( 6 * 60 + 40)   // 06:40

void schedule_init(void) {
    setenv("TZ", k_tz, 1);
    tzset();
    LAMP_LOGI("schedule: TZ=%s, night-window=%02d:%02d..%02d:%02d",
              k_tz,
              NIGHT_START_MIN / 60, NIGHT_START_MIN % 60,
              NIGHT_END_MIN / 60, NIGHT_END_MIN % 60);
}

static int minutes_of_day(const struct tm *t) {
    return t->tm_hour * 60 + t->tm_min;
}

bool schedule_is_night(time_t now) {
    struct tm lt;
    localtime_r(&now, &lt);
    int m = minutes_of_day(&lt);
    // Window wraps midnight: night iff m >= START OR m < END.
    return m >= NIGHT_START_MIN || m < NIGHT_END_MIN;
}

time_t schedule_next_boundary(time_t now) {
    struct tm lt;
    localtime_r(&now, &lt);
    int m = minutes_of_day(&lt);

    // Determine the next boundary's minutes-of-day and how many days ahead.
    int next_min;
    int day_offset;
    if (m < NIGHT_END_MIN) {
        next_min = NIGHT_END_MIN;
        day_offset = 0;
    } else if (m < NIGHT_START_MIN) {
        next_min = NIGHT_START_MIN;
        day_offset = 0;
    } else {
        next_min = NIGHT_END_MIN;
        day_offset = 1;
    }

    struct tm target = lt;
    target.tm_mday += day_offset;
    target.tm_hour = next_min / 60;
    target.tm_min  = next_min % 60;
    target.tm_sec  = 0;
    return mktime(&target);
}
