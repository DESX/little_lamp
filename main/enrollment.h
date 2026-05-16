// IAS Zone CIE enrollment + Third Reality cancelDoubleClick + diagnostic
// readbacks. Logic specific to onboarding a sleepy IAS-Zone device.
//
// See devices/thirdreality-3rsb22bz.md for protocol details and quirks.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "button.h"

// Kick off the auto-enrollment sequence for a newly-(re-)joined button.
// Schedules (with delays) on the Zigbee scheduler:
//   t=0     write our IEEE into the device's IAS_CIE_Address attribute
//   t=500   send unsolicited Zone Enroll Response (SUCCESS, zone_id=23)
//   t=1000  write cancelDoubleClick=1 (no-op on firmware that doesn't have it)
void start_ias_enrollment(uint16_t short_addr);

// Reply to a runtime Zone Enroll Request from the button. Called from the
// APS callback when a cluster-0x0500 cmd 0x01 frame arrives.
void enrollment_handle_enroll_request(uint16_t src_short_addr,
                                      uint8_t src_endpoint);

// Console-driven diagnostics. Trigger a Discover Attributes for the Third
// Reality private cluster (0xFF01), and a Read Attributes on the Basic
// cluster (manufacturer / model / firmware version). Responses arrive
// asynchronously via the action handler's READ_ATTR_RESP / DISC_ATTR_RESP
// callbacks.
void discover_button_attrs(const button_t *btn);
void read_button_basic(const button_t *btn);
