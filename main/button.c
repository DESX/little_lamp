#include "button.h"

#include "log.h"

void button_init(button_t *b) {
    b->short_addr = 0xFFFE;
    b->endpoint   = 0;
    b->known      = false;
}

void button_set_address(button_t *b, uint16_t short_addr, uint8_t endpoint) {
    b->short_addr = short_addr;
    b->endpoint   = endpoint;
    b->known      = true;
    LAMP_LOGI("button: address set short=0x%04x ep=%u", b->short_addr, b->endpoint);
}

bool     button_is_known(const button_t *b)    { return b->known; }
uint16_t button_short_addr(const button_t *b)  { return b->short_addr; }
uint8_t  button_endpoint(const button_t *b)    { return b->endpoint; }
