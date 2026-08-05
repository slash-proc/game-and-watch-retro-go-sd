/* Mouse synthesis for the MS-DOS core. See dos_mouse_ui.h for the design and
 * for the acceleration constants.
 */

#include "dos_mouse_ui.h"
#include "dos_video.h"          /* DOS_LCD_*, DOS_LETTERBOX, DOS_ACTIVE_ROWS */
#include <stdio.h>
#include <string.h>

/* The guest-facing driver, external/8086tiny/dos_mouse.c. Declared here rather
 * than by including dos_mouse.h so that this file does not pull the submodule's
 * header search path into the porting layer -- dos_osk.c takes the same view of
 * dos_key_event(). Keep in step with dos_mouse.h. */
void dos_mouse_motion(int dmx, int dmy);
void dos_mouse_buttons(unsigned buttons);
int  dos_mouse_cursor_wanted(int *vx, int *vy, int *hot_x, int *hot_y);
void dos_mouse_virtual_extent(unsigned *vw, unsigned *vh);
void dos_mouse_stats(unsigned *calls, unsigned *unimpl, unsigned *unimpl_fn,
                     unsigned *cb_delivered, unsigned *cb_dropped);

#define MB_LEFT   1u
#define MB_RIGHT  2u

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

static bool     ui_active;
static bool     ui_render = true;       /* see dos_mouse_ui_set_render() */
static unsigned ui_held[4];             /* frames each direction has been held */
static unsigned ui_buttons;

enum { DIR_UP = 0, DIR_DOWN, DIR_LEFT, DIR_RIGHT };

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void dos_mouse_ui_reset(void)
{
    ui_active  = false;
    ui_buttons = 0;
    memset(ui_held, 0, sizeof ui_held);
    /* ui_render is NOT reset: it is a user preference, not machine state, and
     * a guest that resets the driver on every video mode change must not keep
     * switching the pointer back on under the user. */
}

bool dos_mouse_ui_active(void) { return ui_active; }

void dos_mouse_ui_set_active(bool on)
{
    if (on == ui_active)
        return;
    ui_active = on;
    memset(ui_held, 0, sizeof ui_held);
    if (!on && ui_buttons) {
        /* Release anything still held. A button "let go" by a mode switch
         * rather than by the user stays down forever otherwise -- the mouse
         * version of the trap dos_input_release_all() exists for. */
        ui_buttons = 0;
        dos_mouse_buttons(0);
    }
}

void dos_mouse_ui_set_render(bool on) { ui_render = on; }
bool dos_mouse_ui_render(void)        { return ui_render; }

/* ---------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------- */

/* Mickeys this frame for a direction held for `frames` frames. See the constant
 * block in the header for the shape and for why it is a ramp at all. */
static int step_for(unsigned frames)
{
    unsigned s;

    if (frames < DOS_MOUSE_UI_FINE_FRAMES)
        return DOS_MOUSE_UI_FINE_MICKEYS;

    s = DOS_MOUSE_UI_FINE_MICKEYS
        + (frames - DOS_MOUSE_UI_FINE_FRAMES) / DOS_MOUSE_UI_RAMP_DIV;
    if (s > DOS_MOUSE_UI_MAX_MICKEYS)
        s = DOS_MOUSE_UI_MAX_MICKEYS;
    return (int)s;
}

void dos_mouse_ui_input(const odroid_gamepad_state_t *js)
{
    static const uint8_t dir_btn[4] = {
        ODROID_INPUT_UP, ODROID_INPUT_DOWN, ODROID_INPUT_LEFT, ODROID_INPUT_RIGHT
    };
    int dx = 0, dy = 0;
    unsigned btn = 0;
    int i;

    for (i = 0; i < 4; i++) {
        if (js->values[dir_btn[i]])
            ui_held[i]++;
        else
            ui_held[i] = 0;
    }

    /* Opposite directions held together cancel, which is what the sum below
     * does naturally. Do not special-case it: on a real d-pad it is a rocking
     * motion during a direction change and cancelling is the correct answer for
     * the one frame it lasts. */
    if (ui_held[DIR_UP])    dy -= step_for(ui_held[DIR_UP] - 1);
    if (ui_held[DIR_DOWN])  dy += step_for(ui_held[DIR_DOWN] - 1);
    if (ui_held[DIR_LEFT])  dx -= step_for(ui_held[DIR_LEFT] - 1);
    if (ui_held[DIR_RIGHT]) dx += step_for(ui_held[DIR_RIGHT] - 1);

    if (dx || dy)
        dos_mouse_motion(dx, dy);

    /* B = left click, A = right click. ODROID_INPUT_A/B are the physically
     * labelled A and B on BOTH targets -- Core/Inc/gw_buttons.h gives B_A and
     * B_B fixed bit positions and only the GPIO behind them moves between
     * GNW_TARGET=mario and =zelda -- so this needs no per-target arm. (The
     * names that DO differ per target are START/SELECT, which is what
     * dos_input.c's naming warning is about; neither is used here.) */
    if (js->values[ODROID_INPUT_B]) btn |= MB_LEFT;
    if (js->values[ODROID_INPUT_A]) btn |= MB_RIGHT;

    /* Pushed every frame, not on edges: dos_mouse_buttons() does its own edge
     * detection and needs to see the live state to keep press/release counts
     * (INT 33h 0005h/0006h) in step with it. */
    if (btn != ui_buttons) {
        ui_buttons = btn;
        dos_mouse_buttons(btn);
    }
}

