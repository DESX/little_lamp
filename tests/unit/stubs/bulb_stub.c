#include <stdbool.h>
#include <stdint.h>

#include "bulb.h"
#include "bulb_stub.h"

bulb_stub_call_t bulb_stub_last_call  = BULB_STUB_NONE;
int              bulb_stub_call_count = 0;

void bulb_init(bulb_t *b) {
    (void)b;
    bulb_stub_last_call  = BULB_STUB_NONE;
    bulb_stub_call_count = 0;
}
void bulb_set_address(bulb_t *b, uint16_t s, uint8_t e) { (void)b; (void)s; (void)e; }
bool bulb_is_known(const bulb_t *b) { (void)b; return true; }

void bulb_command_on_warm(bulb_t *b, uint16_t t) {
    (void)b; (void)t;
    bulb_stub_last_call = BULB_STUB_ON_WARM;
    bulb_stub_call_count++;
}
void bulb_command_off(bulb_t *b, uint16_t t) {
    (void)b; (void)t;
    bulb_stub_last_call = BULB_STUB_OFF;
    bulb_stub_call_count++;
}
void bulb_command_dim_red(bulb_t *b, uint16_t t) {
    (void)b; (void)t;
    bulb_stub_last_call = BULB_STUB_DIM_RED;
    bulb_stub_call_count++;
}
