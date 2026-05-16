// Zigbee subsystem. Hides every protocol-level detail — cluster IDs,
// frame parsing, signal-handler dispatch tables, coordinator endpoint
// descriptors — from main.c.
//
// The only thing main.c does with Zigbee is:
//   1) call zigbee_start() at boot
//   2) implement the callbacks declared in this header, which the
//      subsystem fires when something the application cares about
//      happens.

#pragma once

#include <stdint.h>

// Bring up the Zigbee stack. Forms a fresh network on first boot or
// rejoins the persisted one on subsequent boots. Opens a 180-second join
// window if either device is unpaired. Returns once the stack task has
// been created — events arrive asynchronously after that.
void zigbee_start(void);

// Application callback: fires when a button press arrives from any
// paired button device, regardless of which Zigbee protocol path the
// button used (OnOff cluster command, Multistate Input report, IAS Zone
// Status Change Notification — all converge here). main.c implements it.
void on_button_press_event(void);
