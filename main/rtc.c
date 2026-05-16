#include "rtc.h"

#include <sys/time.h>

#include "log.h"
#include "nvs.h"
#include "nvs_flash.h"

// Below this epoch we assume the clock has never been set. Picked as
// "well after this firmware was written," so any plausible real wall-clock
// time is above the threshold.
#define RTC_MIN_VALID_EPOCH ((time_t)1700000000)

#define NVS_NAMESPACE "lamp"
#define NVS_KEY_RTC_SET "rtc_set"

static bool s_rtc_set = false;

void rtc_init(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY_RTC_SET, &v) == ESP_OK && v == 1) {
            s_rtc_set = true;
        }
        nvs_close(h);
    }
    time_t now = rtc_now();
    if (now < RTC_MIN_VALID_EPOCH) {
        s_rtc_set = false;
    }
    LAMP_LOGI("rtc: now=%lld set=%d", (long long)now, (int)s_rtc_set);
}

time_t rtc_now(void) {
    time_t t;
    time(&t);
    return t;
}

void rtc_set(time_t t) {
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_rtc_set = (t >= RTC_MIN_VALID_EPOCH);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_RTC_SET, s_rtc_set ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    LAMP_LOGI("rtc: set epoch=%lld", (long long)t);
}

bool rtc_is_set(void) {
    return s_rtc_set;
}
