#pragma once

#include "bulb.h"
#include "button.h"
#include "commissioning.h"
#include "log.h"

// console_init builds the REPL and registers commands. The command
// handlers need to mutate the system's state from outside the normal
// event flow, so we pass them every object they touch.
//
// esp_console (the SDK) holds the REPL's state file-static because it
// doesn't expose a handle. That's the documented exception inside
// console.c — our objects are explicit; the SDK's are not.
void console_init(log_t           *log,
                  commissioning_t *commissioning,
                  button_t        *button);
