/* MS-DOS core video layer -- text modes 0-3 and CGA graphics modes 4/5/6.
 *
 * Upstream 8086tiny has no graphical text mode at all: it renders only in
 * graphics modes and sends text to stdout through termios. DOS boots into text
 * mode, so this is the port's primary display path and is written from scratch.
 * See external/8086tiny/docs/video/04-text-rendering.md.
 *
 * Upstream's graphics blit is not reused either: it packed two RGB332 colours
 * per 32-bit word, rendered CGA pixel-doubled at 640x400, and addressed video
 * memory through a ~250 KB precomputed vid_addr_lookup table. All three are
 * wrong for a 320x240 LUT8 panel with 101 KB of headroom. We read the real
 * 320x200 framebuffer natively and emit palette indices.
 * See external/8086tiny/docs/video/05-graphics-blit.md.
 */

#include "dos_video.h"

#include "gw_lcd.h"

#include <stdio.h>
#include <string.h>

/* Synthesised CGA self-test level; see the DOS_VIDEO_TEST block near the bottom
 * of this file. Declared up here so dos_video_init() can announce a test build:
 * the test functions are static and inlined, so there is no symbol for `nm` to
 * find and a contaminated shared build tree otherwise just looks like broken
 * video. */
#ifndef DOS_VIDEO_TEST
#define DOS_VIDEO_TEST 0
#endif

/* Guest memory access. dos_fold() in 8086tiny.c maps the guest's 1 MB space
 * into a 768 KB array; raw guest addresses must never index mem[] directly,
 * which is exactly what this accessor exists to prevent. */
extern unsigned char *dos_mem_ptr(unsigned int guest_addr);
/* 256 entries x 3 channels, 6 bits each, live in 8086tiny.c's DAC model
 * (ports 0x3C7/0x3C8/0x3C9). */
extern const unsigned char *dos_vga_palette(void);
extern unsigned char io_ports[];

/* ---------------------------------------------------------------------------
 * BIOS Data Area
 *
 * 8086tiny's BIOS lays out a standard BDA at segment 0x40 and maintains it on
 * every INT 10h mode set (bios_source/bios.asm:1097, :1128, declarations at
 * :3609+). Offsets verified against that declaration block.
 * ------------------------------------------------------------------------- */
#define BDA_VIDMODE      0x449   /* current video mode                      */
#define BDA_VID_COLS     0x44A   /* word: column count                      */
#define BDA_PAGE_SIZE    0x44C   /* word: bytes per page (0x1000)           */
#define BDA_CURPOS       0x450   /* 8 pages x (col, row)                    */
#define BDA_CUR_V_END    0x460   /* cursor last scanline                    */
#define BDA_CUR_V_START  0x461   /* cursor first scanline (note the order)  */
#define BDA_DISP_PAGE    0x462   /* active display page                     */
#define BDA_CRT_START    0x4AD   /* word: CRTC start address, in characters */

/* NOTE: BDA 0x484 (vid_rows) is NOT usable here. A real PC BIOS stores
 * rows-1; 8086tiny declares `vid_rows db 25` (bios.asm:3662) -- the literal
 * count, not count-1 -- and never reads it anywhere. Treating it as rows-1
 * would render 26 rows. We use the fixed 25 the BIOS actually implements. */

#define CGA_MODE_CTRL    0x3D8   /* bit 1: graphics, 4: 640x200, 5: blink    */
#define CGA_MODE_GRAPHICS 0x02
#define CGA_MODE_HIRES    0x10
#define CGA_BLINK_ENABLE  0x20

#define CGA_COLOR_SEL    0x3D9   /* bits 0-3 bg, bit 4 intensity, 5 palette  */
#define CGA_COLSEL_BG     0x0F
#define CGA_COLSEL_INTENS 0x10
#define CGA_COLSEL_PAL1   0x20

#define VRAM_BASE        0xB8000 /* guest base of colour text and CGA gfx    */
#define TEXT_VRAM_MASK   0x7FFF  /* 32 KB window at 0xB8000-0xBFFFF          */
#define GFX_VRAM_SIZE    0x4000  /* CGA graphics is 16 KB of that            */
#define GFX_VRAM_MASK    (GFX_VRAM_SIZE - 1)
#define GFX_BANK_STRIDE  0x2000  /* odd rows live here                       */
#define GFX_BANK_MASK    (GFX_BANK_STRIDE - 1)
#define GFX_ROW_BYTES     80     /* both 320x200x2bpp and 640x200x1bpp       */
#define GFX_ROWS         200

/* Text-mode source: 0xB8000, real colour video memory, unconditionally.
 *
 * Historically this read the BIOS teletype shadow at C000:0 instead, because
 * INT 10h AH=0Eh wrote char+attr there rather than to video memory
 * (bios.asm:1802, :1847). That shadow exists to serve int10_charatcur -- it is
 * a read-char-at-cursor cache and is *never scrolled*, so it diverges from the
 * real screen the moment DOS scrolls, which is what made text look garbled.
 *
 * The BIOS now writes 0xB8000, which already has working scroll machinery, so
 * there is one source. Deliberately no heuristic: an earlier version chose
 * between the two per frame by scanning 0xB8000 for non-blank characters, and
 * that flip happening mid-boot was itself a suspected cause of garbling. One
 * source cannot flip. */

/* ---------------------------------------------------------------------------
 * Palette
 *
 * Exactly 32 entries are declared, never 16. LCD_DARKEN_BIT is fixed at +32
 * (gw_lcd.h:169) but lcd_set_clut() writes darkened twins at count+i
 * (gw_lcd.c:455); those agree only when count == 32. Declaring 16 would leave
 * the twins at [16..32) while the darken path reads [32..48), so dimming the
 * screen behind a Retro-Go menu would show black instead of a dimmed image.
 *
 * CGA's 16 colours occupy [0..16), which is what text mode emits directly.
 *
 * [16..32) is ours. Slots 16-19 hold the *four currently selected CGA graphics
 * colours*, so the graphics blit emits 16+pixel and a guest write to port 0x3D9
 * costs a CLUT reprogram rather than a repaint of the frame (or a rebuild of the
 * 1 KB expansion table). 20-31 mirror colours 4-15 so no stray index lands on an
 * uninitialised slot.
 * ------------------------------------------------------------------------- */
#define DOS_CLUT_COUNT 32
#define GFX_CLUT_BASE  16

/* The comment above is a live constraint, so it is checked rather than trusted.
 * lcd_set_clut() writes the darkened twin of entry i at count+i (gw_lcd.c:455)
 * while the darken path reads i+LCD_DARKEN_BIT (gw_lcd.h:169); the only count
 * that satisfies both is LCD_DARKEN_BIT itself. */
_Static_assert(DOS_CLUT_COUNT == LCD_DARKEN_BIT,
               "DOS CLUT must be exactly LCD_DARKEN_BIT (32) entries or menu dimming breaks");

