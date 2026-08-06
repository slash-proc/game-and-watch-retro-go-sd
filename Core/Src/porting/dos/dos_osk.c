/* On-screen keyboard for the MS-DOS core.
 *
 * Design: external/8086tiny/docs/input/03-onscreen-keyboard.md.
 *
 * Two things here are non-obvious and load-bearing:
 *
 *  1. EVERY STATE CHANGE PAINTS TWICE. The LCD is double buffered and the two
 *     buffers are swapped, not copied, so content written to one of them is
 *     visible on alternate frames only -- it flickers at the swap rate. Painting
 *     is therefore driven by a repaint *counter* seeded to 2, decremented once
 *     per painted frame. Two painted frames means two swaps means both buffers
 *     hold the same bars. (docs/video/06-framebuffer-clut.md)
 *
 *  2. A SHIFTED KEY MUST SET 0x1000 ITSELF. The BIOS's INT 7h has two decode
 *     paths, and only the ASCII one consults a2shift_tbl (bios.asm:694). The SDL
 *     word format we use goes through sdl_process_key (bios.asm:539), which
 *     looks the keysym up in a2scan_tbl and never synthesises a Shift. So
 *     sending keysym '!' alone produces '1'. osk_needs_shift() below mirrors
 *     a2shift_tbl (bios.asm:4016) exactly and is applied at press time.
 *
 * Rendering lives here rather than in dos_video.c because the two own disjoint
 * parts of the panel: video owns rows 20-219, input owns rows 0-19 and 220-239.
 *
 * THREE TOP-LEVEL MODES, ONE BUTTON. GAME cycles
 *
 *     OFF --GAME--> KBD --GAME--> MOUSE --GAME--> OFF
 *
 * and osk_set_mode() is the ONLY place a transition happens. Everything that
 * has to be true on a mode change -- the mouse UI following the mode, held
 * clicks released, the stale button image discarded, both framebuffers
 * repainted -- lives in that one function, so a future per-mode behaviour is a
 * branch there and in draw_status()/dos_osk_input(), not a rewrite. See
 * external/8086tiny/docs/input/03-onscreen-keyboard.md.
 */

#include "dos_osk.h"
#include "dos_mouse_ui.h"
#include "dos_video.h"      /* dos_font_8x8, DOS_LCD_* , DOS_LETTERBOX */
#include <string.h>

/* Injection queue in 8086tiny.c -- same seam dos_input.c uses. */
extern void dos_key_event(unsigned short key_word);

/* Guest memory, for the BDA video-mode probe below. 0x449/0x44A are inside the
 * identity-mapped first 640 KB, so no fold is needed (main_dos.c does the same). */
extern unsigned char mem[];

/* SDL word-format bits (bios.asm:387/396/441/453/459). Duplicated from
 * dos_input.c deliberately: the two files are independent users of the same
 * BIOS ABI and neither should have to include the other's private header. */
#define DOS_KEY_SDL      0x0400u
#define DOS_KMOD_ALT     0x0800u
#define DOS_KMOD_SHIFT   0x1000u
#define DOS_KMOD_CTRL    0x2000u
#define DOS_KEY_UP       0x4000u

/* SDL keysyms the BIOS decodes specially.
 *   0x111-0x114 cursor keys, UP/DOWN/RIGHT/LEFT   (unix_cursor_xlt, bios.asm:4094)
 *   0x116-0x119 Home/End/PgUp/PgDn                (pgup_pgdn_xlt,   bios.asm:4098)
 *   0x11A-0x123 F1..F10                           (bios.asm:497-508)
 * Everything below 0x100 is its own ASCII code (a2scan_tbl, bios.asm:4015). */
#define K_UP    0x111
#define K_DOWN  0x112
#define K_RIGHT 0x113
#define K_LEFT  0x114
#define K_HOME  0x116
#define K_END   0x117
#define K_PGUP  0x118
#define K_PGDN  0x119
#define K_F(n)  (0x11A + (n) - 1)

/* ---------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------- */

