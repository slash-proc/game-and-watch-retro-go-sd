#include "odroid_display.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "odroid_system.h"
#include "odroid_sdcard.h"
#include "gw_lcd.h"
#include "common.h"
#include "appid.h"
#include <string.h>

extern unsigned char *mem;
extern unsigned short *regs16;
extern unsigned char *regs8;
extern unsigned char io_ports[];

extern void dos_cpu_init(int argc, char **argv);
extern int dos_cpu_frame(int cycles);

static void dos_blit(void) {
    // TODO: Blit CGA framebuffer to odroid_display
}

int app_main_dos(uint8_t load_state, uint8_t start_paused, int8_t save_slot) {
    printf("Initializing 8086tiny...\n");

    odroid_system_init(APPID_DOS, AUDIO_SAMPLE_RATE);
    lcd_setup_framebuffers(LCD_MODE_LUT8);
    
    // The LCD bonus pool and RAM_EMU are contiguous.
    size_t mem_size = 0;
    lcd_get_bonus_pool((uint8_t**)&mem, &mem_size);
    memset(mem, 0, mem_size); // Clear the actual available pool
    
    char *argv[5];
    int argc = 1;
    argv[0] = "8086tiny";
    argv[1] = RG_BASE_PATH_BIOS "/bios";
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
    
    dos_cpu_init(argc, argv);
    
    if (start_paused) {
        common_emu_state.pause_after_frames = 4;
        odroid_audio_mute(true);
    } else {
        common_emu_state.pause_after_frames = 0;
    }

    lcd_set_refresh_rate(60);
    common_emu_state.frame_time_10us = (uint16_t)(100000 / 60 + 0.5f);
    audio_start_playing(AUDIO_SAMPLE_RATE / 60);

    while (1) {
        odroid_gamepad_state_t joystick;
        odroid_input_read_gamepad(&joystick);

        common_emu_input_loop(&joystick, NULL, &dos_blit);

        bool drawFrame = common_emu_frame_loop();

        if (dos_cpu_frame(20000)) {
            // Emulation finished (CS:IP = 0:0)
            break;
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

    return 0;
}
