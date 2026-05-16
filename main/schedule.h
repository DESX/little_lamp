// Time-of-day rules. The only module allowed to read the wall-clock.

#pragma once

#include <stdbool.h>
#include <time.h>

// Initialise timezone. Must be called once at boot before any other call.
void schedule_init(void);

// Is the given UTC time currently inside the night-mode window?
bool schedule_is_night(time_t now);

// Returns the next time_t (>= now) at which schedule_is_night flips.
// Used to schedule the boundary timer.
time_t schedule_next_boundary(time_t now);
