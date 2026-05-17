// Time-of-day rules. The only module allowed to read the wall-clock.
//
// Configuration (timezone, night-window bounds) flows in via schedule_init.
// The values that govern observable behavior live in main / configuration.h,
// not here.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// Initialise timezone and night-window bounds. Must be called once at boot
// before any other schedule_* call.
//
//   tz              — POSIX TZ string, passed to setenv("TZ", ...)
//   night_start_min — minutes-since-midnight local time when night begins
//   night_end_min   — minutes-since-midnight local time when night ends
//                     (window wraps midnight when end < start)
void schedule_init(const char *tz, uint16_t night_start_min, uint16_t night_end_min);

// Is the given UTC time currently inside the night-mode window?
bool schedule_is_night(time_t now);

// Returns the next time_t (>= now) at which schedule_is_night flips.
// Used to schedule the boundary timer.
time_t schedule_next_boundary(time_t now);
