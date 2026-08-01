#include "odroid_display.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "odroid_system.h"
#include "odroid_sdcard.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "main.h"        /* wdog_refresh() */
#include "common.h"
#include "appid.h"
#include "rg_i18n.h"     /* ODROID_DIALOG_CHOICE_SEPARATOR */
#include "odroid_settings.h" /* odroid_settings_cpu_oc_level_get() */
#include "dos_video.h"
#include "dos_input.h"
#include "dos_osk.h"
#include "dos_cpu.h"
#include "dos_audio.h"
#include "dos_ospi_bench.h"
#include "gw_malloc.h"   /* ahb_calloc() */
#include "gw_flash_alloc.h" /* store_file_in_flash() -- DOS_XIP_CACHE */
#include <string.h>

/* Guest memory is a BSS array inside this overlay (.overlay_dos_bss), zeroed by
 * the launcher before we are entered. Not a pointer, and nothing to allocate. */
extern unsigned char mem[];
/* Guest RAM footprint, owned by 8086tiny.c so the two cannot drift. */
extern const unsigned int dos_mem_required;
/* The guest's other physical region. The fold returns a POINTER, not an index,
 * so the guest's 1 MB no longer has to come out of one allocation: the cold
 * windows (colour text at 0xB8000, the two BIOS shadows, the scratch page) live
 * in AHB SRAM instead, which nothing else uses while a DOS core is resident.
 * That is 44 KB of AXI returned to .overlay_dos_bss. See dos_fold() in
 * 8086tiny.c for what moved and, more importantly, what must not. */
extern const unsigned int dos_mem_ahb_required;
extern void dos_mem_set_ahb(unsigned char *p);
extern const unsigned int dos_mem_hma_required;
extern void dos_mem_set_hma(unsigned char *p);
extern unsigned short *regs16;
extern unsigned char *regs8;
extern unsigned char io_ports[];

extern int dos_cpu_init(int argc, char **argv);   /* 0 = ok, <0 = BIOS load failed */
extern int dos_cpu_frame(int cycles);

/* ---- The AXI SRAM the mapping fold actually gives back ---------------------
 *
 * DOS_MEM_TRIM shortens mem[] by removing guest [dos_mem_trim_base, 0x100000).
 * Those bytes then have NO SRAM behind them and must be served from somewhere
 * read-only, or dos_cpu_init() refuses to run rather than execute a BIOS that
 * is half scratch page. Copy-on-write is what makes serving them from
 * read-only storage safe: a store into the region costs a 4 KB pool page and a
 * memcpy instead of being silently lost.
 *
 * Both constants come from 8086tiny.c so the porting layer cannot drift from
 * the fold. When the core is built without a trim they are 0 and every branch
 * below folds away.
 *
 * dos_mem_map_ro() is the read-only form of dos_mem_map(): the granules it
 * covers get the real pointer in the READ map and DOS_COW_TRAP in the WRITE
 * map. dos_cow_stats() is what makes an undersized pool visible -- `lost` is
 * the count of stores that had nowhere to go, and it must be 0. */
extern const unsigned int dos_mem_trim_base;
extern const unsigned int dos_mem_trim;
extern int dos_mem_map_ro(unsigned int gbase, unsigned int len, unsigned char *host);
extern void dos_cow_stats(unsigned *faults, unsigned *pages_used,
                          unsigned *pool_pages, unsigned *lost);
#if DOS_INT13_OBS
/* 8086tiny.c's INT 13h observer, built only under -DDOS_INT13_OBS=1. */
extern void dos_int13_obs_report(void);
#endif

/* Monotonic millisecond source for the guest's BIOS timer tick, called from
 * 8086tiny.c's GET_RTC hook (which must stay free of STM32 HAL headers).
 *
 * It has to be SysTick and not the RTC: the BIOS advances guest time by
 * differencing this across timer interrupts, and the RTC's sub-second field
 * (GW_GetCurrentMillis() -> rg_rtc.c:251, via _gettimeofday()) is not modelled
 * by gwemu -- it reads back constant, which made every delta zero and hung any
 * guest that waits on elapsed time. HAL_GetTick() is a real 1 kHz counter both
 * on hardware and under emulation. Wrap-safe: the caller subtracts in uint32. */
