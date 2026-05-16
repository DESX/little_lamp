// Wall-clock access. The ESP32-C6's internal RTC is kept alive by the
// soldered battery; if it ever resets, rtc_is_set() returns false until
// `set-time` is invoked over USB.

#pragma once

#include <stdbool.h>
#include <time.h>

void rtc_init(void);
time_t rtc_now(void);
void rtc_set(time_t t);
bool rtc_is_set(void);
