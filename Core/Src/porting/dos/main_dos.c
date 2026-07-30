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
#include "dos_video.h"
#include "dos_input.h"
#include "dos_cpu.h"
#include <string.h>

/* Guest memory is a BSS array inside this overlay (.overlay_dos_bss), zeroed by
 * the launcher before we are entered. Not a pointer, and nothing to allocate. */
extern unsigned char mem[];
/* Guest RAM footprint, owned by 8086tiny.c so the two cannot drift. */
extern const unsigned int dos_mem_required;
extern unsigned short *regs16;
extern unsigned char *regs8;
extern unsigned char io_ports[];

extern int dos_cpu_init(int argc, char **argv);   /* 0 = ok, <0 = BIOS load failed */
extern int dos_cpu_frame(int cycles);

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

    odroid_system_init(APPID_DOS, AUDIO_SAMPLE_RATE);

    /* NOTE: the LCD is already in LUT8 mode and mem[] is already zeroed by the
     * time we get here -- the launcher must switch modes before copying this
     * overlay into the bonus area, and it zeroes .overlay_dos_bss (which mem[]
     * lives in) as part of the standard overlay load. Do NOT call
     * lcd_setup_framebuffers() here: it would re-zero the 150 KB framebuffer
     * footprint and rewrite MPU regions 3-6 underneath our own code. */
    printf("DOS: guest RAM %u KB at %p, ends %p (limit %p)\n",
           (unsigned)(dos_mem_required / 1024), (void *)mem,
           (void *)(mem + dos_mem_required), (void *)&__RAM_EMU_END__);

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
    
    /* The BIOS is mandatory: 8086tiny reads its instruction-decode tables out of
     * the BIOS image, so a missing file yields garbage decoding that looks exactly
     * like a broken memory map. Fail loudly here instead of burning debug time. */
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

    dos_screen_apply_rate();

    extern unsigned int inst_counter;
    extern unsigned short reg_ip;
    #define DBG_REG_CS 9            /* REG_CS is private to 8086tiny.c */
    unsigned int dbg_frames = 0;

    /* Guest CPU speed. Selectable at runtime because most DOS games have no
     * frame limiter and run at whatever speed the CPU provides -- the turbo
     * button problem. The table and the MAX-mode deadline loop live in
     * dos_cpu.c; this file only owns the menu row. */
    char dos_cpu_speed_value[DOS_CPU_VALUE_LEN];
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_SEPARATOR,
        {200, "CPU speed", dos_cpu_speed_value, 1, &dos_cpu_speed_update_cb},
        ODROID_DIALOG_CHOICE_LAST};
    /* Populate the value string before the menu can be opened. */
    dos_cpu_speed_update_cb(&options[1], ODROID_DIALOG_INIT, 0);

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
        }

        if (!lcd_is_swap_pending() && drawFrame) {
            /* Bracketed for the cpu/blit/idle split (docs/video/09-video-profiling.md).
             * Note this only runs when common_emu_frame_loop() said so: a dropped
             * frame skips the blit entirely, which preserves guest speed at the
             * cost of visual smoothness -- so blit cost per *frame* and blit cost
             * per *blit* are different numbers, and both are reported. */
            dos_prof_blit_begin();
            dos_blit();
            lcd_swap();
            dos_prof_blit_end();
        }

        if (drawFrame) {
            // TODO: Submit audio
        }

        /* The only place this loop deliberately waits. If idle_pct comes back at
         * ~0 there is no headroom left and the frame is CPU/blit bound. */
        dos_prof_idle_begin();
        common_emu_sound_sync(false);
        dos_prof_idle_end();
    }
}