unsigned int dos_host_millis(void)
{
    return (unsigned int)HAL_GetTick();
}

/* ------------------------------------------------------------ guest XIP ---
 * Phase 3 of external/8086tiny/docs/memory/02-guest-xip.md: cache the whole
 * .dsk into the 64 MB of OSPI NOR that an SD_CARD=1 build leaves entirely
 * unused at runtime (docs/gwemu.md:194), and hand back the memory-mapped
 * pointer. NOTHING READS IT YET -- the fold's third case is phase 5. This is
 * here so the cost, the failure modes and the address are all measured before
 * anything depends on them.
 *
 * OFF BY DEFAULT, and that is the phase's own rule rather than caution: phase 3
 * is specified as a pure observer, and a multi-second "first boot of this
 * image" stall in exchange for a pointer nobody dereferences is a behaviour
 * change. Enable with DOS_CFLAGS_EXTRA=-DDOS_XIP_CACHE=1 (which is NOT a make
 * dependency -- touch the file, see docs/testing.md section 4).
 *
 * store_file_in_flash() and not odroid_overlay_cache_file_in_flash(): the
 * latter draws the "Caching game" progress bar, and by the time app_main_dos()
 * runs the LCD is already in LUT8 with the DOS palette loaded (see the note
 * above the guest-RAM printf below), so that bar would paint in the wrong
 * colours into a framebuffer this core is about to own. A progress callback
 * that prints instead costs nothing and works under gwemu, where the log is
 * the only output that matters.
 *
 * The relocation hook is deliberately NULL. A .dsk is data, contains no
 * absolute host addresses, and phase 5's window is pure arithmetic
 * (dos_xip + (a - XIP_BASE)) with no alignment requirement -- see
 * 02-guest-xip.md section 4.2. Nothing needs patching on the way in.
 */
#ifndef DOS_XIP_CACHE
#define DOS_XIP_CACHE 0
#endif

#if DOS_XIP_CACHE
static uint8_t *dos_xip_base;
static uint32_t dos_xip_size;

static void dos_xip_progress(uint32_t total, uint32_t done, uint8_t pct)
{
    static uint8_t last = 255;
    (void)total;
    (void)done;
    /* Quarters only. The log ring is 4 KB (Core/Src/main.c:94) and this runs
     * once per 16 KB of image. */
    if (pct / 25 != last / 25 || last == 255) {
        last = pct;
        printf("DOS: xip caching %u%%\n", pct);
    }
    wdog_refresh();
}

static void dos_xip_cache(const char *path, uint32_t known_size)
{
    uint32_t t0 = HAL_GetTick();

    dos_xip_size = 0;   /* 0 = "whole file"; the cache fills it in */
    dos_xip_base = store_file_in_flash(path, &dos_xip_size, false, &dos_xip_progress);

    if (dos_xip_base == NULL) {
        /* Expected, not exceptional. BATTLECHESS.dsk is 66,060,288 bytes
         * against a 64 MB part, and find_write_slot() (gw_flash_alloc.c:144)
         * answers false for anything that cannot fit clear of the files
         * already live this boot. A full or failing cache must still boot the
         * guest -- 02-guest-xip.md section 5, "the one new obligation" -- and it
         * does, because nothing downstream of here consults the pointer. */
        printf("DOS: xip NOT cached (%s, %lu bytes) - guest runs from SD as before\n",
               path, (unsigned long)known_size);
        return;
    }
    printf("DOS: xip .dsk at %p, %lu bytes, %lu ms\n", (void *)dos_xip_base,
           (unsigned long)dos_xip_size, (unsigned long)(HAL_GetTick() - t0));
}
#endif

