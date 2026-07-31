/* Game & Watch buttons -> guest keyboard keystrokes.
 *
 * The mapping is a data table on purpose (docs/input/02-button-mapping.md): a
 * remap UI or per-title bindings become a matter of swapping the table's
 * contents, not rewriting control flow. Nothing here knows what any individual
 * key means.
 *
 * Encoding is the BIOS's SDL word format, not its ASCII byte format. Two
 * reasons, both hard requirements rather than preferences:
 *   - ASCII generates an inseparable press+release pair, so a held direction is
 *     inexpressible, and Alley Cat needs held directions.
 *   - Arrow keys are not ASCII at all. In the word format they are SDL keysyms
 *     0x111-0x114, which INT 7h translates through unix_cursor_xlt
 *     (bios.asm:507-519, table at bios.asm:3833) into the extended-key scancodes
 *     0x48/0x50/0x4D/0x4B with ASCII zero -- exactly what a real BIOS delivers.
 * See dos_input_key_word() for the bit layout.
 */

#include "dos_input.h"
#include "dos_osk.h"
#include <stdbool.h>
#include <stdint.h>

/* Injection queue in 8086tiny.c. Enqueues one keyboard event; the emulator
 * drains it at an instruction boundary where raising INT 7h is legal. */
extern void dos_key_event(unsigned short key_word);

/* ---- SDL 1.2 keysyms -------------------------------------------------------
 *
 * These are the values INT 7h's own tables are indexed by, verified against
 * bios.asm rather than against an SDL header:
 *   - 0x111-0x114 are the cursor keys, in the order UP, DOWN, RIGHT, LEFT --
 *     which is the order of unix_cursor_xlt's scancodes 0x48, 0x50, 0x4D, 0x4B
 *     (bios.asm:3833) and matches SDL 1.2's SDLK_UP=273 .. SDLK_LEFT=276.
 *   - Anything below 0x100 falls through to sdl_process_key (bios.asm:521) and
 *     is looked up in a2scan_tbl (bios.asm:3754) as a plain ASCII code, so the
 *     keysym for a printable key *is* its ASCII value. Spot-checked in that
 *     table: 13 -> 0x1C (Enter), 27 -> 0x01 (Esc), 'y' -> 0x15, 'n' -> 0x31.
 *
 * Note the SDL path does not consult a2shift_tbl (only the ASCII path does, at
 * bios.asm:675), so an uppercase keysym would not get a synthesized Shift. Any
 * key needing Shift must set DOS_KMOD_SHIFT explicitly.
 */
#define SDLK_UP        0x111
#define SDLK_DOWN      0x112
#define SDLK_RIGHT     0x113
#define SDLK_LEFT      0x114
#define SDLK_RETURN    13
#define SDLK_ESCAPE    27
#define SDLK_SPACE     32
#define SDLK_y         121
#define SDLK_n         110

/* Word-format flag bits, from the INT 7h handler:
 *   0x0400  "from SDL" -- selects the word format at all (bios.asm:387)
 *   0x0800  Alt        (bios.asm:441)
 *   0x1000  Shift      (bios.asm:459)
 *   0x2000  Ctrl       (bios.asm:453)
 *   0x4000  key up     (bios.asm:396)
 */
#define DOS_KEY_SDL      0x0400u
#define DOS_KMOD_ALT     0x0800u
#define DOS_KMOD_SHIFT   0x1000u
#define DOS_KMOD_CTRL    0x2000u
#define DOS_KEY_UP       0x4000u

typedef struct {
    uint8_t  button;    /* odroid_gamepad_key_t */
    uint16_t keysym;    /* SDL keysym, 0 = unmapped */
    uint16_t mods;      /* DOS_KMOD_* to hold with it */
} dos_key_binding_t;

/* ---- The mapping ----------------------------------------------------------
 *
 * Beware the naming: the odroid_gamepad_key_t names do NOT match the legends on
 * the case. odroid_input.c:50-59 maps B_GAME -> ODROID_INPUT_START and
 * B_TIME -> ODROID_INPUT_SELECT, while the physically-labelled START and SELECT
 * buttons arrive as ODROID_INPUT_X and ODROID_INPUT_Y. The comments below name
 * the button on the case; the enum is what the code keys on.
 *
 * PAUSE/SET and POWER are deliberately absent: common_emu_input_loop() consumes
 * ODROID_INPUT_VOLUME as a macro prefix (Core/Src/porting/common.c:236) and
 * main_dos.c calls it every frame, so binding it would fight the launcher.
 *
 * Enter and Escape are on A and B rather than on the physical START/GAME
 * buttons, deliberately: START and SELECT are wired only on GNW_TARGET=zelda
 * ("not connected on mario", Core/Inc/main.h:228), so the keys that make a DOS
 * prompt usable are kept on inputs that exist on every unit.
 *
 * GAME (ODROID_INPUT_START) is NOT in this table any more: it is the on-screen
 * keyboard toggle. See dos_input_update() for why it, and not the zelda-only
 * START button, got the job. It used to send 'n'; 'n' is still on SELECT for
 * zelda units and is reachable from the OSK on every unit.
 *
 * Unassigned: the physical START button (ODROID_INPUT_X, zelda only).
 */
