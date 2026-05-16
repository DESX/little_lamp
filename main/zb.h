#pragma once

#include "commissioning.h"

// zb owns the connection to ZBOSS / ESP-Zigbee SDK. The SDK keeps its
// own state file-static and registers callbacks by symbol name, so zb
// stashes the application's commissioning_t pointer at start time and
// uses it inside the signal handler. That's the documented SDK
// exception; the storage itself still lives in main.
//
// Application contract:
//   - main calls zigbee_start(&commissioning) once at boot.
//   - zb fires on_button_press_event() (declared below, defined in
//     main) whenever a frame classified as a button press arrives.
//   - everything else (frame parsing, signal dispatch, cluster
//     registration, vendor quirks) is private to zb.c.
void zigbee_start(commissioning_t *commissioning);

// Implemented in main.
void on_button_press_event(void);