enum {
    OSK_ACT_KEY = 0,    /* send keysym */
    OSK_ACT_SHIFT,      /* latch/unlatch sticky Shift */
    OSK_ACT_CTRL,
    OSK_ACT_ALT,
    OSK_ACT_LAYER,      /* cycle layer */
    OSK_ACT_CLOSE,      /* straight to OFF, short-cutting mouse mode */
    /* Mouse mode IS a top-level mode now (GAME cycles into it), so no grid key
     * carries OSK_ACT_MOUSE any more -- see the K_NAV comment below. The action
     * is kept because it costs three lines and is the whole of re-adding an
     * MSE cell if the button route turns out not to be discoverable enough. */
    OSK_ACT_MOUSE,      /* enter mouse mode */
    OSK_ACT_CURSOR,     /* toggle the driver-drawn pointer */
};

/* Cell width is one 8x8 glyph per label character, so a row's total label
 * length must not exceed 320/8 = 40. Checked by _Static_assert-equivalent
 * arithmetic in the comments beside each row. */
typedef struct {
    char     label[4];  /* 1..3 chars, NUL terminated */
    uint16_t keysym;    /* 0 for action keys */
    uint8_t  action;
} osk_key_t;

#define KL(c)          { { (c), 0, 0, 0 }, (uint16_t)(unsigned char)(c), OSK_ACT_KEY }
#define KS(lbl, sym)   { lbl, (sym), OSK_ACT_KEY }
#define KA(lbl, act)   { lbl, 0, (act) }

/* Common tail keys, spelled out per row because C has no array splicing. */
#define K_MODS   KA("SHF", OSK_ACT_SHIFT), KA("CTL", OSK_ACT_CTRL), KA("ALT", OSK_ACT_ALT)
/* MSE IS GONE FROM THE GRID AND ITS THREE CELLS WENT BACK TO BACKTICK.
 * GAME now reaches mouse mode in one press from anywhere, on every unit, with
 * no grid navigation at all -- so an MSE cell would be a strictly slower
 * duplicate of a button that already exists. Backtick, by contrast, is
 * reachable nowhere else on the device, which is exactly what the comment on
 * layer 1 row 1 said when it was dropped to make room. Trading a duplicate for
 * the only route to a key is the good side of that trade.
 *
 * Discoverability moved to the status bar instead of the grid: the top line
 * says "GAME>MSE" on every layer, which is more visible than one cell on one
 * layer ever was. */
#define K_NAV    KA("LYR", OSK_ACT_LAYER), KA("OFF", OSK_ACT_CLOSE)

/* Layer 0 -- letters. 13 + 4*3 = 25 cells / 13 + 3*3 + 3*3 = 31 cells. */
static const osk_key_t osk_l0r0[] = {
    KL('a'), KL('b'), KL('c'), KL('d'), KL('e'), KL('f'), KL('g'),
    KL('h'), KL('i'), KL('j'), KL('k'), KL('l'), KL('m'),
    KS("SPC", ' '), KS("ENT", '\r'), KS("BSP", '\b'), KS("TAB", '\t'),
};
static const osk_key_t osk_l0r1[] = {
    KL('n'), KL('o'), KL('p'), KL('q'), KL('r'), KL('s'), KL('t'),
    KL('u'), KL('v'), KL('w'), KL('x'), KL('y'), KL('z'),
    K_MODS, KS("ESC", 27), K_NAV,
};

/* Layer 1 -- digits and punctuation. 20 + 9 = 29 / 20 + 18 = 38 cells. */
static const osk_key_t osk_l1r0[] = {
    KL('0'), KL('1'), KL('2'), KL('3'), KL('4'),
    KL('5'), KL('6'), KL('7'), KL('8'), KL('9'),
    KL('.'), KL('\\'), KL(':'), KL('/'), KL('-'),
    KL('_'), KL('='), KL('+'), KL('*'), KL('?'),
    KS("SPC", ' '), KS("ENT", '\r'), KS("BSP", '\b'),
};
static const osk_key_t osk_l1r1[] = {
    KL(','), KL(';'), KL('\''), KL('"'), KL('('),
    KL(')'), KL('['), KL(']'), KL('<'), KL('>'),
    KL('!'), KL('@'), KL('#'), KL('$'), KL('%'),
    /* '`' IS BACK. It was dropped when K_NAV gained the three-cell MSE key,
     * because a row is 320/8 = 40 cells and this one would have been at 41.
     * K_NAV lost MSE again (see its comment), so the arithmetic is now
     * 20 single-char keys + K_MODS(9) + ESC(3) + K_NAV(6) = 38. Still under 40,
     * which matters because draw_glyph() CLIPS SILENTLY -- an over-long row
     * loses its OFF key with no warning and no fault.
     *
     * This number is no longer maintained by hand: test286/runoskmode.sh arm C
     * counts all six rows from the source and fails over 40. */
    KL('&'), KL('^'), KL('~'), KL('|'), KL('`'),
    K_MODS, KS("ESC", 27), K_NAV,
};