/* ---------------------------------------------------------------------------
 * Rendering
 * ------------------------------------------------------------------------- */

/* An 8x8 arrow. 1 = body, and the outline is drawn by testing the neighbours,
 * so the pointer stays visible over both black and white guest content without
 * a second bitmap.
 *
 * THIS IS NOT THE GUEST'S CURSOR SHAPE. INT 33h function 0009h hands the driver
 * a bitmap and dos_mouse.c ignores it (see "WHAT IS REFUSED" there): we honour
 * the HOTSPOT, which is what software can detect, and draw our own glyph, which
 * is cosmetic. A title with a custom pointer therefore gets an arrow where it
 * expected a crosshair -- stated rather than hidden. */
static const uint8_t arrow[8] = {
    0x80,   /* X....... */
    0xC0,   /* XX...... */
    0xE0,   /* XXX..... */
    0xF0,   /* XXXX.... */
    0xF8,   /* XXXXX... */
    0xE0,   /* XXX..... */
    0x90,   /* X..X.... */
    0x10,   /* ...X.... */
};

#define C_BLACK   0
#define C_WHITE   15

void dos_mouse_ui_draw(uint8_t *fb)
{
    int vx, vy, hx, hy;
    unsigned vw, vh;
    int px, py, r, c;

    /* NOTHING TO UNDRAW, AND THAT IS A DEPENDENCY, NOT LUCK. dos_video.c
     * repaints every one of rows 20..219 on every painted frame -- blit_text()
     * walks all 25x80 cells unconditionally, and the graphics blits are a
     * memcpy per row -- and a frame is only swapped when it was blitted. So the
     * pointer of frame N-1 is gone before this runs, and switching the render
     * toggle off makes it vanish on the next painted frame with no save-under
     * and no smear.
     *
     * IF ANYONE ADDS DIRTY-RECTANGLE TRACKING TO dos_video.c, THIS BREAKS AND
     * LEAVES A TRAIL OF ARROWS. The fix would be a 8x8 save-under per
     * framebuffer, not a repaint here. */
    if (!ui_render)
        return;

    /* The guest's own opinion, ANDed with the user's. Kept separate all the way
     * down: a game that hid the driver cursor so it could draw its own must win
     * over a render toggle that is on, or the user sees two pointers. */
    if (!dos_mouse_cursor_wanted(&vx, &vy, &hx, &hy))
        return;

    dos_mouse_virtual_extent(&vw, &vh);
    if (!vw || !vh)
        return;

    /* Virtual -> panel. Scaled by the MODE's extent, never by the INT 33h
     * range: a guest that narrowed the range with 0007h/0008h restricted where
     * the pointer may go, it did not rescale the screen. Getting that backwards
     * makes the pointer drift away from whatever the guest is drawing under it
     * the moment a program sets a window. */
    px = (vx - hx) * DOS_LCD_WIDTH  / (int)vw;
    py = (vy - hy) * DOS_ACTIVE_ROWS / (int)vh + DOS_LETTERBOX;

    for (r = 0; r < 8; r++) {
        int y = py + r;
        if (y < DOS_LETTERBOX || y >= DOS_LETTERBOX + DOS_ACTIVE_ROWS)
            continue;           /* never write the OSK's bars */
        for (c = 0; c < 8; c++) {
            int x = px + c;
            uint8_t bit = 0x80u >> c;
            if (x < 0 || x >= DOS_LCD_WIDTH)
                continue;
            if (arrow[r] & bit) {
                fb[y * DOS_LCD_WIDTH + x] = C_WHITE;
            } else {
                /* Outline: black wherever the body is adjacent. One pass, no
                 * second bitmap to keep in step with the first. */
                uint8_t up   = (r > 0) ? arrow[r - 1] : 0;
                uint8_t down = (r < 7) ? arrow[r + 1] : 0;
                if ((arrow[r] & (bit << 1)) || (arrow[r] & (bit >> 1)) ||
                    (up & bit) || (down & bit))
                    fb[y * DOS_LCD_WIDTH + x] = C_BLACK;
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Status
 * ------------------------------------------------------------------------- */

void dos_mouse_ui_status(char *out, unsigned n)
{
    int vx, vy, hx, hy;
    unsigned unimpl = 0, unimpl_fn = 0, cbd = 0, cbdrop = 0;

    dos_mouse_cursor_wanted(&vx, &vy, &hx, &hy);
    dos_mouse_stats(NULL, &unimpl, &unimpl_fn, &cbd, &cbdrop);

    /* The unimplemented-function report is on screen and not behind a debug
     * flag on purpose. INT 33h has no error channel, so an unimplemented
     * function returns with every register unchanged (dos_mouse.c) -- which is
     * indistinguishable from success from inside the guest. If a title "ignores
     * the mouse", this line names the function it actually wanted, and a
     * diagnostic that has to be specially built is one nobody runs. */
    if (unimpl)
        snprintf(out, n, "MOUSE %3d,%3d CUR%s CB%u UNIMPL %02X x%u",
                 vx, vy, ui_render ? "+" : "-", cbd, unimpl_fn, unimpl);
    else
        snprintf(out, n, "MOUSE %3d,%3d CUR%s CB%u/%u",
                 vx, vy, ui_render ? "+" : "-", cbd, cbdrop);
}
