#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "odroid_input.h"

/* Mouse synthesis for the MS-DOS core -- the porting half of INT 33h.
 *
 * The guest-facing driver is external/8086tiny/dos_mouse.c. This file is
 * everything that is specific to a machine with no pointing device: turning a
 * d-pad into motion, two buttons into clicks, and drawing a pointer.
 *
 * IT IS A MODE OF THE ON-SCREEN KEYBOARD, NOT A THIRD MODE. The d-pad cannot
 * drive a key grid and a pointer at the same time, so mouse mode is reached from
 * the OSK (dos_osk.c, the MSE key) and is left the same way. dos_input.c is not
 * changed at all: as far as it is concerned the OSK still owns the buttons.
 * That is deliberate -- the requirement was that the non-mouse path stay
 * byte-identical, and the cheapest way to guarantee that is not to touch it.
 *
 * Call order per frame, from main_dos.c:
 *   dos_input_update(&js)      -- reaches dos_osk_input(), which reaches
 *                                 dos_mouse_ui_input() when mouse mode is on
 *   ... emulate ...
 *   dos_video_blit();
 *   dos_mouse_ui_draw(fb);     -- the pointer, over the guest image
 *   dos_osk_draw(fb);          -- the bars
 *   lcd_swap();
 */

/* ---- Acceleration ---------------------------------------------------------
 *
 * A d-pad gives DIRECTION ONLY. Without a ramp the choice is between a pointer
 * too slow to cross a 640-wide virtual screen and one too coarse to hit a menu
 * item, and there is no single speed that is both. So a held direction starts
 * fine and coarsens.
 *
 * Units are MICKEYS PER FRAME. At the driver's default ratio (8 mickeys per 8
 * pixels, dos_mouse.c) one mickey is one virtual pixel, so these numbers read as
 * pixels per frame at 60 Hz until a guest changes the ratio with function 000Fh
 * -- at which point they scale with it, which is what a real mouse does too.
 *
 * Tuned, not derived. The whole ramp is these four numbers and nothing else
 * reads them, so retuning is editing this block:
 *
 *   FINE_MICKEYS   speed while tapping and for the first FINE_FRAMES of a hold.
 *                  1 is deliberate: a single tap must move exactly one pixel or
 *                  fine positioning is impossible.
 *   FINE_FRAMES    how long that lasts. 10 frames is ~166 ms -- long enough that
 *                  a deliberate tap never accelerates, short enough that a hold
 *                  does not feel stuck.
 *   RAMP_DIV       frames per +1 mickey after that. 2 reaches the cap in ~0.9 s.
 *   MAX_MICKEYS    the cap. 24/frame crosses 640 virtual pixels in ~27 frames.
 */
#define DOS_MOUSE_UI_FINE_MICKEYS   1
#define DOS_MOUSE_UI_FINE_FRAMES    10
#define DOS_MOUSE_UI_RAMP_DIV       2
#define DOS_MOUSE_UI_MAX_MICKEYS    24

/* Clear all state. Called from dos_osk_reset(). */
void dos_mouse_ui_reset(void);

/* True while the pointer owns the d-pad. */
bool dos_mouse_ui_active(void);

/* Enter/leave mouse mode. Leaving releases any held button -- a click that is
 * "let go" by a mode switch rather than by the user would otherwise stay down
 * forever, which is the same trap dos_input_release_all() exists for on the
 * keyboard side. */
void dos_mouse_ui_set_active(bool on);

/* The USER's cursor-rendering toggle, separate from the guest's own show/hide
 * (INT 33h 0001h/0002h). The pointer is drawn only when BOTH say yes.
 *
 * Two behaviours are needed and neither is right for every title: a game that
 * hides the driver cursor and draws its own gets a ghost pointer if we also
 * draw, and text-mode software that relies on the driver shows nothing if we do
 * not. So it is a runtime toggle, and it can be flipped while a guest is
 * running -- see dos_mouse_ui_draw() for why that leaves no smear.
 *
 * Default ON. Reasoning: a title that draws its own pointer has almost always
 * called 0002h to hide the driver's, and the AND above then suppresses ours
 * anyway -- so ON is the setting that is correct without user intervention for
 * both of the checked targets. OFF exists for the title that draws its own
 * cursor WITHOUT hiding the driver's, which is a real if rarer bug. */
void dos_mouse_ui_set_render(bool on);
bool dos_mouse_ui_render(void);

/* Consume one frame of gamepad state. Only called while mouse mode is on. */
void dos_mouse_ui_input(const odroid_gamepad_state_t *js);

/* Draw the pointer into the framebuffer, if it should be drawn. Call once per
 * painted frame, AFTER dos_video_blit() and before dos_osk_draw(). */
void dos_mouse_ui_draw(uint8_t *fb);

/* One line of status for the OSK's top bar while mouse mode is on. Writes at
 * most `n` bytes including the NUL. */
void dos_mouse_ui_status(char *out, unsigned n);