/* Layer 2 -- function and navigation keys. 21+8+6 = 35 / 21+15 = 36 cells. */
static const osk_key_t osk_l2r0[] = {
    KS("F1", K_F(1)), KS("F2", K_F(2)), KS("F3", K_F(3)), KS("F4", K_F(4)),
    KS("F5", K_F(5)), KS("F6", K_F(6)), KS("F7", K_F(7)), KS("F8", K_F(8)),
    KS("F9", K_F(9)), KS("F10", K_F(10)),
    KS("UP", K_UP), KS("DN", K_DOWN), KS("LT", K_LEFT), KS("RT", K_RIGHT),
    KS("HOM", K_HOME), KS("END", K_END),
};
static const osk_key_t osk_l2r1[] = {
    KS("ESC", 27), KS("TAB", '\t'), KS("ENT", '\r'), KS("BSP", '\b'),
    KS("SPC", ' '), KS("PGU", K_PGUP), KS("PGD", K_PGDN),
    K_MODS, K_NAV,
};

typedef struct {
    const osk_key_t *keys;
    uint8_t          n;
} osk_row_t;

#define ROW(a) { (a), (uint8_t)(sizeof(a) / sizeof((a)[0])) }

#define OSK_LAYERS 3
#define OSK_ROWS   2

static const osk_row_t osk_layout[OSK_LAYERS][OSK_ROWS] = {
    { ROW(osk_l0r0), ROW(osk_l0r1) },
    { ROW(osk_l1r0), ROW(osk_l1r1) },
    { ROW(osk_l2r0), ROW(osk_l2r1) },
};

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */

#define OSK_ECHO_MAX 38

/* The GAME cycle. Order IS the cycle order -- dos_osk_cycle() is (mode + 1) %
 * OSK_MODE_COUNT and nothing else, so inserting a mode here inserts it into the
 * cycle and nowhere else has to agree. */
enum {
    OSK_MODE_OFF = 0,   /* buttons belong to the guest (dos_input.c's table) */
    OSK_MODE_KBD,       /* the key grid owns the d-pad */
    OSK_MODE_MOUSE,     /* the pointer owns the d-pad */
    OSK_MODE_COUNT,
};

static uint8_t  osk_mode;
static uint8_t  osk_layer;
static uint8_t  osk_row;
static uint8_t  osk_col;
static uint16_t osk_sticky;          /* DOS_KMOD_* currently latched */
static uint8_t  osk_paints;          /* framebuffers still to repaint */
static uint32_t osk_prev_buttons;    /* edge detection for navigation */
static char     osk_echo[OSK_ECHO_MAX + 1];
static uint8_t  osk_echo_len;
static uint8_t  osk_seen_mode;       /* BDA 0x449 as of the last paint */
static uint8_t  osk_seen_cols;       /* BDA 0x44A low byte */

/* Panel geometry. The bars are DOS_LETTERBOX (20) rows each; an 8x8 glyph row
 * plus padding fits twice. */
#define OSK_W          DOS_LCD_WIDTH
#define OSK_CELL       8
#define OSK_TOP_Y0     2                                   /* rows  2..9  */
#define OSK_TOP_Y1     11                                  /* rows 11..18 */
#define OSK_BOT_Y0     (DOS_LETTERBOX + DOS_ACTIVE_ROWS + 1)   /* 221..228 */
#define OSK_BOT_Y1     (DOS_LETTERBOX + DOS_ACTIVE_ROWS + 10)  /* 230..237 */

