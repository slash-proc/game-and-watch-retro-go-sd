#pragma once

#include "odroid_input.h"

/* Game & Watch buttons -> guest keyboard.
 *
 * Call dos_input_reset() once before the frame loop and dos_input_update()
 * exactly once per frame with the freshly-read gamepad state. Edge detection is
 * against the previous call, so calling it twice in a frame loses events.
 *
 * The actual injection is dos_key_event() in 8086tiny.c -- see
 * external/8086tiny/docs/input/01-injection-path.md. */
void dos_input_reset(void);
void dos_input_update(const odroid_gamepad_state_t *js);