static const dos_key_binding_t dos_key_map[] = {
    { ODROID_INPUT_UP,     SDLK_UP,     0 },
    { ODROID_INPUT_DOWN,   SDLK_DOWN,   0 },
    { ODROID_INPUT_LEFT,   SDLK_LEFT,   0 },
    { ODROID_INPUT_RIGHT,  SDLK_RIGHT,  0 },
    { ODROID_INPUT_A,      SDLK_RETURN, 0 },   /* A button                   */
    { ODROID_INPUT_B,      SDLK_ESCAPE, 0 },   /* B button                   */
    { ODROID_INPUT_SELECT, SDLK_y,      0 },   /* TIME button                */
    { ODROID_INPUT_Y,      SDLK_n,      0 },   /* SELECT button (zelda only) */
    /* Unassigned: ODROID_INPUT_X, the physical START button (zelda only).
     * SDLK_SPACE and DOS_KMOD_CTRL are the usual DOS-game wants if it is ever
     * given a job. Verified as a working slot: binding it to 'k' (0x6B) is what
     * answered Alley Cat's "(K)itten" skill prompt during testing. */
};

#define DOS_KEY_MAP_LEN ((int)(sizeof dos_key_map / sizeof dos_key_map[0]))

static uint8_t dos_prev_down[DOS_KEY_MAP_LEN];

/* Edge state for the OSK toggle button, tracked separately because it is not a
 * table row -- it produces no keystroke at all. */
static uint8_t dos_toggle_prev;

static unsigned short dos_input_key_word(const dos_key_binding_t *b, bool down)
{
    return (unsigned short)(DOS_KEY_SDL | b->mods | b->keysym |
                            (down ? 0u : DOS_KEY_UP));
}

void dos_input_reset(void)
{
    for (int i = 0; i < DOS_KEY_MAP_LEN; i++)
        dos_prev_down[i] = 0;
    dos_toggle_prev = 0;
    dos_osk_reset();
}

/* Release every game-mode key that is still held, before the OSK takes the
 * buttons. Not tidiness: once a key has arrived in the SDL format the BIOS
 * stops synthesising releases (last_key_sdl latches, bios.asm:393; auto-release
 * skipped at :937), so a key-down whose button is "let go" by a mode switch
 * rather than by the user would stick down forever. */
static void dos_input_release_all(void)
{
    for (int i = 0; i < DOS_KEY_MAP_LEN; i++) {
        if (!dos_prev_down[i])
            continue;
        dos_prev_down[i] = 0;
        if (dos_key_map[i].keysym)
            dos_key_event(dos_input_key_word(&dos_key_map[i], false));
    }
}

void dos_input_update(const odroid_gamepad_state_t *js)
{
    /* ---- Mode toggle ------------------------------------------------------
     *
     * GAME (ODROID_INPUT_START on the case's legend -- see the naming warning
     * above) opens and closes the on-screen keyboard.
     *
     * docs/input-roadmap.md flagged this as a CONFLICT and suggested the
     * zelda-only START button (ODROID_INPUT_X) as "the obvious spare". That is
     * the wrong call and this deliberately diverges from it: START does not
     * exist on a mario unit (Core/Inc/main.h:228), and a mario unit is precisely
     * the one with no spare button -- which docs/input/02-button-mapping.md
     * itself calls "the strongest single argument for the OSK". Putting the only
     * door to the keyboard on a button half the fleet lacks would make the
     * feature unreachable exactly where it matters most.
     *
     * GAME is the cheapest button to spend. It carried 'n', which was always a
     * duplicate of SELECT's 'n' and which the OSK now makes reachable on every
     * unit. The cost on mario is that 'n' is no longer a single button press;
     * the gain is 90-odd keys that were not reachable at all.
     *
     * While the OSK is up it owns the d-pad, A, B and TIME; GAME stays the
     * toggle so there is always a way out. */
    const bool osk_was_up = dos_osk_visible();
    const uint8_t toggle_now = js->values[ODROID_INPUT_START] ? 1 : 0;

    if (toggle_now && !dos_toggle_prev) {
        if (!osk_was_up)
            dos_input_release_all();
        dos_osk_toggle();
    }
    dos_toggle_prev = toggle_now;

    const bool osk_now = dos_osk_visible();

    if (osk_now && osk_was_up)
        dos_osk_input(js);

    if (osk_now || osk_was_up) {
        /* The keyboard consumes the frame -- including the frame it opens on and
         * the frame it closes on. Keep tracking raw button state so that a
         * button still held across the transition does not look like a fresh
         * press on the next frame, but emit nothing from the table. */
        for (int i = 0; i < DOS_KEY_MAP_LEN; i++)
            dos_prev_down[i] = js->values[dos_key_map[i].button] ? 1 : 0;
        return;
    }

    /* One event per edge, never per frame. The BIOS's INT 7h accumulates
     * modifier state with `add`, not `or` (bios.asm:449-462), so re-sending a
     * key-down for a still-held button would corrupt keyflags1. Repeat, if a
     * program wants it, is the guest's business.
     *
     * Several buttons can change state in one frame, and mem[0x4A6] plus
     * pc_interrupt(7) can only carry one event at a time -- so every edge is
     * pushed to a queue that the emulator drains one event per eligible
     * instruction boundary. Table order therefore decides the order simultaneous
     * presses reach the guest; nothing is dropped unless the queue overflows. */
    for (int i = 0; i < DOS_KEY_MAP_LEN; i++) {
        const dos_key_binding_t *b = &dos_key_map[i];
        uint8_t now = js->values[b->button] ? 1 : 0;

        if (now == dos_prev_down[i])
            continue;

        dos_prev_down[i] = now;
        if (b->keysym)
            dos_key_event(dos_input_key_word(b, now != 0));
    }
}