/* CGA CLUT slots, which dos_video.c programs 0-15 with (dos_video.c
 * program_clut()). No extra colours are needed and none are disturbed. */
#define C_BLACK   0
#define C_DIM     8
#define C_WHITE   15

static void osk_mark_dirty(void) { osk_paints = 2; }

/* THE ONLY PLACE osk_mode CHANGES. Four obligations, all of which have a
 * failure behind them, and all of which are easy to forget at a new call site:
 *
 *  - the mouse UI must follow the mode. dos_mouse_ui_set_active(false) releases
 *    a held click; a click "let go" by a mode switch rather than by the user
 *    would otherwise stay down forever (dos_mouse_ui.h), which is the same trap
 *    dos_input_release_all() exists for on the keyboard side.
 *  - entering a mode must NOT inherit the button image from the previous one.
 *    The GAME press that causes the transition is still physically held on the
 *    frame the new mode first runs, and without this it reads as a fresh press
 *    inside the new mode. ~0u means "everything was already down", so the first
 *    frame in a mode sees no edges at all.
 *  - a latched modifier must not survive a mode change. It is invisible from
 *    outside the key grid, so it would fire on whatever key was pressed next.
 *  - both framebuffers must be repainted (the file header, note 1).
 *
 * Adding per-mode entry/exit behaviour later means adding it here. */
static void osk_set_mode(uint8_t m)
{
    if (m == osk_mode)
        return;

    osk_mode = m;
    dos_mouse_ui_set_active(m == OSK_MODE_MOUSE);

    if (m != OSK_MODE_OFF) {
        osk_prev_buttons = ~0u;
        osk_sticky       = 0;
    }
    osk_mark_dirty();
}

/* ---------------------------------------------------------------------------
 * Painting
 * ------------------------------------------------------------------------- */

static void draw_glyph(uint8_t *fb, int x, int y, uint8_t ch, uint8_t fg, uint8_t bg)
{
    const uint8_t *g = dos_font_8x8[ch];

    if (x < 0 || x + OSK_CELL > OSK_W)
        return;

    for (int r = 0; r < 8; r++) {
        uint8_t  bits = g[r];
        uint8_t *p    = fb + (y + r) * OSK_W + x;
        for (int c = 0; c < 8; c++)
            p[c] = (bits & (0x80u >> c)) ? fg : bg;
    }
}

static int draw_text(uint8_t *fb, int x, int y, const char *s, uint8_t fg, uint8_t bg)
{
    for (; *s; s++, x += OSK_CELL)
        draw_glyph(fb, x, y, (uint8_t)*s, fg, bg);
    return x;
}

static void blank_bars(uint8_t *fb)
{
    memset(fb, C_BLACK, DOS_LETTERBOX * OSK_W);
    memset(fb + (DOS_LETTERBOX + DOS_ACTIVE_ROWS) * OSK_W, C_BLACK,
           DOS_LETTERBOX * OSK_W);
}

/* The label as displayed: a latched Shift uppercases the letter keys, which is
 * the only place the latch is visible where the user is actually looking. */
static char display_char(char c)
{
    if ((osk_sticky & DOS_KMOD_SHIFT) && c >= 'a' && c <= 'z')
        return (char)(c - 'a' + 'A');
    return c;
}

static void draw_row(uint8_t *fb, int y, uint8_t r)
{
    const osk_row_t *row = &osk_layout[osk_layer][r];
    int x = 0;

    for (uint8_t i = 0; i < row->n; i++) {
        const osk_key_t *k   = &row->keys[i];
        bool             sel = (r == osk_row && i == osk_col);
        /* Highlight is a foreground/background swap, not a new colour --
         * nothing extra to program into the CLUT. */
        uint8_t          fg  = sel ? C_BLACK : C_WHITE;
        uint8_t          bg  = sel ? C_WHITE : C_BLACK;

        for (const char *s = k->label; *s; s++, x += OSK_CELL)
            draw_glyph(fb, x, y, (uint8_t)display_char(*s), fg, bg);
    }

    /* Tail of the row that no key covers. */
    if (x < OSK_W)
        for (int ry = 0; ry < 8; ry++)
            memset(fb + (y + ry) * OSK_W + x, C_BLACK, (size_t)(OSK_W - x));
}