/* Arm the read-only window over the trimmed BIOS tail. Returns 0 on success,
 * -1 if there is nothing to map it from -- in which case the caller must NOT
 * start the guest, because dos_cpu_init() would be reading its instruction
 * decode tables out of the scratch page.
 *
 * WHERE THE BYTES COME FROM, and why this is not store_file_in_flash() on the
 * BIOS. The BIOS image is 8,096 bytes and loads at guest 0xF0100, so it ends at
 * 0xF203F; the trim starts at 0xF3000 at the earliest. EVERY BYTE OF THE
 * TRIMMED TAIL IS A ZERO that a real machine's power-on left there, and stays
 * zero for the whole session (docs/memory/05-copy-on-write.md section 7).
 * So what is needed is not the BIOS file -- it is any dos_mem_trim bytes of
 * memory-mapped, readable, permanently-zero storage.
 *
 * The cached .dsk is that, and it is already in flash and already held live by
 * gw_flash_alloc's live-range set (invariant I2 in dos_xipimg.h). A FAT floppy
 * image is mostly unallocated clusters, which are zeros. So: scan the cached
 * image for a run of dos_mem_trim zero bytes and map that. If there is no such
 * run -- or no cache at all -- say so and refuse, rather than mapping something
 * that merely looks blank.
 *
 * THE SCAN IS THE PROOF, not a heuristic: it reads every byte it is about to
 * promise is zero. It costs one linear pass over flash once per session. */
#if DOS_XIP_CACHE
static int dos_trim_arm(void)
{
    uint32_t run = 0, i;

    if (dos_mem_trim == 0)
        return 0;                       /* nothing trimmed, nothing to serve */
    if (dos_xip_base == NULL || dos_xip_size < dos_mem_trim) {
        printf("DOS: TRIM %u B but no flash image to serve it from\n",
               (unsigned)dos_mem_trim);
        return -1;
    }
    for (i = 0; i < dos_xip_size; i++) {
        if (dos_xip_base[i] != 0) { run = 0; continue; }
        if (++run < dos_mem_trim)
            continue;
        {
            uint8_t *p = dos_xip_base + (i + 1 - dos_mem_trim);
            /* dos_mem_map() rejects an unaligned host pointer -- the fold's
             * DOS_COW_TRAP sentinel is 1 and relies on every real entry being
             * 0 mod 4. Walk the run forward to the first 4-aligned start. */
            uint8_t *a = (uint8_t *)(((uintptr_t)p + 3u) & ~(uintptr_t)3);
            if ((uint32_t)(a - p) + dos_mem_trim > run)
                continue;               /* alignment ate the run; keep looking */
            if (dos_mem_map_ro(dos_mem_trim_base, dos_mem_trim, a) != 0) {
                printf("DOS: TRIM window did not take\n");
                return -1;
            }
            printf("DOS: TRIM mem[] is %u B shorter; guest %05X+%X from flash %p\n",
                   (unsigned)dos_mem_trim, (unsigned)dos_mem_trim_base,
                   (unsigned)dos_mem_trim, (void *)a);
            return 0;
        }
    }
    printf("DOS: TRIM no %u B zero run in the cached image\n",
           (unsigned)dos_mem_trim);
    return -1;
}
#else
static int dos_trim_arm(void)
{
    if (dos_mem_trim == 0)
        return 0;
    printf("DOS: TRIM needs DOS_XIP_CACHE=1 to have flash to map\n");
    return -1;
}
#endif

/* One line per session. `lost` is the only number that can be wrong silently:
 * it counts stores that reached a granule with no writable page behind it and
 * were routed to the scratch page. A nonzero value means the pool is too small
 * for what is mapped, and the fix is DOS_COW_POOL_PAGES -- subtracting 4,104
 * bytes per page from whatever the trim reclaimed. */
static void dos_cow_log(const char *when)
{
    unsigned faults = 0, pages = 0, pool = 0, lost = 0;

    dos_cow_stats(&faults, &pages, &pool, &lost);
    if (pool == 0 && faults == 0)
        return;                         /* feature not built in; say nothing */
    printf("DOS: cow %s faults=%u pages=%u/%u lost=%u\n",
           when, faults, pages, pool, lost);
}

static void dos_blit(void) {
    dos_video_blit();
}

/* ---- Diagnostics -----------------------------------------------------------
 *
 * DOS_DEBUG_STATUS controls per-frame instrumentation. It is deliberately not a
 * throwaway: "the guest is not writing video memory" and "the renderer is not
 * reading it" look identical on a garbled screen, and the only way to tell them
 * apart is to read the guest's text buffer directly, out of band from the
 * renderer.
 *
 *   0 (default) nothing
 *   1           a status line once a second: frame, instruction count, CS:IP,
 *               BDA video mode and column count
 *   2           the above plus an ASCII dump of the 80x25 CGA text buffer at
 *               guest 0xB8000, non-blank rows only
 *
 * Set it on the make command line, e.g. DOS_CFLAGS_EXTRA=-DDOS_DEBUG_STATUS=2.
 */
