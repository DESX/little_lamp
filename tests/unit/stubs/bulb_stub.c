// Host-side stub of bulb.c. Records what state.c's render() invokes so
// tests can assert. Exports two observable globals: `bulb_stub_last_call`
// and `bulb_stub_call_count`. Tests can reset them via `bulb_init()`.

#include <stdbool.h>
#include <stdint.h>

#include "bulb.h"
#include "bulb_stub.h"

bulb_stub_call_t bulb_stub_last_call  = BULB_STUB_NONE;
int              bulb_stub_call_count = 0;

void bulb_init(void) {
    bulb_stub_last_call  = BULB_STUB_NONE;
    bulb_stub_call_count = 0;
}
void bulb_set_address(uint16_t s, uint8_t e) { (void)s; (void)e; }
bool bulb_is_known(void) { return true; }
void bulb_command_on_warm(uint16_t t) { (void)t; bulb_stub_last_call = BULB_STUB_ON_WARM; bulb_stub_call_count++; }
void bulb_command_off(uint16_t t)     { (void)t; bulb_stub_last_call = BULB_STUB_OFF;     bulb_stub_call_count++; }
void bulb_command_dim_red(uint16_t t) { (void)t; bulb_stub_last_call = BULB_STUB_DIM_RED; bulb_stub_call_count++; }
