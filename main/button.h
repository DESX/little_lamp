#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t short_addr;
    uint8_t  endpoint;
    bool     known;
} button_t;

void     button_init(button_t *b);
void     button_set_address(button_t *b, uint16_t short_addr, uint8_t endpoint);
bool     button_is_known(const button_t *b);
uint16_t button_short_addr(const button_t *b);
uint8_t  button_endpoint(const button_t *b);