static void draw_status(uint8_t *fb)
{
    char lay[4] = { 'L', (char)('1' + osk_layer), 0, 0 };
    int  x;

    memset(fb, C_BLACK, DOS_LETTERBOX * OSK_W);

    /* Mouse mode replaces the whole top bar rather than sharing it: the
     * position readout changes every frame and interleaving it with the sticky
     * modifier indicators made both unreadable. */
    if (osk_mode == OSK_MODE_MOUSE) {
        char st[41];
        dos_mouse_ui_status(st, sizeof st);
        draw_text(fb, 0, OSK_TOP_Y0, st, C_WHITE, C_BLACK);
        /* 38 cells of 40. The old line spent nine of them on "TIME back" and
         * had no room to name GAME at all; GAME is the one that must be here,
         * because it is the only control that exists on every unit and the only
         * way back to the game. */
        draw_text(fb, 0, OSK_TOP_Y1,
                  "dpad move  B/A click  TIME kbd  GAME off", C_DIM, C_BLACK);
        return;
    }

    x = draw_text(fb, 0, OSK_TOP_Y0, "KBD ", C_WHITE, C_BLACK);
    x = draw_text(fb, x, OSK_TOP_Y0, lay, C_WHITE, C_BLACK);
    x = draw_text(fb, x, OSK_TOP_Y0, "  ", C_WHITE, C_BLACK);
    x = draw_text(fb, x, OSK_TOP_Y0, "SHF ",
                  (osk_sticky & DOS_KMOD_SHIFT) ? C_WHITE : C_DIM, C_BLACK);
    x = draw_text(fb, x, OSK_TOP_Y0, "CTL ",
                  (osk_sticky & DOS_KMOD_CTRL) ? C_WHITE : C_DIM, C_BLACK);
    x = draw_text(fb, x, OSK_TOP_Y0, "ALT",
                  (osk_sticky & DOS_KMOD_ALT) ? C_WHITE : C_DIM, C_BLACK);
    /* Where GAME goes from here. This replaces the MSE grid cell as the way
     * mouse mode is discovered, and it is on every layer rather than one.
     * 19 cells used above + 13 = 32 of 40. */
    draw_text(fb, x, OSK_TOP_Y0, "   GAME>MSE", C_DIM, C_BLACK);

    /* Echo of what has been sent. Genuinely useful: the guest may be mid-redraw,
     * or not echoing at all, and then this is the only feedback there is. */
    draw_text(fb, 0, OSK_TOP_Y1, osk_echo, C_DIM, C_BLACK);
}

void dos_osk_draw(uint8_t *fb)
{
    /* dos_video.c clears both framebuffers -- bars included -- on a guest video
     * mode change (clear_framebuffers(), called from the mode dispatch). We
     * cannot see that call, but we can see the same BDA bytes it keys on, so
     * re-arm from those rather than reaching into video's state. */
    uint8_t mode = mem[0x449];
    uint8_t cols = mem[0x44A];
    if (mode != osk_seen_mode || cols != osk_seen_cols) {
        osk_seen_mode = mode;
        osk_seen_cols = cols;
        osk_mark_dirty();
    }

    if (!osk_paints)
        return;
    osk_paints--;

    if (osk_mode == OSK_MODE_OFF) {
        blank_bars(fb);
        return;
    }

    draw_status(fb);
    if (osk_mode == OSK_MODE_MOUSE) {
        /* The grid is not reachable in mouse mode, so drawing it would be
         * offering keys the buttons cannot press. Blank the bottom bar and
         * leave the rest of this function's tail clears to tidy the edges. */
        memset(fb + (DOS_LETTERBOX + DOS_ACTIVE_ROWS) * OSK_W, C_BLACK,
               DOS_LETTERBOX * OSK_W);
        return;
    }
    draw_row(fb, OSK_BOT_Y0, 0);
    draw_row(fb, OSK_BOT_Y1, 1);

    /* The two unused scan lines at the bottom of the bar. */
    memset(fb + (OSK_BOT_Y1 + 8) * OSK_W, C_BLACK,
           (DOS_LCD_HEIGHT - (OSK_BOT_Y1 + 8)) * OSK_W);
    memset(fb + (DOS_LETTERBOX + DOS_ACTIVE_ROWS) * OSK_W, C_BLACK,
           (OSK_BOT_Y0 - (DOS_LETTERBOX + DOS_ACTIVE_ROWS)) * OSK_W);
    memset(fb + (OSK_BOT_Y0 + 8) * OSK_W, C_BLACK,
           (OSK_BOT_Y1 - (OSK_BOT_Y0 + 8)) * OSK_W);
}