static const uint32_t cga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* ---------------------------------------------------------------------------
 * Row expansion
 *
 * Turns a 4-bit glyph row into four bytes of 0x00/0xFF mask, so a glyph row
 * becomes one 32-bit store instead of four byte stores plus branches:
 *
 *     word = (mask & fg_word) | (~mask & bg_word)
 *
 * Byte 0 of the word is the leftmost pixel. The core is little-endian, so byte
 * 0 sits in the low 8 bits. Nibble bit 3 is the leftmost pixel, matching the
 * font tables generated by tools/gen_dos_font.py.
 *
 * The same table serves the 8x8 font: its high nibble expands to pixels 0-3
 * and its low nibble to pixels 4-7.
 * ------------------------------------------------------------------------- */
static uint32_t expand4[16];

/* Set when vga13_clut[] matches the guest DAC; see vga13_sync_palette(). */
static bool     vga13_clut_valid = false;

/* Graphics expansion tables. Both turn one source byte into one 32-bit store of
 * four CLUT indices, so a 320-pixel row is 80 loads and 80 stores.
 *
 *   expand2[b]  CGA 2 bpp: four pixels per byte, most significant pair
 *               leftmost. Byte 0 of the word is the leftmost pixel and the core
 *               is little-endian, so pixel x sits at bit shift 8*x.
 *   expand1[b]  CGA 1 bpp: eight pixels per byte, bit 7 leftmost, decimated to
 *               four output pixels by OR-ing adjacent pairs. Dropping the odd
 *               column instead would make single-pixel vertical lines vanish
 *               half the time, and mode 6 is line art -- see
 *               docs/video/05-graphics-blit.md.
 *
 * Values are CLUT indices already, biased by GFX_CLUT_BASE, so neither table
 * ever needs rebuilding when the guest changes palette. 2 KB of rodata-equivalent
 * BSS, against the ~250 KB address table upstream used. */
static uint32_t expand2[256];
static uint32_t expand1[256];

static void build_expand_table(void)
{
    for (unsigned n = 0; n < 16; n++) {
        uint32_t w = 0;
        for (unsigned x = 0; x < 4; x++) {
            if (n & (1u << (3 - x)))
                w |= 0xFFu << (8 * x);
        }
        expand4[n] = w;
    }

    for (unsigned b = 0; b < 256; b++) {
        uint32_t w2 = 0, w1 = 0;
        for (unsigned x = 0; x < 4; x++) {
            unsigned pix2 = (b >> (6 - 2 * x)) & 3;
            /* Source bits 7-2x and 6-2x, OR'd: a set bit in either lights the
             * output pixel. */
            unsigned pix1 = ((b >> (7 - 2 * x)) | (b >> (6 - 2 * x))) & 1;
            w2 |= (uint32_t)(GFX_CLUT_BASE + pix2) << (8 * x);
            w1 |= (uint32_t)(GFX_CLUT_BASE + pix1) << (8 * x);
        }
        expand2[b] = w2;
        expand1[b] = w1;
    }
}

/* Replicate a CLUT index across four bytes. */
static inline uint32_t splat(uint8_t idx)
{
    return (uint32_t)idx * 0x01010101u;
}

/* ---------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------- */
static uint8_t  last_mode  = 0xFF;
static uint16_t last_cols  = 0;
static bool     initialised = false;
static uint8_t  last_colsel = 0xFF;   /* last port 0x3D9 value programmed  */
static uint8_t  last_gfx_mode = 0xFF; /* which palette rule that was for   */

