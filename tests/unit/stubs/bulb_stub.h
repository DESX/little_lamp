// Test-side observation interface for the bulb stub.
//
// Tests #include this (not bulb.h directly) to read what state.c's
// render() called.

#pragma once

typedef enum {
    BULB_STUB_NONE = 0,
    BULB_STUB_ON_WARM,
    BULB_STUB_OFF,
    BULB_STUB_DIM_RED,
} bulb_stub_call_t;

extern bulb_stub_call_t bulb_stub_last_call;
extern int              bulb_stub_call_count;
