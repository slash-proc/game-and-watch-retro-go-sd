#include "odroid_display.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "odroid_system.h"
#include "odroid_sdcard.h"
#include <string.h>

// Hook into 8086tiny's memory space
extern unsigned char mem[];
extern unsigned short *regs16;
extern unsigned char *regs8;
extern unsigned char io_ports[];

void rg_emu_init(const char *rom_path) {
    printf("Initializing 8086tiny for %s\n", rom_path);
    // TODO: Load the ROM image, parse the .cfg profile
}

void rg_emu_frame(void) {
    // TODO: Execute 8086tiny interpreter loop for one frame's worth of cycles
    // TODO: Blit CGA framebuffer (mem[0xB8000] etc) to odroid_display
}

void rg_emu_deinit(void) {
    // TODO: Save state, close files
}