static inline uint16_t bda16(unsigned addr)
{
    const unsigned char *p = dos_mem_ptr(addr);
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint8_t bda8(unsigned addr)
{
    return *dos_mem_ptr(addr);
}

static void clear_framebuffers(void)
{
    /* Clears the letterbox bars too. Video owns clearing them on a mode change;
     * it must not write them at any other time -- they belong to the on-screen
     * keyboard (docs/video/06-framebuffer-clut.md). Both buffers are cleared so
     * bar content cannot flicker at the swap rate. */
    memset(lcd_get_active_buffer(),   0, DOS_LCD_WIDTH * DOS_LCD_HEIGHT);
    memset(lcd_get_inactive_buffer(), 0, DOS_LCD_WIDTH * DOS_LCD_HEIGHT);
}

/* Push all 32 entries: 0-15 the fixed CGA palette (text mode emits these
 * directly), 16-19 the four currently selected graphics colours, 20-31 filler.
 *
 * Note this is a full re-push, not a partial update -- lcd_set_clut() rebuilds
 * the darkened twins as well, and those must stay consistent with slots 16-19 or
 * a Retro-Go menu over a graphics screen would dim to stale colours. */
static void program_clut(const uint8_t gfx[4])
{
    uint32_t clut[DOS_CLUT_COUNT];

    for (unsigned i = 0; i < 16; i++)
        clut[i] = cga_palette[i];
    for (unsigned i = 0; i < 4; i++)
        clut[GFX_CLUT_BASE + i] = cga_palette[gfx[i] & 15];
    for (unsigned i = GFX_CLUT_BASE + 4; i < DOS_CLUT_COUNT; i++)
        clut[i] = cga_palette[i & 15];

    lcd_set_clut(clut, DOS_CLUT_COUNT);
}

/* Resolve port 0x3D9 into the four on-screen CGA colours.
 *
 * Mode 4:  colour 0 is the background (any of 16, bits 0-3); colours 1-3 come
 *          from the palette-select bit (green/red/brown or cyan/magenta/grey),
 *          promoted to their high-intensity twins by bit 4.
 * Mode 5:  the same layout with the fixed cyan/red/white palette (the
 *          colour-burst-off variant), background still from bits 0-3.
 * Mode 6:  two colours only. Pixel 0 is black; pixel 1 takes the foreground
 *          from bits 0-3. Entries 2-3 are unreachable -- the 1 bpp table only
 *          ever emits 0 and 1 -- but are filled so the CLUT has no holes. */
static void cga_select_colors(uint8_t mode, uint8_t colsel, uint8_t out[4])
{
    const uint8_t bg     = colsel & CGA_COLSEL_BG;
    const uint8_t bright = (colsel & CGA_COLSEL_INTENS) ? 0x08 : 0x00;

    if (mode == 6) {
        out[0] = 0;
        out[1] = bg ? bg : 15;   /* 0x3D9 untouched (0) means white-on-black */
        out[2] = out[3] = out[1];
        return;
    }

    out[0] = bg;

    if (mode == 5) {
        out[1] = 3 | bright;     /* cyan  */
        out[2] = 4 | bright;     /* red   */
        out[3] = 7 | bright;     /* white */
    } else if (colsel & CGA_COLSEL_PAL1) {
        out[1] = 3 | bright;     /* cyan     */
        out[2] = 5 | bright;     /* magenta  */
        out[3] = 7 | bright;     /* grey     */
    } else {
        out[1] = 2 | bright;     /* green */
        out[2] = 4 | bright;     /* red   */
        out[3] = 6 | bright;     /* brown */
    }
}

/* Reprogram slots 16-19 if, and only if, the guest's selection changed. */
static void cga_sync_palette(uint8_t mode)
{
    const uint8_t colsel = io_ports[CGA_COLOR_SEL];

    if (colsel == last_colsel && mode == last_gfx_mode)
        return;

    uint8_t gfx[4];
    cga_select_colors(mode, colsel, gfx);
    program_clut(gfx);

    last_colsel   = colsel;
    last_gfx_mode = mode;
}

void dos_video_init(void)
{
#if DOS_VIDEO_TEST
    printf("DOS: *** VIDEO SELFTEST BUILD (DOS_VIDEO_TEST=%d) -- not a normal image%s\n",
           DOS_VIDEO_TEST,
           DOS_VIDEO_TEST > 1 ? "; the guest will NOT be displayed" : "");
#endif

    build_expand_table();

    /* Text-mode default: slots 16-19 get the mode 4 palette-0 colours. They are
     * unused until a graphics mode is entered, at which point cga_sync_palette()
     * replaces them. */
    static const uint8_t gfx_default[4] = { 0, 2, 4, 6 };
    program_clut(gfx_default);

    clear_framebuffers();

    last_mode = 0xFF;
    last_cols = 0;
    last_colsel = 0xFF;
    last_gfx_mode = 0xFF;
    vga13_clut_valid = false;
    initialised = true;
}

/* ---------------------------------------------------------------------------
 * Text rendering
 * ------------------------------------------------------------------------- */

static void blit_text(uint8_t *fb, uint16_t cols)
{
    const uint8_t (*font)[8] = (cols >= 80) ? dos_font_4x8 : dos_font_8x8;
    const unsigned cell_w    = (cols >= 80) ? 4 : 8;
    const bool     narrow    = (cols >= 80);

    /* Active page. Needed for the cursor position lookup regardless of which
     * buffer we render from, so it stays out here. */
    const unsigned page = bda8(BDA_DISP_PAGE) & 7;

    /* Text mode deliberately IGNORES the CRTC start address (BDA 0x4AD).
     *
     * On real hardware software scrolls by moving the start address instead of
     * moving 4 KB of memory, so honouring it is normally mandatory. This BIOS
     * does not work that way: int10_write_char / int10_write_char_attrib and the
     * scroll routines all address 0xB8000 flat from offset 0 and ignore
     * vmem_offset -- only vmem_driver_entry consults it. Adding the offset here
     * would therefore disagree with how the BIOS itself scrolls.
     *
     * Graphics modes are the opposite case and do honour it -- see blit_cga(). */
    unsigned page_size = bda16(BDA_PAGE_SIZE);
    if (page_size == 0)
        page_size = 0x1000;

    /* Colour text memory is the 32 KB window at 0xB8000-0xBFFFF. Wrap the page
     * offset inside it: it comes from a guest-writable BDA field, and an
     * out-of-range value would otherwise fold to the scratch page and render
     * garbage rather than wrapping like real hardware. */
    const unsigned base = (page * page_size) & TEXT_VRAM_MASK;

    /* Row addresses wrap inside the window too, so a start address near the top
     * of it scrolls round rather than folding to the scratch page. */
    #define TEXT_ROW_PTR(r) \
        dos_mem_ptr(VRAM_BASE + ((base + (r) * cols * 2) & TEXT_VRAM_MASK))

    const bool blink_enabled = (io_ports[CGA_MODE_CTRL] & CGA_BLINK_ENABLE) != 0;

    /* ~1.9 Hz: the CGA divides vertical refresh by 16, giving 16 frames on and
     * 16 off at 60 Hz, which is bit 4 of the frame counter. */
    const bool blink_on = (lcd_get_frame_counter() & 0x10) != 0;

    uint8_t *dst_top = fb + DOS_LETTERBOX * DOS_LCD_WIDTH;

    for (unsigned row = 0; row < DOS_TEXT_ROWS; row++) {
        const unsigned char *cell = TEXT_ROW_PTR(row);
        uint8_t *dst_row = dst_top + row * 8 * DOS_LCD_WIDTH;

        for (unsigned col = 0; col < cols; col++) {
            uint8_t ch   = cell[col * 2];
            uint8_t attr = cell[col * 2 + 1];

            uint8_t fg = attr & 0x0F;
            uint8_t bg = (attr >> 4) & 0x07;

            /* Bit 7 is blink or background intensity, depending on the CGA
             * mode-control register. Software that wants 16 background colours
             * clears that bit explicitly.
             *
             * The intensity promotion is conditional on bit 7 being SET in this
             * cell -- it is that bit reinterpreted, not a global mode. Applying
             * it unconditionally turns every black background into dark grey
             * (index 0 -> 8), which is a grey screen rather than a black one. */
            if (blink_enabled) {
                if ((attr & 0x80) && !blink_on)
                    fg = bg;            /* blinked off: draw as background */
            } else if (attr & 0x80) {
                bg |= 0x08;             /* bit 7 = background intensity */
            }

            const uint32_t fgw = splat(fg);
            const uint32_t bgw = splat(bg);
            const uint8_t *glyph = font[ch];

            /* Cell origins are 4-byte aligned: rows are 320 bytes (a multiple
             * of 4), cells are 4 or 8 bytes wide, and the framebuffer base is
             * linker-aligned. The 32-bit stores below are therefore safe. */
            uint8_t *dst = dst_row + col * cell_w;

            if (narrow) {
                for (unsigned y = 0; y < 8; y++) {
                    uint32_t mask = expand4[glyph[y] & 0x0F];
                    *(uint32_t *)(dst + y * DOS_LCD_WIDTH) =
                        (mask & fgw) | (~mask & bgw);
                }
            } else {
                for (unsigned y = 0; y < 8; y++) {
                    uint8_t  g  = glyph[y];
                    uint32_t m0 = expand4[(g >> 4) & 0x0F];
                    uint32_t m1 = expand4[g & 0x0F];
                    uint32_t *p = (uint32_t *)(dst + y * DOS_LCD_WIDTH);
                    p[0] = (m0 & fgw) | (~m0 & bgw);
                    p[1] = (m1 & fgw) | (~m1 & bgw);
                }
            }
        }
    }

    /* Cursor. Shape comes from BDA 0x460 (end) / 0x461 (start) -- reversed
     * order, matching real hardware and bios.asm:3646-3647. start > end means
     * the cursor is HIDDEN, which is how software turns it off; treating it as
     * an inverted range would paint a block over the character instead. */
    uint8_t c_end   = bda8(BDA_CUR_V_END);
    uint8_t c_start = bda8(BDA_CUR_V_START);

    if (c_start <= c_end && c_start < 8 && blink_on) {
        unsigned cx = bda8(BDA_CURPOS + page * 2);
        unsigned cy = bda8(BDA_CURPOS + page * 2 + 1);

        if (cx < cols && cy < DOS_TEXT_ROWS) {
            const unsigned char *cell = TEXT_ROW_PTR(cy);
            uint8_t fg = cell[cx * 2 + 1] & 0x0F;
            uint32_t fgw = splat(fg);

            uint8_t *dst = dst_top + cy * 8 * DOS_LCD_WIDTH + cx * cell_w;
            unsigned y_end = (c_end < 7) ? c_end : 7;

            for (unsigned y = c_start; y <= y_end; y++) {
                uint32_t *p = (uint32_t *)(dst + y * DOS_LCD_WIDTH);
                p[0] = fgw;
                if (!narrow)
                    p[1] = fgw;
            }
        }
    }

    #undef TEXT_ROW_PTR
}

/* ---------------------------------------------------------------------------
 * CGA graphics rendering
 *
 * Modes 4 and 5 are 320x200 at 2 bpp, mode 6 is 640x200 at 1 bpp. Both share
 * one layout: 80 bytes per row, and scanlines split across two banks -- even
 * rows from aperture offset 0, odd rows from 0x2000. So
 *
 *     row_offset(y) = (y & 1) * 0x2000 + ((start + (y >> 1) * 80) & 0x1FFF)
 *
 * The wrap is *per bank*, at 8 KB, not across the whole 16 KB buffer. On a real
 * CGA the 6845 supplies only MA0-MA11 -- 4096 character positions of 2 bytes,
 * i.e. 8 KB -- and the bank bit comes from the scanline row counter RA0, outside
 * the address counter entirely. So a start address large enough to run an even
 * row off the end of bank 0 wraps to the *base of bank 0*, never into bank 1.
 * docs/video/05-graphics-blit.md says "wrap within the mode's VRAM size" (16 KB);
 * that is the wrong granularity, and under it an odd row that overran would have
 * wrapped into the even bank. The two agree for every start below 0x110 bytes,
 * which is why a start-at-zero test cannot tell them apart.
 *
 * 320x200 lands 1:1 on the panel with the 20-row letterbox, so there is no
 * scaling in either mode; mode 6 halves horizontally by OR-ing pixel pairs.
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * VGA mode 13h -- 320x200, 256 colours, LINEAR
 *
 * One byte per pixel at guest A000:0000, row y at y*320, 64000 bytes. No banks,
 * no interleave, no planes, no CRTC start address to honour. It is byte-for-byte
 * our LUT8 framebuffer format, so the blit is a memcpy per row into the
 * letterboxed origin -- 200 rows of 320, never touching rows 0-19 or 220-239.
 *
 * Deliberately NOT a single 64000-byte memcpy: the destination is contiguous
 * only because the panel happens to be exactly 320 wide. Keeping the per-row
 * form means the letterbox and any future non-320-wide mode cost nothing.
 *
 * The palette is a CLUT reprogram, not a repaint: the guest's byte IS the CLUT
 * index. That is what makes palette animation and fades -- how most DOS-era
 * 256-colour effects are done -- free here.
 * ------------------------------------------------------------------------- */
#define VGA13_BASE   0xA0000   /* guest base of the linear 8bpp aperture */
#define VGA13_WIDTH  320
#define VGA13_ROWS   200

/* RGB888 mirror of the guest DAC, rebuilt only when the DAC changed. Lives in
 * the DOS overlay, NOT in firmware DTCM -- the whole point of
 * lcd_set_clut_ext() taking a caller-owned array. */
static uint32_t vga13_clut[256];
static uint8_t  vga13_dac_shadow[256 * 3];

static void vga13_sync_palette(void)
{
    const unsigned char *dac = dos_vga_palette();

    /* 768-byte compare against a shadow rather than a dirty flag from the DAC
     * write path. A dirty flag would be one more thing on the guest's OUT path
     * and one more thing to get wrong; 768 bytes of memcmp is nothing next to
     * the 64000-byte blit beside it, and it cannot miss an update. */
    if (vga13_clut_valid && memcmp(vga13_dac_shadow, dac, sizeof vga13_dac_shadow) == 0)
        return;

    memcpy(vga13_dac_shadow, dac, sizeof vga13_dac_shadow);

    for (unsigned i = 0; i < 256; i++) {
        /* 6 bits to 8 as (v << 2) | (v >> 4), so 63 becomes 255 and not 252 --
         * a plain << 2 makes every "white" 1.2% grey and every fade end short
         * of full brightness. */
        const unsigned r = dac[i * 3 + 0], g = dac[i * 3 + 1], b = dac[i * 3 + 2];
        vga13_clut[i] = (uint32_t)(((r << 2 | r >> 4) & 0xFF) << 16) |
                        (uint32_t)(((g << 2 | g >> 4) & 0xFF) <<  8) |
                        (uint32_t)( ((b << 2 | b >> 4) & 0xFF)      );
    }

    lcd_set_clut_ext(vga13_clut, 256);
    vga13_clut_valid = true;
}

static void blit_vga13(uint8_t *fb)
{
    /* The aperture folds contiguously, so one dos_mem_ptr() covers all 64000
     * bytes and rows are plain offsets. Do NOT reach for a fold constant here --
     * where 0xA0000 lands is dos_fold()'s business. */
    const uint8_t *src = (const uint8_t *)dos_mem_ptr(VGA13_BASE);
    uint8_t *dst = fb + DOS_LETTERBOX * DOS_LCD_WIDTH;

    for (unsigned y = 0; y < VGA13_ROWS; y++) {
        memcpy(dst, src, VGA13_WIDTH);
        src += VGA13_WIDTH;
        dst += DOS_LCD_WIDTH;
    }
}

static void blit_cga(uint8_t *fb, uint8_t mode)
{
    /* The whole 64 KB aperture folds contiguously (guest 0xB0000-0xBFFFF ->
     * folded 0xA0000-0xAFFFF, 8086tiny.c:249), so one dos_mem_ptr() call covers
     * the entire 16 KB graphics buffer and offsets can be masked rather than
     * re-folded per row. */
    const unsigned char *vram = dos_mem_ptr(VRAM_BASE);

    /* CRTC start address. Games that hardware-scroll move this instead of moving
     * 16 KB of pixels, so ignoring it renders a static or torn image. 8086tiny
     * assembles it from CRTC registers 12/13 into a little-endian word at
     * mem[0x4AD] (8086tiny.c:750), the same field text mode uses.
     *
     * The CRTC counts in character clocks, which is 2 bytes per unit in CGA
     * graphics modes just as it is in text -- hence the *2. (docs/video/
     * 05-graphics-blit.md calls this "a byte offset"; that does not match how
     * the register works or how 8086tiny stores it.)
     *
     * Verified: 8086tiny.c:780 stores CRTC register 13 (low) at mem[0x4AD] and
     * register 12 (high) at mem[0x4AE], so bda16() reads the right little-endian
     * word; :767/:781 then use that same field in *character* units for the text
     * cursor, confirming the unit. Every CGA graphics mode programs the CRTC with
     * h_displayed = 40 (two bytes fetched per character clock), so 40 characters
     * is the 80-byte row -- hence the *2.
     *
     * Masked to the bank, not the buffer: it is guest-controlled, and per-bank is
     * what the hardware does (see the header comment). */
    const unsigned start = (bda16(BDA_CRT_START) * 2) & GFX_BANK_MASK;

    const uint32_t *expand = (mode == 6) ? expand1 : expand2;

    /* Destination origin is exactly framebuffer + 20*320 and exactly 200 rows;
     * the letterbox bars belong to the compositing layer (the future on-screen
     * keyboard) and are never written here. */
    uint8_t *dst_row = fb + DOS_LETTERBOX * DOS_LCD_WIDTH;

    for (unsigned y = 0; y < GFX_ROWS; y++, dst_row += DOS_LCD_WIDTH) {
        /* Bank base is chosen by scanline parity and is never part of the
         * wrapping arithmetic; only the in-bank offset wraps. Both terms are
         * bounded by construction, so no 16 KB mask is needed afterwards. */
        const unsigned bank = (y & 1) * GFX_BANK_STRIDE;
        const unsigned off  = bank + ((start + (y >> 1) * GFX_ROW_BYTES) & GFX_BANK_MASK);

        /* A non-zero start address can leave a row straddling the end of the
         * bank. Real hardware wraps to the base of the same bank, so the row is
         * fetched in up to two runs rather than reading past the end or bleeding
         * into the other bank. */
        unsigned first = GFX_BANK_STRIDE - (off - bank);
        if (first > GFX_ROW_BYTES)
            first = GFX_ROW_BYTES;

        /* dst_row is 4-byte aligned (320 is a multiple of 4, DOS_LETTERBOX*320
         * likewise, and the framebuffer base is linker-aligned), so the 32-bit
         * stores are safe. Each source byte is four output pixels in both modes.
         */
        uint32_t *out = (uint32_t *)dst_row;
        const unsigned char *src = vram + off;

        for (unsigned x = 0; x < first; x++)
            out[x] = expand[src[x]];
        for (unsigned x = first; x < GFX_ROW_BYTES; x++)
            out[x] = expand[vram[bank + (x - first)]];
    }
}

/* ---------------------------------------------------------------------------
 * Synthesised CGA self-test
 *
 * Enabled with DOS_CFLAGS_EXTRA=-DDOS_VIDEO_TEST=1, in the same style as
 * DOS_DEBUG_STATUS (main_dos.c) and DOS_TRACE_INSNS (8086tiny.c): off by
 * default, compiled out entirely, kept in the tree as a regression check.
 *
 * Why a synthesised pattern rather than a game: a known pattern has a known
 * correct rendering, so every failure names itself. Two-bank interleave, 2 bpp
 * pixel order, mode 6 decimation, letterbox containment, CRTC start wrapping and
 * the palette path can each be wrong independently, and each gets its own check.
 *
 *   1  run the checks once on the first blit, log a line per failure plus a
 *      summary, then FALL THROUGH to normal rendering for the rest of the run.
 *   2  additionally hold a fixed demo pattern on screen every frame, for a
 *      stable screenshot across the buffer swap. The guest never renders.
 *
 * Level 1 falls through deliberately. An earlier version always held the demo
 * pattern and returned before the mode dispatch, which meant a shared build tree
 * accidentally left with the flag on showed the test pattern for every guest and
 * looked exactly like broken video rather than a stale build flag. Level 1 now
 * degrades to log noise instead of no video at all, and dos_video_init() prints a
 * banner so a contaminated build announces itself. (There is no symbol to find:
 * the test functions are static and get inlined, so `nm` cannot detect this.)
 *
 * Either level writes the guest's video aperture and BDA 0x4AD once, so the
 * guest's screen is clobbered at startup; at level 1 the guest repaints it.
 *
 * The expectations are literal constants wherever a literal is checkable by
 * hand, precisely so a shared misconception between blit and test cannot pass.
 * The full-frame sweep additionally recomputes addressing independently.
 * ------------------------------------------------------------------------- */
#if DOS_VIDEO_TEST

#define G(i) (GFX_CLUT_BASE + (i))          /* expected CLUT index for pixel i */

static unsigned test_fails;
static unsigned test_checks;

#define CHK(cond, ...)                                                        \
    do {                                                                      \
        test_checks++;                                                        \
        if (!(cond)) { test_fails++; printf("DOSVT: FAIL " __VA_ARGS__); }     \
    } while (0)

/* Byte `n` of a packed expansion word == output pixel n, leftmost first. */
static inline uint8_t wbyte(uint32_t w, unsigned n) { return (uint8_t)(w >> (8 * n)); }

static inline uint8_t *test_vram(void)   { return dos_mem_ptr(VRAM_BASE); }

/* Pixel at guest coordinate (x,y); y is a guest row, so the letterbox offset is
 * applied here and nowhere else. */
static inline uint8_t px(const uint8_t *fb, unsigned x, unsigned y)
{
    return fb[(DOS_LETTERBOX + y) * DOS_LCD_WIDTH + x];
}

static void test_set_start(uint16_t chars)
{
    uint8_t *p = dos_mem_ptr(BDA_CRT_START);
    p[0] = (uint8_t)(chars & 0xFF);
    p[1] = (uint8_t)(chars >> 8);
}

/* -------- 1. expansion tables, against hand-computed literals -------------- */
static void test_tables(void)
{
    /* 0x1B = 00 01 10 11, most significant pair leftmost. */
    CHK(wbyte(expand2[0x1B], 0) == G(0) && wbyte(expand2[0x1B], 1) == G(1) &&
        wbyte(expand2[0x1B], 2) == G(2) && wbyte(expand2[0x1B], 3) == G(3),
        "expand2[0x1B]=%08lX want %d,%d,%d,%d\n",
        (unsigned long)expand2[0x1B], G(0), G(1), G(2), G(3));

    /* 0xE4 = 11 10 01 00 -- the reverse, so a byte-order slip cannot pass both. */
    CHK(wbyte(expand2[0xE4], 0) == G(3) && wbyte(expand2[0xE4], 1) == G(2) &&
        wbyte(expand2[0xE4], 2) == G(1) && wbyte(expand2[0xE4], 3) == G(0),
        "expand2[0xE4]=%08lX want %d,%d,%d,%d\n",
        (unsigned long)expand2[0xE4], G(3), G(2), G(1), G(0));

    /* 0x40 = 01 00 00 00: colour only in the leftmost pixel. */
    CHK(expand2[0x40] == ((uint32_t)G(1) | ((uint32_t)G(0) * 0x01010100u)),
        "expand2[0x40]=%08lX\n", (unsigned long)expand2[0x40]);
    CHK(expand2[0x00] == splat(G(0)), "expand2[0x00]=%08lX\n", (unsigned long)expand2[0x00]);
    CHK(expand2[0xFF] == splat(G(3)), "expand2[0xFF]=%08lX\n", (unsigned long)expand2[0xFF]);

    /* Mode 6 decimation is OR, not drop-odd. Every one of the eight isolated
     * source bits must light output pixel k/2 -- this is the entire reason OR was
     * chosen, and drop-odd fails it for k=1,3,5,7. Position within the row is
     * irrelevant because the table is indexed by byte value alone, so eight
     * checks cover all 640 columns. */
    for (unsigned k = 0; k < 8; k++) {
        const uint32_t w = expand1[0x80u >> k];
        for (unsigned p = 0; p < 4; p++) {
            const uint8_t want = (p == k / 2) ? G(1) : G(0);
            CHK(wbyte(w, p) == want,
                "expand1[bit%u] pixel%u=%d want %d (drop-odd bug?)\n",
                k, p, wbyte(w, p), want);
        }
    }
    CHK(expand1[0xAA] == splat(G(1)), "expand1[0xAA]=%08lX\n", (unsigned long)expand1[0xAA]);
    CHK(expand1[0x55] == splat(G(1)), "expand1[0x55]=%08lX\n", (unsigned long)expand1[0x55]);
    CHK(expand1[0x00] == splat(G(0)), "expand1[0x00]=%08lX\n", (unsigned long)expand1[0x00]);
    CHK(expand1[0xFF] == splat(G(1)), "expand1[0xFF]=%08lX\n", (unsigned long)expand1[0xFF]);
}

/* -------- 2. interleave, stride and pixel order end to end ----------------- */
static void test_interleave(uint8_t *fb)
{
    uint8_t *v = test_vram();
    test_set_start(0);
    memset(v, 0, GFX_VRAM_SIZE);

    /* Three literal anchors. Together they pin the bank split (0 vs 0x2000), the
     * 80-byte stride, and pixel order within a byte. A swapped interleave shows
     * up as anchor A and B trading rows; a collapsed one as row 1 reading row 0's
     * byte; a wrong stride as row 2 being blank. */
    v[0x0000] = 0x1B;    /* row 0, x 0..3 -> 0,1,2,3 */
    v[0x2000] = 0xE4;    /* row 1, x 0..3 -> 3,2,1,0 */
    v[0x0050] = 0x40;    /* row 2, x 0..3 -> 1,0,0,0 (0x50 = 80) */
    v[0x2050] = 0x03;    /* row 3, x 0..3 -> 0,0,0,3 */

    blit_cga(fb, 4);

    CHK(px(fb,0,0)==G(0) && px(fb,1,0)==G(1) && px(fb,2,0)==G(2) && px(fb,3,0)==G(3),
        "row0 = %d,%d,%d,%d want %d,%d,%d,%d\n",
        px(fb,0,0), px(fb,1,0), px(fb,2,0), px(fb,3,0), G(0), G(1), G(2), G(3));
    CHK(px(fb,0,1)==G(3) && px(fb,1,1)==G(2) && px(fb,2,1)==G(1) && px(fb,3,1)==G(0),
        "row1 (bank 0x2000) = %d,%d,%d,%d want %d,%d,%d,%d\n",
        px(fb,0,1), px(fb,1,1), px(fb,2,1), px(fb,3,1), G(3), G(2), G(1), G(0));
    CHK(px(fb,0,2)==G(1) && px(fb,1,2)==G(0) && px(fb,2,2)==G(0) && px(fb,3,2)==G(0),
        "row2 (stride 80) = %d,%d,%d,%d\n",
        px(fb,0,2), px(fb,1,2), px(fb,2,2), px(fb,3,2));
    CHK(px(fb,3,3)==G(3) && px(fb,0,3)==G(0),
        "row3 = %d..%d\n", px(fb,0,3), px(fb,3,3));

    /* Now a pattern that differs on every row and every byte, swept in full with
     * addressing recomputed independently of blit_cga(). Row parity is folded
     * into the value so a bank swap cannot alias. */
    for (unsigned y = 0; y < GFX_ROWS; y++)
        for (unsigned b = 0; b < GFX_ROW_BYTES; b++)
            v[(y & 1) * GFX_BANK_STRIDE + (y >> 1) * GFX_ROW_BYTES + b] =
                (uint8_t)(y * 7u + b * 13u + (y & 1) * 0x55u);

    blit_cga(fb, 4);

    unsigned bad = 0;
    for (unsigned y = 0; y < GFX_ROWS && bad < 4; y++) {
        for (unsigned x = 0; x < DOS_LCD_WIDTH; x++) {
            const uint8_t src =
                v[(y & 1) * GFX_BANK_STRIDE + (y >> 1) * GFX_ROW_BYTES + x / 4];
            const uint8_t want = G((src >> (6 - 2 * (x & 3))) & 3);
            if (px(fb, x, y) != want) {
                printf("DOSVT: FAIL sweep (%u,%u)=%d want %d src=%02X\n",
                       x, y, px(fb, x, y), want, src);
                bad++;
                break;
            }
        }
    }
    CHK(bad == 0, "mode4 full-frame sweep\n");
}

/* -------- 3. mode 6: 640 -> 320, both column parities survive -------------- */
static void test_mode6(uint8_t *fb)
{
    uint8_t *v = test_vram();
    test_set_start(0);
    memset(v, 0, GFX_VRAM_SIZE);

    /* Row 0: vertical lines in every EVEN 640-column. Row 1: every ODD one.
     * Both must come out as a solid 320-pixel foreground run. Drop-odd
     * decimation blanks row 1 entirely -- the failure this mode exists to
     * prevent. Row 2 stays black as a control. */
    memset(v + 0x0000, 0xAA, GFX_ROW_BYTES);
    memset(v + 0x2000, 0x55, GFX_ROW_BYTES);

    blit_cga(fb, 6);

    unsigned bad_even = 0, bad_odd = 0, bad_blank = 0;
    for (unsigned x = 0; x < DOS_LCD_WIDTH; x++) {
        if (px(fb, x, 0) != G(1)) bad_even++;
        if (px(fb, x, 1) != G(1)) bad_odd++;
        if (px(fb, x, 2) != G(0)) bad_blank++;
    }
    CHK(bad_even == 0, "mode6 even-column lines: %u/320 pixels lost\n", bad_even);
    CHK(bad_odd  == 0, "mode6 odd-column lines: %u/320 pixels lost (drop-odd, not OR?)\n", bad_odd);
    CHK(bad_blank == 0, "mode6 blank control row: %u/320 pixels set\n", bad_blank);
}

/* -------- 4. letterbox containment ---------------------------------------- */
#define TEST_SENTINEL 0xA5

static void test_letterbox(uint8_t *fb)
{
    uint8_t *v = test_vram();
    test_set_start(0);
    memset(v, 0xFF, GFX_VRAM_SIZE);          /* every guest pixel non-zero */

    memset(fb, TEST_SENTINEL, DOS_LETTERBOX * DOS_LCD_WIDTH);
    memset(fb + (DOS_LETTERBOX + GFX_ROWS) * DOS_LCD_WIDTH, TEST_SENTINEL,
           DOS_LETTERBOX * DOS_LCD_WIDTH);

    blit_cga(fb, 4);

    unsigned top = 0, bot = 0;
    for (unsigned i = 0; i < DOS_LETTERBOX * DOS_LCD_WIDTH; i++) {
        if (fb[i] != TEST_SENTINEL) top++;
        if (fb[(DOS_LETTERBOX + GFX_ROWS) * DOS_LCD_WIDTH + i] != TEST_SENTINEL) bot++;
    }
    CHK(top == 0, "top bar (rows 0-19) written: %u bytes\n", top);
    CHK(bot == 0, "bottom bar (rows 220-239) written: %u bytes\n", bot);

    /* ...and the 200 rows in between really were all written, so "did not touch
     * the bars" cannot be satisfied by not drawing at all. */
    CHK(px(fb, 0, 0) == G(3) && px(fb, 319, GFX_ROWS - 1) == G(3),
        "active area corners: %d,%d want %d\n",
        px(fb, 0, 0), px(fb, 319, GFX_ROWS - 1), G(3));
}

/* -------- 5. CRTC start address and per-bank wrap -------------------------- */
static void test_start_address(uint8_t *fb)
{
    uint8_t *v = test_vram();
    memset(v, 0, GFX_VRAM_SIZE);

    /* Mark each row-pair with a distinct byte so a shift is measurable. */
    for (unsigned p = 0; p < 100; p++) {
        memset(v + p * GFX_ROW_BYTES, (uint8_t)(0x40 | p), GFX_ROW_BYTES);
        memset(v + GFX_BANK_STRIDE + p * GFX_ROW_BYTES, (uint8_t)(0x80 | p), GFX_ROW_BYTES);
    }

    /* Baseline at start 0: row-pair 0 is 0x40 / 0x80, whose low pixel pair is 0. */
    test_set_start(0);
    blit_cga(fb, 4);
    CHK(px(fb, 3, 0) == G(0) && px(fb, 3, 1) == G(0),
        "start=0 baseline: %d,%d want %d\n", px(fb, 3, 0), px(fb, 3, 1), G(0));

    /* start = 40 characters = 80 bytes = exactly one row-pair, so guest rows 0/1
     * must now show row-pair 1 (0x41 / 0x81), whose low pixel pair is 1. If the
     * *2 character-to-byte scaling were missing the shift would be half a row and
     * nothing would move; if the start were ignored entirely this stays 0. */
    test_set_start(40);
    blit_cga(fb, 4);
    CHK(px(fb, 3, 0) == G(1), "start=40: even row not shifted, got %d\n", px(fb, 3, 0));
    CHK(px(fb, 3, 1) == G(1), "start=40: odd row not shifted, got %d\n", px(fb, 3, 1));

    /* Wrap. Distinct markers at each bank base and at the top of each bank, so
     * head and tail of a straddling row are separately identifiable. */
    memset(v + 0x0000, 0x1B, 40);            /* bank 0 base -> pixels 0,1,2,3 */
    memset(v + 0x2000, 0xE4, 40);            /* bank 1 base -> pixels 3,2,1,0 */
    memset(v + 0x1FD8, 0xFF, 40);            /* top of bank 0 -> all 3        */
    memset(v + 0x3FD8, 0x00, 40);            /* top of bank 1 -> all 0        */

    /* start = 4076 chars = 0x1FD8 bytes. Each row reads 40 bytes to the end of
     * its bank, then the remaining 40 must come from the base of THAT SAME bank.
     * Output byte 40 is x 160..163, so those are the wrapped pixels. */
    test_set_start(0x1FD8 / 2);
    blit_cga(fb, 4);

    CHK(px(fb, 0, 0) == G(3) && px(fb, 0, 1) == G(0),
        "start wrap heads: %d,%d want %d,%d (top of each bank)\n",
        px(fb, 0, 0), px(fb, 0, 1), G(3), G(0));
    CHK(px(fb,160,0)==G(0) && px(fb,161,0)==G(1) && px(fb,162,0)==G(2) && px(fb,163,0)==G(3),
        "even row tail = %d,%d,%d,%d want 0,1,2,3 biased (bank 0 base)\n",
        px(fb,160,0), px(fb,161,0), px(fb,162,0), px(fb,163,0));
    /* This is the check that separates per-bank wrap from a 16 KB wrap: under a
     * 16 KB wrap the odd row's tail would come from byte 0 -- bank 0 -- and read
     * 0,1,2,3 instead of 3,2,1,0. */
    CHK(px(fb,160,1)==G(3) && px(fb,161,1)==G(2) && px(fb,162,1)==G(1) && px(fb,163,1)==G(0),
        "odd row tail = %d,%d,%d,%d want 3,2,1,0 biased (bank 1 base, not bank 0)\n",
        px(fb,160,1), px(fb,161,1), px(fb,162,1), px(fb,163,1));

    test_set_start(0);
}

/* -------- 6. palette: port 0x3D9 -> selected colours -> CLUT 16..19 -------- */
static void test_palette(uint8_t *fb)
{
    static const struct { uint8_t mode, colsel; uint8_t want[4]; const char *name; } cases[] = {
        { 4, 0x00, { 0, 2, 4, 6 },   "mode4 pal0, black bg" },
        { 4, 0x20, { 0, 3, 5, 7 },   "mode4 pal1 (bit5)" },
        { 4, 0x10, { 0, 10, 12, 14 }, "mode4 pal0 + intensity (bit4)" },
        { 4, 0x30, { 0, 11, 13, 15 }, "mode4 pal1 + intensity" },
        { 4, 0x01, { 1, 2, 4, 6 },   "mode4 bg=blue (bits0-3)" },
        { 4, 0x2F, { 15, 3, 5, 7 },  "mode4 bg=white, pal1" },
        { 5, 0x00, { 0, 3, 4, 7 },   "mode5 fixed cyan/red/white" },
        { 5, 0x12, { 2, 11, 12, 15 }, "mode5 bg=green + intensity" },
        { 6, 0x0F, { 0, 15, 15, 15 }, "mode6 fg from bits0-3" },
        { 6, 0x02, { 0, 2, 2, 2 },   "mode6 fg=green" },
    };

    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint8_t got[4];
        cga_select_colors(cases[c].mode, cases[c].colsel, got);
        CHK(got[0] == cases[c].want[0] && got[1] == cases[c].want[1] &&
            got[2] == cases[c].want[2] && got[3] == cases[c].want[3],
            "%s: got %u,%u,%u,%u want %u,%u,%u,%u\n", cases[c].name,
            got[0], got[1], got[2], got[3],
            cases[c].want[0], cases[c].want[1], cases[c].want[2], cases[c].want[3]);
    }

    /* And that the selection actually reaches the hardware CLUT. Slots 16-19 are
     * compared against slots 0-15 rather than against RGB literals, so the check
     * is independent of the firmware's RGB888->RGB565 conversion. */
    io_ports[CGA_COLOR_SEL] = 0x31;                  /* pal1 + intensity + bg=blue */
    last_colsel = 0xFF;                              /* force a reprogram */
    cga_sync_palette(4);

    uint16_t clut[LCD_SCREENSHOT_CLUT_ENTRIES];
    lcd_get_clut_rgb565(clut);
    static const uint8_t want31[4] = { 1, 11, 13, 15 };
    for (unsigned i = 0; i < 4; i++)
        CHK(clut[GFX_CLUT_BASE + i] == clut[want31[i]],
            "CLUT slot %u = %04X, want CGA colour %u (%04X)\n",
            GFX_CLUT_BASE + i, clut[GFX_CLUT_BASE + i], want31[i], clut[want31[i]]);
    /* Slots 0-15 must still be the fixed CGA palette, and 20-31 initialised. */
    CHK(clut[0] != clut[15], "CLUT slots 0-15 not distinct\n");
    CHK(clut[20] == clut[4] && clut[31] == clut[15],
        "CLUT filler slots 20-31 not mirroring colours 4-15\n");

    /* A palette change must be a CLUT reprogram, not a repaint: the framebuffer
     * indices produced by the blit must be identical before and after. */
    uint8_t *v = test_vram();
    test_set_start(0);
    for (unsigned i = 0; i < GFX_VRAM_SIZE; i++) v[i] = (uint8_t)(i * 31u + 7u);

    static uint8_t before[DOS_LCD_WIDTH];
    blit_cga(fb, 4);
    memcpy(before, fb + (DOS_LETTERBOX + 100) * DOS_LCD_WIDTH, DOS_LCD_WIDTH);

    io_ports[CGA_COLOR_SEL] = 0x0E;
    cga_sync_palette(4);
    blit_cga(fb, 4);
    CHK(memcmp(before, fb + (DOS_LETTERBOX + 100) * DOS_LCD_WIDTH, DOS_LCD_WIDTH) == 0,
        "palette change altered framebuffer indices (repaint, not reprogram)\n");
}