#ifndef DOS_DEBUG_STATUS
#define DOS_DEBUG_STATUS 0
#endif

/* Folded index of guest 0xB8000 -- the CGA text buffer, i.e. the real screen.
 * The video aperture folds guest 0xB0000 to 0xA0000 (8086tiny.c dos_fold()),
 * so 0xB8000 lands at 0xA8000. */
#define DOS_VID_TEXT 0xA8000

#if DOS_DEBUG_STATUS > 1
static void dos_debug_dump_text(void) {
    char line[81];
    for (int row = 0; row < 25; row++) {
        const unsigned char *cell = &mem[DOS_VID_TEXT + row * 160];
        int last = -1;
        for (int col = 0; col < 80; col++) {
            unsigned char ch = cell[col * 2];
            /* The BIOS blanks video memory with char 0, not 0x20
             * (bios.asm:266, :3096), so treat both as space. */
            if (ch == 0 || ch == 0x20) { line[col] = ' '; continue; }
            line[col] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
            last = col;
        }
        if (last < 0) continue;                 /* blank row, skip */
        line[last + 1] = '\0';
        printf("DOS: |%s\n", line);
    }
}
#elif DOS_DEBUG_STATUS > 0
static void dos_debug_dump_text(void) { }
#endif

/* void, matching every other app_main_* in the tree. The dispatch chain has
 * nowhere to consume a status code -- run_internal_emu() already establishes
 * "on failure, fall through and return to the launcher" -- and every early
 * return below logs its own reason first. */
