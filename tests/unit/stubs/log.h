// Host-side stubs of the firmware's log.h. Used only by tests/unit/.
// All log macros become no-ops; LAMP_UI becomes a printf so tests can
// optionally observe banner output.

#pragma once

#include <stdio.h>

#define LAMP_LOG_TAG "test"

// Wrap printf in `if(0)` so the compiler still type-checks the arguments
// (preventing "unused variable" warnings in production code) but the call
// is dead-coded out at -O0.
#define LAMP_LOGI(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define LAMP_LOGW(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define LAMP_LOGE(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define LAMP_LOGD(...) do { if (0) printf(__VA_ARGS__); } while (0)
#define LAMP_UI(fmt, ...) printf(fmt "\n", ##__VA_ARGS__)
