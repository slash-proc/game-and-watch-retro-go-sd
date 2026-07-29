#include "odroid_display.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "odroid_system.h"
#include "odroid_sdcard.h"
#include "gw_lcd.h"
#include "gw_linker.h"
#include "common.h"
#include "appid.h"
#include "dos_video.h"
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

static void dos_blit(void) {
    dos_video_blit();
}

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

    lcd_set_refresh_rate(60);
    common_emu_state.frame_time_10us = (uint16_t)(100000 / 60 + 0.5f);
    audio_start_playing(AUDIO_SAMPLE_RATE / 60);

    /* TEMPORARY first-boot instrumentation -- remove once text renders.
     * Distinguishes "CPU is not advancing" from "CPU runs but nothing is drawn",
     * which are indistinguishable at a black screen. */
    extern unsigned int inst_counter;
    extern unsigned short reg_ip;
    #define DBG_REG_CS 9            /* REG_CS is private to 8086tiny.c */
    #define DBG_VID_TEXT 0xA8000    /* guest 0xB8000 folded: -0x10000 */
    unsigned int dbg_frames = 0;

    while (1) {
        odroid_gamepad_state_t joystick;
        odroid_input_read_gamepad(&joystick);

        common_emu_input_loop(&joystick, NULL, &dos_blit);

        bool drawFrame = common_emu_frame_loop();

        if (dos_cpu_frame(20000)) {
            // Emulation finished (CS:IP = 0:0)
            printf("DOS: emulation finished at CS:IP=%04X:%04X after %u frames\n",
                   regs16[DBG_REG_CS], reg_ip, dbg_frames);
            break;
        }

        if ((++dbg_frames & 0x3F) == 0) {   /* every 64 frames ~= 1s */
            printf("DOS: f=%u inst=%u CS:IP=%04X:%04X mode=%02X cols=%u vram[%02X %02X %02X %02X]\n",
                   dbg_frames, inst_counter,
                   regs16[DBG_REG_CS], reg_ip,
                   mem[0x449], (unsigned)*(unsigned short *)&mem[0x44A],
                   mem[DBG_VID_TEXT], mem[DBG_VID_TEXT + 1],
                   mem[DBG_VID_TEXT + 2], mem[DBG_VID_TEXT + 3]);
        }

        if (!lcd_is_swap_pending() && drawFrame) {
            dos_blit();
            lcd_swap();
        }

        if (drawFrame) {
            // TODO: Submit audio
        }

        common_emu_sound_sync(false);
    }
}