void app_main_dos(uint8_t load_state, uint8_t start_paused, int8_t save_slot) {
    printf("Initializing 8086tiny...\n");

    /* Raise the core clock, like every other core in this tree does on entry
     * (main_nes_fceu.c:893, main_msx.c:2045, main_amstrad.c:1061,
     * main_gwenesis.c:627). DOS was the only core still running at the stock
     * 280 MHz. Level 2 is 340 MHz core / 97 MHz OSPI (Core/Src/main.c:446-460);
     * level 3 (~353 MHz) is deliberately NOT used -- "maximum speed could cause
     * random crash so it should not be used" (main_gwenesis.c:626), and DOS is
     * the one core doing live SD reads/writes of the .dsk image during
     * emulation (8086tiny.c OPCODE 48 DISK_READ/DISK_WRITE), so the OSPI/SD
     * path is under more sustained stress here than elsewhere.
     *
     * Guarded on oc_level == 0 so a user who picked an overclock in the
     * launcher menu keeps it -- we only lift the default, never lower a choice.
     *
     * MUST come before dos_screen_apply_rate(): SystemClock_Config()
     * reprograms PLL3 back to its boot value (60 Hz, main.c:536-543) as part of
     * the peripheral clock setup, which would silently undo
     * lcd_set_refresh_rate(). It also updates SystemCoreClock, which the MAX
     * profile's deadline reads live (dos_cpu.c:389). It does NOT change the
     * emulated CPU speed: the fixed profiles are instructions/second and the
     * per-frame budget is derived from the refresh rate, not from the host
     * clock -- a faster host shows up as lower cpu%, not as a faster guest. */
    if (odroid_settings_cpu_oc_level_get() == 0) {
        SystemClock_Config(2);
    }

    odroid_system_init(APPID_DOS, AUDIO_SAMPLE_RATE);

    /* Measurement build only (-DDOS_OSPI_BENCH=1); compiles to nothing when
     * off. Runs here, AFTER SystemClock_Config() so the OSPI clock is the one
     * a real DOS session uses, and BEFORE any guest activity so nothing else
     * is contending for the bus or the D-cache. See dos_ospi_bench.c. */
    dos_ospi_bench();

    /* NOTE: the LCD is already in LUT8 mode and mem[] is already zeroed by the
     * time we get here -- the launcher must switch modes before copying this
     * overlay into the bonus area, and it zeroes .overlay_dos_bss (which mem[]
     * lives in) as part of the standard overlay load. Do NOT call
     * lcd_setup_framebuffers() here: it would re-zero the 150 KB framebuffer
     * footprint and rewrite MPU regions 3-6 underneath our own code. */
    printf("DOS: guest RAM %u KB at %p, ends %p (limit %p)\n",
           (unsigned)(dos_mem_required / 1024), (void *)mem,
           (void *)(mem + dos_mem_required), (void *)&__RAM_EMU_END__);

    /* ahb_ONLY_malloc, and that is not a stylistic choice -- ahb_malloc() and
     * ahb_calloc() both try ram_malloc() FIRST (gw_malloc.c:65,83), which hands
     * out AXI RAM_EMU starting at the global `ram_start`. ahb_init() resets
     * current_ram_pointer but NOT ram_start, and ram_start is intflash BSS that
     * whichever core ran last leaves set (main_smsplusgx.c:92, main_smw.c:313,
     * ...). So ahb_calloc() here would return AXI memory at the *previous*
     * core's BSS end -- straight through the middle of this overlay -- on any
     * session where a game was played before DOS, and not on a cold boot. That
     * is a session-dependent silent corruption of guest memory, so take the AHB
     * pool explicitly.
     *
     * Zeroed by hand for the same reason ahb_calloc would have: unlike mem[]
     * this block is NOT inside .overlay_dos_bss, so the launcher's overlay
     * memset does not reach it and a dirty colour-text window would show the
     * previous core's garbage.
     *
     * Must be handed over BEFORE dos_cpu_init(), which runs BIOS setup that
     * already touches the shadows. No failure path: ahb_only_malloc() asserts
     * if the pool is short, the same contract every other AHB user here has. */
    {
        unsigned char *ahb = (unsigned char *)ahb_only_malloc(dos_mem_ahb_required);
        memset(ahb, 0, dos_mem_ahb_required);
        printf("DOS: guest AHB %u KB at %p\n",
               (unsigned)(dos_mem_ahb_required / 1024), (void *)ahb);
        dos_mem_set_ahb(ahb);
    }

    /* The HMA: 64 KB more of the same pool, and the reason DOS=HIGH works.
     * Same ahb_only_malloc reasoning as the block above -- do not "simplify"
     * this to ahb_calloc().
     *
     * It is a SEPARATE allocation rather than an extension of the AHB block
     * because it is optional: dos_fold() wraps at 1 MB like an 8086 when it is
     * absent and XMS function 01h answers "HMA does not exist", which is a
     * machine that shipped. Merging them would turn a tight pool into a dead
     * core.
     *
     * Budget: the AHB heap is 128 KB less the 8 KB .audio DMA reserve =
     * 122,880 B, and .gba_ahbram is overlaid at the bottom rather than charged
     * to the region (STM32H7B0VBTx_SDCARD.ld), so it is all ours while DOS is
     * resident. 45,060 (above) + 65,536 (here) = 110,596, leaving ~12 KB.
     * ahb_only_malloc() asserts rather than returning NULL if that ever stops
     * being true, so a regression here is loud. */
    {
        unsigned char *hma = (unsigned char *)ahb_only_malloc(dos_mem_hma_required);
        memset(hma, 0, dos_mem_hma_required);
        printf("DOS: guest HMA %u KB at %p\n",
               (unsigned)(dos_mem_hma_required / 1024), (void *)hma);
        dos_mem_set_hma(hma);
    }

    /* Literal path, matching every other core in this tree (/bios/nes/palettes.bin,
     * /bios/msx/msxromdb.bin, /bios/mini/bios.min, ...). Do NOT use
     * RG_BASE_PATH_BIOS: it resolves to "/retro-go/bios", which is upstream
     * retro-go's layout, not this fork's -- and nothing ever writes there. The
     * build delivers the BIOS to /bios/dos/bios.bin (Makefile.common). */
    static const char dos_bios_path[] = "/bios/dos/bios.bin";

    char *argv[5];
    int argc = 1;
    argv[0] = "8086tiny";
    argv[1] = (char *)dos_bios_path;
    argc = 2;

    // Check if the selected image is small enough to be a floppy (< 2.88MB)
    rg_stat_t st = rg_storage_stat(ACTIVE_FILE->path);
    if (st.size <= 2880 * 1024) {
        argv[2] = (char *)ACTIVE_FILE->path; // Floppy disk
        argv[3] = NULL;
        argc = 3;
    } else {
        argv[2] = NULL; // No floppy
        argv[3] = (char *)ACTIVE_FILE->path; // Hard disk
        argv[4] = NULL;
        argc = 4;
    }
    
#if DOS_XIP_CACHE
    /* Before dos_cpu_init(), for one reason worth stating: the cache opens the
     * .dsk itself, and dos_cpu_init() then holds three FILE* for the whole run.
     * MAX_OPEN_FILES is 8 (Core/Src/syscalls.c:51) and running out fails
     * SILENTLY -- see the fopen entry in ../../../../CLAUDE.md. Doing the cache
     * first keeps its handle transient and the count at three afterwards. */
    dos_xip_cache(ACTIVE_FILE->path, (uint32_t)st.size);
#endif

    /* The BIOS is mandatory: 8086tiny reads its instruction-decode tables out of
     * the BIOS image, so a missing file yields garbage decoding that looks exactly
     * like a broken memory map. Fail loudly here instead of burning debug time. */
    /* BEFORE dos_cpu_init(), and after the .dsk cache: init reads the BIOS
     * decode tables through the fold, so the window over the trimmed tail has
     * to already be there. */
    if (dos_trim_arm() != 0) {
        printf("DOS: refusing to start with a trimmed mem[] and no window\n");
        return;
    }

    int init_rc = dos_cpu_init(argc, argv);
    if (init_rc != 0) {
        printf("DOS: BIOS load failed (%d) - expected %s\n", init_rc, dos_bios_path);
        return;
    }
    
    if (start_paused) {
        common_emu_state.pause_after_frames = 4;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

    /* Restore the persisted rate before applying it, so the core comes up at the
     * user's frequency rather than switching a frame in. */
    dos_screen_freq_init();
    dos_screen_apply_rate();

    /* After dos_screen_apply_rate(): that is what calls audio_start_playing()
     * and so decides the DMA buffer length we will be filling. */
    dos_audio_init();

    extern unsigned int inst_counter;
    extern unsigned short reg_ip;
    #define DBG_REG_CS 9            /* REG_CS is private to 8086tiny.c */
    unsigned int dbg_frames = 0;

    /* Guest CPU speed. Selectable at runtime because most DOS games have no
     * frame limiter and run at whatever speed the CPU provides -- the turbo
     * button problem. The table and the MAX-mode deadline loop live in
     * dos_cpu.c; this file only owns the menu row. */
    char dos_cpu_speed_value[DOS_CPU_VALUE_LEN];
    char dos_screen_freq_value[DOS_SCREEN_FREQ_VALUE_LEN];
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_SEPARATOR,
        {200, "CPU speed",      dos_cpu_speed_value,    1, &dos_cpu_speed_update_cb},
        {201, "Screen Freq Hz", dos_screen_freq_value,  1, &dos_screen_freq_update_cb},
        ODROID_DIALOG_CHOICE_LAST};
    /* Populate the value strings before the menu can be opened. */
    dos_cpu_speed_update_cb(&options[1], ODROID_DIALOG_INIT, 0);
    dos_screen_freq_update_cb(&options[2], ODROID_DIALOG_INIT, 0);

    dos_cpu_speed_init();

    /* Baseline for the input edge detector. Without this, any button already
     * held when the core starts (typically A, which launched the ROM) would look
     * like a fresh press on frame 1 and inject a stray Enter into the guest. */
    dos_input_reset();

    while (1) {
        /* Feed the window watchdog. NOT optional and NOT cosmetic: WWDG1 is
         * enabled unconditionally in main() (MX_WWDG1_Init), wdog_refresh() is
         * the only thing that services it, and its window is a few hundred
         * milliseconds. A frame loop that never refreshes resets the chip after
         * ~0.3 s no matter how fast the frames are.
         *
         * This is why DOS BSOD'd on real hardware with PC=0/LR=0 shortly after
         * "Starting MS-DOS..." while running fine under emulation: gwemu does
         * not model WWDG1, so the counter never expires there. Every other core
         * does this (main_tama.c:462, main_pce.c:1041, main_gba.c:891, ...) --
         * DOS was the only frame loop missing it. */
        wdog_refresh();

        odroid_gamepad_state_t joystick;
        odroid_input_read_gamepad(&joystick);

        common_emu_input_loop(&joystick, options, &dos_blit);

        /* Once per frame, after the launcher has had its look at the state:
         * common_emu_input_loop() owns PAUSE as a macro prefix (common.c:236),
         * and it may block here for the duration of the pause menu. Reading
         * button edges afterwards means keys are not injected from a menu the
         * guest cannot see. */
        dos_input_update(&joystick);

        bool drawFrame = common_emu_frame_loop();

        /* dos_cpu_run_frame() executes one frame's worth of guest instructions
         * under the selected speed profile (dos_cpu.c) and forwards
         * dos_cpu_frame()'s contract: 1 when the budget was spent and the guest
         * is still running, 0 when the fetch loop exited because CS:IP folded to
         * 0 -- i.e. the guest is done. Test for zero: the inverted form breaks
         * out of the loop on the first *healthy* frame. */
        if (!dos_cpu_run_frame()) {
            printf("DOS: emulation finished at CS:IP=%04X:%04X after %u frames\n",
                   regs16[DBG_REG_CS], reg_ip, dbg_frames);
            break;
        }

        ++dbg_frames;
        /* Once, a couple of seconds in: by then the guest has booted and the
         * copy-on-write pool has taken whatever the boot was going to take, so
         * this is the reading that sizes it. Silent when no window is armed. */
        if (dbg_frames == 128)
            dos_cow_log("boot");
#if DOS_DEBUG_STATUS > 0
        if ((dbg_frames & 0x3F) == 0) {     /* every 64 frames: ~1.07s at 60Hz */
            /* BDA clk_dtimer (guest 0x40:0x6C = 0x46C) is the 32-bit BIOS
             * tick counter. It must advance at ~18.2 Hz; printing it next to
             * HAL_GetTick() makes the rate directly measurable from the log
             * (tick delta / ms delta * 1000), which is how the frozen-timer bug
             * was found and how the fix was verified. */
            printf("DOS: f=%u inst=%u CS:IP=%04X:%04X mode=%02X cols=%u tick=%lu ms=%lu\n",
                   dbg_frames, inst_counter,
                   regs16[DBG_REG_CS], reg_ip,
                   mem[0x449], (unsigned)*(unsigned short *)&mem[0x44A],
                   (unsigned long)*(unsigned int *)&mem[0x46C],
                   (unsigned long)HAL_GetTick());
            dos_debug_dump_text();
        }
#endif

        /* The measurement this whole speed-profile exercise exists to produce:
         * instructions actually achieved per frame and per second, the ARM
         * cycles that cost per guest instruction, and where the rest of the
         * frame went. MAX mode is worthless without the first, "how far off a
         * decent interpreter are we" is unanswerable without cpi, and "attack
         * the interpreter or the renderer" is unanswerable without the split.
         *
         * Deliberately NOT behind DOS_DEBUG_STATUS. It is one printf per second
         * against counters that are three loads per frame, and
         * docs/video/09-video-profiling.md makes the case directly: "a profiling
         * build that has to be specially produced is a profiling build nobody
         * runs -- particularly awkward here because the interesting cases (a
         * specific game, a specific mode) are exactly the ones that are
         * inconvenient to reproduce." It also means measuring MAX needs no debug
         * build, so there is no debug flag to accidentally leave switched on. */
        /* Every 64 frames. That is ~1.07 s at 60 Hz, ~1.28 s at 50 and ~0.85 s
         * at 75 -- the window length is measured (prof_win_ms), not assumed, so
         * only the reporting cadence moves with the refresh rate. */
        if ((dbg_frames & 0x3F) == 0) {
            dos_prof_sample_t s;
            if (dos_prof_take_sample(&s)) {
                printf("DOS: prof %s @%luMHz ipf=%lu ips=%lu cpi=%lu | cpu=%u%% (%luus/f) "
                       "blit=%u%% (%luus x%lu) idle=%u%% other=%u%% "
                       "putc=%lu tick=%lu/%lu rs=%lu frames=%lu/%lums\n",
                       dos_cpu_profile_name(), (unsigned long)s.mhz,
                       (unsigned long)s.insn_per_frame, (unsigned long)s.insn_per_sec,
                       (unsigned long)s.cyc_per_insn,
                       s.cpu_pct, (unsigned long)s.cpu_us_frame,
                       s.blit_pct, (unsigned long)s.blit_us, (unsigned long)s.blits,
                       s.idle_pct, s.other_pct, (unsigned long)s.putchars,
                       (unsigned long)s.int8_fired, (unsigned long)s.int8_due,
                       (unsigned long)s.int8_resync,
                       (unsigned long)s.frames, (unsigned long)s.ms);
            }
#if DOS_INT13_OBS
            /* Retire the open INT 13h run and print the running totals, on the
             * same cadence. Without this the last run of a boot never prints:
             * the observer only retires a run when a *later* transfer breaks
             * it, and the interesting one -- the program image -- is usually
             * the last thing read. */
            dos_int13_obs_report();
#endif
        }

        if (!lcd_is_swap_pending() && drawFrame) {
            /* Bracketed for the cpu/blit/idle split (docs/video/09-video-profiling.md).
             * Note this only runs when common_emu_frame_loop() said so: a dropped
             * frame skips the blit entirely, which preserves guest speed at the
             * cost of visual smoothness -- so blit cost per *frame* and blit cost
             * per *blit* are different numbers, and both are reported. */
            dos_prof_blit_begin();
            dos_blit();
            /* After the guest blit, before the swap. The OSK owns rows 0-19 and
             * 220-239, which dos_blit() never touches, so ordering only matters
             * for the mode-change case where video clears the whole buffer --
             * drawing second means the bars survive that. It is a no-op unless
             * OSK state changed, and a state change paints on two consecutive
             * painted frames so both framebuffers get it (dos_osk.c). */
            dos_osk_draw((uint8_t *)lcd_get_active_buffer());
            lcd_swap();
            dos_prof_blit_end();
        }

        /* NOTE: audio submission is NOT here, and NOT under `if (drawFrame)`.
         * It is fused with the sound-sync loop below -- see the comment there. */

        /* The only place this loop deliberately waits. If idle_pct comes back at
         * ~0 there is no headroom left and the frame is CPU/blit bound.
         *
         * Once per panel refresh, so dos_screen_sync_edges() times per guest
         * frame: 1 in the full-rate modes, 2 in "25 (50)" and "30 (60)". Each
         * common_emu_sound_sync() consumes exactly one SAI DMA half-buffer edge
         * (common.c:495-511), and the buffer is sized for the PANEL rate, so
         * half rate is paid for in edges rather than in a longer buffer.
         * Lengthening the buffer instead would need 48000/25 = 1920 samples
         * against AUDIO_BUFFER_LENGTH == 1077 -- a DMA write off the end of
         * .audio. See DOS_AUDIO_LEN in dos_cpu.c. */
        /* One dos_audio_submit() per DMA half-buffer edge, immediately before
         * the wait for that edge. Three things force this shape:
         *
         * 1. The buffer accessors are edge-relative. audio_get_active_buffer()
         *    and audio_get_buffer_length() both key off dma_state
         *    (gw_audio.c:31-46), which flips on each half-complete interrupt.
         *    Filling once per guest frame in a half-rate mode -- where
         *    dos_screen_sync_edges() is 2 -- would write one half twice and
         *    leave the other replaying stale content.
         * 2. Audio is the frame clock, not a passenger. common_emu_sound_sync()
         *    blocks on dma_counter, so the ring must never be left unfilled: a
         *    skipped fill is an underrun, which is an audible click, and the
         *    DOS core drops frames routinely under the CPU profiles.
         * 3. The spkr_en latch is sampled and cleared inside submit, so it must
         *    run exactly as often as buffers are consumed -- no more, no less.
         *
         * The submit is outside the profiler's idle bracket on purpose: it is
         * work, not waiting, and folding it into idle_pct would understate how
         * much headroom the frame actually has. */
        for (uint8_t e = dos_screen_sync_edges(); e; e--) {
            dos_audio_submit();
            dos_prof_idle_begin();
            common_emu_sound_sync(false);
            dos_prof_idle_end();
        }
    }
}
