#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "odroid_input.h"

/* On-screen keyboard for the MS-DOS core.
 *
 * Design: external/8086tiny/docs/input/03-onscreen-keyboard.md.
 *
 * The device has eight usable inputs, so only a handful of distinct keys can be
 * bound directly (dos_input.c). The OSK is what makes the other ~90 reachable.
 * It draws into the two 20-row letterbox bars that dos_video.c deliberately
 * never writes (rows 0-19 and 220-239); the guest image occupies rows 20-219 and
 * neither side crosses.
 *
 * Ownership note: this file renders into the framebuffer but lives outside
 * dos_video.c on purpose -- video owns rows 20-219, input owns the bars.
 *
 * Call order per frame, from main_dos.c:
 *   dos_input_update(&js)   -- routes to dos_osk_input() when visible
 *   ... emulate ...
 *   dos_video_blit(); dos_osk_draw(fb); lcd_swap();
 */

/* Clear all state. Call once before the frame loop. */
void dos_osk_reset(void);

/* True while the OSK owns the buttons -- in EITHER non-off mode. dos_input.c
 * consults this and must not care which of the two it is. */
bool dos_osk_visible(void);

/* GAME. Advance one step round
 *
 *     off --> keyboard --> mouse --> off
 *
 * Reaching OFF schedules a blank of both bars in both framebuffers. The mode
 * enum and the single transition point (osk_set_mode) are private to dos_osk.c
 * on purpose: nothing outside needs to name a mode, and the day something does,
 * an accessor is cheaper than a shared enum that two files can disagree about.
 *
 * The current mode is always on screen -- blank bars, "KBD L<n>", or
 * "MOUSE <x>,<y>" -- so there is no state the user is in without being told. */
void dos_osk_cycle(void);

/* Consume one frame of gamepad state. Only called while visible. Edge
 * detection is internal, so this must be called exactly once per frame. */
void dos_osk_input(const odroid_gamepad_state_t *js);

/* Paint the bars, if and only if something changed. Call once per painted
 * frame, after the guest blit and before lcd_swap(). A state change arms two
 * repaints so that both framebuffers get the same content -- drawing to only
 * one makes the bars flicker at the swap rate. */
void dos_osk_draw(uint8_t *fb);
