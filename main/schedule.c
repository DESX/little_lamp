#include "schedule.h"

#include <stdlib.h>
#include <string.h>

#include "log.h"

// SDK exception: the C library's timezone is a process-wide singleton
// (setenv "TZ" + tzset). The night-window bounds get the same treatment
// for symmetry — only one schedule can exist at a time, so a struct
// per instance buys nothing. Configuration is still passed in from main
// at boot, so the values stay visible there.
static char     s_tz[64];
static uint16_t s_night_start_min;
static uint16_t s_night_end_min;

void schedule_init(const char *tz, uint16_t night_start_min, uint16_t night_end_min) {
    strncpy(s_tz, tz, sizeof(s_tz) - 1);
    s_tz[sizeof(s_tz) - 1] = '\0';
    s_night_start_min = night_start_min;
    s_night_end_min   = night_end_min;

    setenv("TZ", s_tz, 1);
    tzset();
    LAMP_LOGI("schedule: TZ=%s, night-window=%02u:%02u..%02u:%02u",
              s_tz,
              s_night_start_min / 60, s_night_start_min % 60,
              s_night_end_min   / 60, s_night_end_min   % 60);
}

static int minutes_of_day(const struct tm *t) {
    return t->tm_hour * 60 + t->tm_min;
}

bool schedule_is_night(time_t now) {
    struct tm lt;
    localtime_r(&now, &lt);
    int m = minutes_of_day(&lt);
    // Window wraps midnight: night iff m >= START OR m < END.
    return m >= s_night_start_min || m < s_night_end_min;
}

time_t schedule_next_boundary(time_t now) {
    struct tm lt;
    localtime_r(&now, &lt);
    int m = minutes_of_day(&lt);

    int next_min;
    int day_offset;
    if (m < s_night_end_min) {
        next_min = s_night_end_min;
        day_offset = 0;
    } else if (m < s_night_start_min) {
        next_min = s_night_start_min;
        day_offset = 0;
    } else {
        next_min = s_night_end_min;
        day_offset = 1;
    }

    struct tm target = lt;
    target.tm_mday += day_offset;
    target.tm_hour = next_min / 60;
    target.tm_min  = next_min % 60;
    target.tm_sec  = 0;
    return mktime(&target);
}