/* -------- the visible demo pattern, repainted every frame ------------------ */
#if DOS_VIDEO_TEST > 1
static void test_paint_demo(uint8_t *fb)
{
    uint8_t *v = test_vram();
    test_set_start(0);
    io_ports[CGA_COLOR_SEL] = 0x00;
    cga_sync_palette(4);

    /* Top half: four horizontal bands of colours 0-3, each 25 rows, so the two
     * banks must both be right or the bands come out striped.
     * Bottom half: a 1-pixel checkerboard of colour 3 on colour 0 -- any
     * interleave or pixel-order error turns it into stripes or blank rows. */
    for (unsigned y = 0; y < GFX_ROWS; y++) {
        uint8_t *row = v + (y & 1) * GFX_BANK_STRIDE + (y >> 1) * GFX_ROW_BYTES;
        if (y < 100) {
            const unsigned band = y / 25;                    /* 0..3 */
            memset(row, (uint8_t)(band * 0x55), GFX_ROW_BYTES);
        } else {
            /* 11 00 11 00 / 00 11 00 11 on alternate rows */
            memset(row, (y & 1) ? 0x33 : 0xCC, GFX_ROW_BYTES);
        }
    }
    blit_cga(fb, 4);
}
#endif /* DOS_VIDEO_TEST > 1 */

