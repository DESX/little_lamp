// Host-side stub of rtc.c. Tests set the clock with rtc_set(epoch).

#include <stdbool.h>
#include <time.h>

#include "rtc.h"

static time_t s_now = 0;

void   rtc_init(void)         { /* nothing */ }
time_t rtc_now(void)          { return s_now; }
void   rtc_set(time_t t)      { s_now = t; }
bool   rtc_is_set(void)       { return s_now > 1700000000; }
