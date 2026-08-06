#ifndef _DOS_PRECACHE_H
#define _DOS_PRECACHE_H

#include <stdint.h>

/* THE NOR CACHING THE MS-DOS CORE NEEDS, DONE BY THE LAUNCHER INSTEAD OF BY
 * THE CORE, AND THE ONLY REASON IS THE PROGRESS BAR.
 *
 * Three files go into OSPI NOR before a DOS session can start: the XIP code
 * blob (~40 KB), the guest .dsk (megabytes -- this is the one the user sees as
 * a frozen screen) and the .xipimg sidecar (up to 0x90000 = 589,824 B). All
 * three used to be cached from app_main_dos(), which is downstream of
 * lcd_setup_framebuffers(LCD_MODE_LUT8): the launcher's theme colours are gone
 * by then, so the core passed callbacks that only wrote to the 4 KB log ring
 * and the screen simply froze for the duration.
 *
 * The fix is the PICO-8 ordering (rg_emulators.c, Pico8CacheCodeToFlash()):
 * do the slow I/O while the machine is still RGB565 with the launcher theme
 * intact, draw the real odroid_overlay_draw_progress_bar(), and switch to LUT8
 * afterwards. Nothing in the caching path needs guest RAM to exist yet --
 * circular_flash_write() streams through a 16 KB *stack* buffer and a malloc'd
 * metadata block (gw_flash_alloc.c:300), never through mem[] or the bonus
 * area -- so the switch can wait.
 *
 * The core then reads the results out of these globals instead of calling
 * store_file_in_flash() itself. Addresses are OSPI memory-mapped and stay live
 * for the whole boot: live_add() is per-boot state and
 * flash_alloc_forget_live_files() has no caller.
 *
 * A NULL address is NOT an error for the .dsk or the sidecar -- "too big for
 * the part" and "no room clear of the files already live" both land there, and
 * both mean the title runs without that optimisation. It IS fatal for the code
 * blob, which is what app_main_dos() checks. */

#define DOS_XIP_PATH   "/cores/dos.xip"
/* The link-time address of the XIP blob. Shared, not duplicated: the launcher
 * runs the in-bound relocation pass and main_dos.c runs the out-bound one, and
 * a disagreement between them presents as "FATAL n unrelocated xip refs". */
#define DOS_CODE_BASE  0xDED00000u

/* /cores/dos.xip -- relocated on the way in against DOS_CODE_BASE. Mandatory. */
extern uint8_t *dos_xip_code_flash_addr;
extern uint32_t dos_xip_code_flash_size;

/* The guest .dsk, for load elision. Optional. */
extern uint8_t *dos_dsk_flash_addr;
extern uint32_t dos_dsk_flash_size;

/* /saves/dos/<base>.xipimg. Optional; the core still validates the key. */
extern uint8_t *dos_xipsm_flash_addr;
extern uint32_t dos_xipsm_flash_size;

/* THE NEGATIVE CONTROL FOR ANY LOAD-ELISION MEASUREMENT, and it moved with the
 * caching. It used to be DOS_CFLAGS_EXTRA=-DDOS_XIP_CACHE=0, which reached
 * main_dos.o only; the .dsk cache now lives in rg_emulators.o, so it has to be
 * CFLAGS_EXTRA=-DDOS_XIP_CACHE=0 (Makefile.common:757) to reach both. Defined
 * here so the launcher and the core cannot disagree about it. */
#ifndef DOS_XIP_CACHE
#define DOS_XIP_CACHE 1
#endif

#endif /* _DOS_PRECACHE_H */