static bool test_done = false;

/* Returns true if the caller must not render the guest this frame. */
static bool dos_video_selftest(uint8_t *fb)
{
    if (!test_done) {
        test_done = true;
        printf("DOSVT: synthesised CGA self-test (DOS_VIDEO_TEST=1)\n");
        printf("DOSVT: aperture guest %05X -> host %p, CLUT=%u\n",
               VRAM_BASE, (void *)test_vram(), DOS_CLUT_COUNT);

        test_tables();
        test_interleave(fb);
        test_mode6(fb);
        test_letterbox(fb);
        test_start_address(fb);
        test_palette(fb);

        printf("DOSVT: %u checks, %u failures -- %s\n",
               test_checks, test_fails, test_fails ? "FAIL" : "PASS");

        /* Hand the guest back a clean slate. The checks scribbled test patterns
         * over the aperture -- which at level 1 the guest is about to render as
         * its own screen -- and test_letterbox() left a sentinel in the bars.
         * Zeroing the aperture matters because the BIOS blanks text with char 0,
         * so a zeroed buffer renders blank rather than as noise until DOS
         * scrolls it away. */
        memset(test_vram(), 0, GFX_VRAM_SIZE);
        io_ports[CGA_COLOR_SEL] = 0;
        last_colsel = 0xFF;
        test_set_start(0);
        clear_framebuffers();
    }

#if DOS_VIDEO_TEST > 1
    test_paint_demo(fb);
    return true;                 /* level 2 owns the screen */
#else
    (void)fb;
    return false;                /* level 1 hands the screen back to the guest */
#endif
}
#endif /* DOS_VIDEO_TEST */

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */
void dos_video_blit(void)
{
    if (!initialised)
        dos_video_init();

    /* The CLUT and framebuffer layout only exist in LUT8 mode. */
    if (lcd_get_mode() != LCD_MODE_LUT8)
        return;

#if DOS_VIDEO_TEST
    /* Runs before the mode dispatch so it does not depend on the guest ever
     * reaching a graphics mode. At level 1 it returns false after the one-shot
     * checks and normal rendering continues below; only level 2 keeps the
     * screen. */
    if (dos_video_selftest((uint8_t *)lcd_get_active_buffer()))
        return;
#endif

    uint8_t mode  = bda8(BDA_VIDMODE);
    uint16_t cols = bda16(BDA_VID_COLS);

    /* Mode comes from BDA 0x449 and nowhere else.
     *
     * There is an obvious-looking fallback here for software that programs the
     * adapter directly and never calls INT 10h: read the CGA mode-control
     * register (port 0x3D8) and treat bit 1 as "graphics", bit 4 as 640x200.
     * DO NOT. It was tried and it breaks text mode: something in the FreeDOS
     * boot path leaves bit 1 set at the DOS prompt (it is not the BIOS -- 0x3D8
     * appears nowhere in bios.asm; the BIOS drives the Hercules register at
     * 0x3B8 instead, bios.asm:1056/1109), so the heuristic latches into graphics
     * mode and decodes text memory as 2 bpp. The symptom is a screen of 2-pixel
     * dashes filling exactly the top half -- 4000 bytes of text at 4 pixels per
     * byte is 50 rows, landing on even scanlines only because of the bank
     * interleave, with the odd bank still zero.
     *
     * If a real game turns out to need this, it needs a proper CRTC/mode-register
     * model, not a bit test on a stale port latch. */

    /* 80 selects the narrow font, 40 the wide one; anything else is a bug in
     * the guest or a mode we do not model. Clamp rather than scribble. */
    if (cols != 80 && cols != 40)
        cols = 80;

    if (mode != last_mode || cols != last_cols) {
        /* A geometry change leaves stale pixels from the previous mode, and a
         * 200-row mode does not overwrite bars a previous mode may have used. */
        clear_framebuffers();
        /* Leaving mode 13h hands the CLUT back to the cached 32-entry path;
         * entering it must re-push, because lcd_set_clut() cleared ext_clut. */
        vga13_clut_valid = false;
        if (mode != 0x13) {
            lcd_set_clut_ext(NULL, 0);
            last_colsel = 0xFF;          /* force program_clut() on the way back */
        }
        last_mode = mode;
        last_cols = cols;
    }

    uint8_t *fb = (uint8_t *)lcd_get_active_buffer();

    /* Modes 0/1 are 40-column text, 2/3 are 80-column; the BIOS folds mono modes
     * 2 and 7 to 3 (bios.asm:1080-1083), so mode 7 never appears. 4/5 are CGA
     * 320x200 and 6 is CGA 640x200.
     *
     * Mode 0x13 is VGA 320x200x256 linear. Anything else -- Hercules, EGA,
     * Mode X/Y -- is unsupported: leave the screen alone rather than render one
     * mode's memory through another mode's decoder, which produces
     * confident-looking garbage. Note that the BIOS
     * programs the *Hercules* CRTC to 640x400 even for CGA modes
     * (bios.asm:1086-1112); that is upstream's pixel-doubling for its SDL path
     * and is deliberately ignored -- we read the real 320x200 buffer. */
    if (mode <= 3) {
        blit_text(fb, cols);
    } else if (mode <= 6) {
        cga_sync_palette(mode);
        blit_cga(fb, mode);
    } else if (mode == 0x13) {
        vga13_sync_palette();
        blit_vga13(fb);
    }
}