/* ---------------------------------------------------------------------------
 * Sending
 * ------------------------------------------------------------------------- */

/* Mirrors a2shift_tbl (bios.asm:4016) for the printable range. The SDL decode
 * path never consults that table, so we have to. */
static bool osk_needs_shift(uint16_t keysym)
{
    if (keysym >= 0x100)
        return false;
    if (keysym == 0)
        return false;
    if (keysym >= 'A' && keysym <= 'Z')
        return true;
    return strchr("!\"#$%&()*+:<>?@^_{|}~", (int)keysym) != NULL;
}

static void osk_echo_push(char c)
{
    if (c < 0x20 || c > 0x7E)
        return;
    if (osk_echo_len >= OSK_ECHO_MAX) {
        memmove(osk_echo, osk_echo + 1, OSK_ECHO_MAX - 1);
        osk_echo_len = OSK_ECHO_MAX - 1;
    }
    osk_echo[osk_echo_len++] = c;
    osk_echo[osk_echo_len]   = '\0';
}

static void osk_send(uint16_t keysym)
{
    uint16_t mods = osk_sticky;

    if (osk_needs_shift(keysym))
        mods |= DOS_KMOD_SHIFT;

    /* Down carries the modifiers; up must NOT. INT 7h zeroes keyflags1 on every
     * SDL event and then re-adds whatever modifier bits the word carries
     * (bios.asm:436-462), so a key-up with Shift set would leave Shift latched
     * in the BDA with nothing to clear it. */
    dos_key_event((unsigned short)(DOS_KEY_SDL | mods | keysym));
    dos_key_event((unsigned short)(DOS_KEY_SDL | DOS_KEY_UP | keysym));

    if (keysym < 0x100) {
        char c = (char)keysym;
        if ((mods & DOS_KMOD_SHIFT) && c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        osk_echo_push(c);
    }

    /* Sticky, not locking: one keypress consumes the latch. */
    osk_sticky = 0;
}

/* ---------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------- */

static void osk_clamp_col(void)
{
    uint8_t n = osk_layout[osk_layer][osk_row].n;
    if (osk_col >= n)
        osk_col = (uint8_t)(n - 1);
}

static void osk_press(void)
{
    const osk_row_t *row = &osk_layout[osk_layer][osk_row];
    const osk_key_t *k   = &row->keys[osk_col];

    switch (k->action) {
    case OSK_ACT_KEY:
        osk_send(k->keysym);
        break;
    case OSK_ACT_SHIFT:
        osk_sticky ^= DOS_KMOD_SHIFT;
        break;
    case OSK_ACT_CTRL:
        osk_sticky ^= DOS_KMOD_CTRL;
        break;
    case OSK_ACT_ALT:
        osk_sticky ^= DOS_KMOD_ALT;
        break;
    case OSK_ACT_LAYER:
        osk_layer = (uint8_t)((osk_layer + 1) % OSK_LAYERS);
        osk_clamp_col();
        break;
    case OSK_ACT_CLOSE:
        osk_set_mode(OSK_MODE_OFF);
        break;
    case OSK_ACT_MOUSE:
        osk_set_mode(OSK_MODE_MOUSE);
        break;
    case OSK_ACT_CURSOR:
        dos_mouse_ui_set_render(!dos_mouse_ui_render());
        break;
    }
}

void dos_osk_input(const odroid_gamepad_state_t *js)
{
    uint32_t now  = 0;
    uint32_t went = 0;

    /* Rebuild a bitmask rather than trusting js->bitmask: odroid_input.c fills
     * values[] unconditionally and the bitmask only on some paths. */
    for (int b = 0; b < ODROID_INPUT_MAX; b++)
        if (js->values[b])
            now |= 1u << b;

    went = now & ~osk_prev_buttons;
    osk_prev_buttons = now;

    /* ---- mouse mode ------------------------------------------------------
     *
     * Taken BEFORE the `if (!went)` early-out, and that is load-bearing: the
     * grid is edge-driven because a key must not repeat, but a pointer is
     * LEVEL-driven -- a held direction has to produce motion on every frame or
     * there is nothing for the acceleration ramp to ramp. So this arm gets the
     * raw state, every frame, and returns before any grid navigation runs.
     *
     * TIME (ODROID_INPUT_SELECT -- see dos_input.c's naming warning) steps BACK
     * to the key grid. That is a deliberate retention, not an oversight: GAME
     * only goes forwards round the cycle, so without it the way from mouse mode
     * to the keyboard is two presses through OFF, which drops the guest's
     * buttons back to the game in between. It is also the arm a future per-mode
     * TIME behaviour would replace -- it is one branch, in one place.
     *
     * GAME cycles on to OFF, which dos_input.c handles a level up, and which is
     * why there is always a way out even from in here. */
    if (osk_mode == OSK_MODE_MOUSE) {
        if (went & (1u << ODROID_INPUT_SELECT)) {
            osk_set_mode(OSK_MODE_KBD);
            return;
        }
        if (went & (1u << ODROID_INPUT_X)) {    /* START, zelda only */
            dos_mouse_ui_set_render(!dos_mouse_ui_render());
            osk_mark_dirty();
        }
        dos_mouse_ui_input(js);
        osk_mark_dirty();       /* the status line shows a live position */
        return;
    }

    if (!went)
        return;

    if (went & (1u << ODROID_INPUT_LEFT)) {
        uint8_t n = osk_layout[osk_layer][osk_row].n;
        osk_col = (uint8_t)((osk_col + n - 1) % n);
    }
    if (went & (1u << ODROID_INPUT_RIGHT)) {
        uint8_t n = osk_layout[osk_layer][osk_row].n;
        osk_col = (uint8_t)((osk_col + 1) % n);
    }
    if (went & ((1u << ODROID_INPUT_UP) | (1u << ODROID_INPUT_DOWN))) {
        osk_row = (uint8_t)(osk_row ^ 1);
        osk_clamp_col();
    }
    if (went & (1u << ODROID_INPUT_A))
        osk_press();
    if (went & (1u << ODROID_INPUT_B))
        osk_send('\b');                 /* the most-used key gets a button */
    if (went & (1u << ODROID_INPUT_SELECT)) {   /* TIME on the case */
        osk_layer = (uint8_t)((osk_layer + 1) % OSK_LAYERS);
        osk_clamp_col();
    }

    osk_mark_dirty();
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

void dos_osk_reset(void)
{
    osk_mode         = OSK_MODE_OFF;
    osk_layer        = 0;
    osk_row          = 0;
    osk_col          = 0;
    osk_sticky       = 0;
    osk_prev_buttons = 0;
    osk_echo[0]      = '\0';
    osk_echo_len     = 0;
    osk_seen_mode    = mem[0x449];
    osk_seen_cols    = mem[0x44A];
    osk_paints       = 0;
    dos_mouse_ui_reset();
}

/* "Visible" is really "the OSK owns the buttons", which is what dos_input.c
 * asks. Both non-OFF modes own them: the key grid drives a cursor over cells,
 * mouse mode drives a pointer over the guest image, and in neither case may the
 * d-pad also reach the guest's keyboard. */
bool dos_osk_visible(void) { return osk_mode != OSK_MODE_OFF; }

/* GAME. OFF -> KBD -> MOUSE -> OFF.
 *
 * One button rather than two because GAME is the only spare button present on
 * BOTH unit variants -- a mario has no START (Core/Inc/main.h) -- and
 * docs/input/02-button-mapping.md's DECIDED note is that the door to the OSK
 * must be on a button the whole fleet has. Adding a second door on START would
 * make mouse mode a zelda-only feature. Cycling costs at most two presses to
 * reach any mode and never strands the user, because the cycle always returns
 * to OFF. */
void dos_osk_cycle(void)
{
    osk_set_mode((uint8_t)((osk_mode + 1u) % OSK_MODE_COUNT));
}
