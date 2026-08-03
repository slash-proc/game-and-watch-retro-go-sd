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
#include "dos_meta.h"    /* the per-title COW pool sidecar (external/8086tiny) */
#include "dos_xms.h"     /* dos_xms_grown_bytes -- the backing-store zero-fill */
#include "dos_xipsm.h"   /* the .xipimg state machine (external/8086tiny) */
#include "dos_fbs.h"     /* the FAST-BOOT SNAPSHOT container (external/8086tiny) */
#include <string.h>
#include <stdio.h>

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

/* ---- Per-title COW pool sizing --------------------------------------------
 *
 * The pool array is linked for the WORST title, so the AXI it guarantees back
 * is the worst case and the per-title figure (110-458 KB) is thrown away.
 * external/8086tiny/dos_meta.h carries a measured per-title page count from
 * package time in a 64-byte `.dosmeta` sidecar beside the `.dsk`;
 * dos_cow_reserve() applies it, and everything the reservation does not claim
 * becomes the SPARE that dos_cow_arena_spare() offers to EGA planes, page
 * flipping and the 4K decode cache.
 *
 * READ dos_meta.h BEFORE CHANGING ANY OF THIS. The one thing that matters here:
 * the number is a reservation, not a cap. A title that exceeds it grows back
 * into the spare, and with no borrower installed -- which is the state of this
 * file today, because nothing has taken the spare yet -- growth always succeeds
 * up to the full array. So a missing, stale or simply wrong sidecar costs
 * nothing but a few out-of-line grow calls, and can never lose a guest store.
 * Anything that later TAKES the spare must install a reclaim callback and
 * honour it; a borrower that will not give a page back is the only way this
 * mechanism can cost correctness, and dos_cow_pool_stats()'s `grows` plus
 * dos_cow_stats()'s `lost` are how that shows up in the log. */
extern unsigned int dos_cow_reserve(unsigned int pages);

/* ---- Per-title machine size (INT 12h) -------------------------------------
 *
 * The .dosmeta sidecar's v2 field. Tell a title it has an N KB machine and
 * DOS's own allocator will never build an MCB above N KB, so guest memory
 * above N KB is unreachable by allocation -- no pool, no fault, no copy, no
 * bet. It must be set BEFORE dos_cpu_init(), which is where the BIOS blob is
 * loaded and the word poked; that is why the call sits here, in the function
 * the ordering comment at dos_cpu_init() already covers.
 *
 * A wrong number is never fatal: DOS simply cannot allocate above it, and a
 * store that lands there anyway is served and COUNTED (dos_mach_over), exactly
 * as DOS_COW_GROW handles an under-reserved pool. See the DOS_MACH_KB_WORD
 * block in external/8086tiny/8086tiny.c and test286/runmach.sh arm 6. */
extern int dos_mach_set_kb(unsigned int kb);
extern unsigned int dos_mach_kb, dos_mach_applied, dos_mach_over;
extern void dos_cow_pool_stats(unsigned *arena, unsigned *reserved,
                               unsigned *live, unsigned *grows);
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

/* ------------------------------------------------------- emulator XIP ---
 * The cold half of the port does not live in RAM. dos_font_data.o (the CP437
 * glyphs), dos_osk.o (the on-screen keyboard) and dos_xms.o (the XMS/HMA
 * driver) are linked at a sentinel address instead, shipped as one file --
 * /cores/dos.xip -- cached into QSPI NOR, and executed and read straight out
 * of it.
 *
 * WHY, in bytes. .overlay_dos_bss holds mem[], 778 KB of guest RAM, and it
 * starts at the first 4 KB boundary after .overlay_dos's code and rodata. So
 * code removed from the overlay becomes guest memory -- but only in 4,096-byte
 * quanta, which is why three objects move and not one. Measured:
 * _OVERLAY_DOS_BSS_END 0x240fe010 -> 0x240fb018 against __RAM_EMU_END__
 * 0x24100000, i.e. 8,176 B of headroom -> 20,456 B. That is what unblocks EGA
 * 0Eh/0Fh/10h and the 4K decode cache (external/8086tiny/docs/handover.md).
 *
 * NO POSITION-INDEPENDENT CODE, and that is the whole trick -- the GBA
 * precedent (main_gba.c:224-243). The blob is linked at DOS_CODE_BASE and
 * relocated to wherever the flash cache put it. Two passes do it:
 *
 *   1. on the way IN: dos_relocate_xip() rewrites every sentinel word in each
 *      buffer BEFORE it is programmed. Not by rewriting flash afterwards -- a
 *      rewrite needs an erase, and an erase interrupted by a flat battery
 *      leaves a blank hole indistinguishable from a finished job. On a cache
 *      hit nothing is written and nothing needs to be: that copy was relocated
 *      to the same address when it was first stored.
 *   2. in the overlay: every call veneer and rodata reference the RAM half
 *      holds still contains a sentinel. dos_cache_xip_to_flash() patches
 *      [_DOS_MAIN_CODE_END, _OVERLAY_DOS_LOAD_END) before a single line of
 *      core code runs. main_dos.o is deliberately outside that window -- it
 *      defines DOS_CODE_BASE, and a scan that cannot tell the constant from a
 *      reference would rewrite the constant it is built on.
 *
 * This is MANDATORY, unlike the .dsk cache below: if it fails the OSK and the
 * font are still at 0xDED0xxxx and the first text blit jumps into nothing. The
 * caller returns to the launcher rather than starting the guest.
 *
 * One thing that does NOT need handling here: circular_flash_write() reads in
 * 16 KB buffers (gw_flash_alloc.c:302) and the blob is ~10 KB, so it arrives
 * as a single call and no sentinel word can straddle a buffer boundary. If
 * the blob ever exceeds 16 KB, revisit -- the pass truncates to a word
 * multiple per buffer exactly as gba_relocate_xip() does. */
#define DOS_CODE_BASE  0xDED00000u
#define DOS_XIP_PATH   "/cores/dos.xip"

static uint8_t *g_dos_xip_addr;
static uint32_t g_dos_xip_size;

static int patch_dos_sentinels(uint32_t *start, uint32_t *end, int32_t offset, uint32_t size)
{
    int patched = 0;
    for (uint32_t *p = start; p < end; p++) {
        uint32_t v = *p;
        /* & ~1 so a Thumb function pointer matches; the bit is preserved by
         * adding the offset to the original value. */
        if ((v & ~1u) >= DOS_CODE_BASE && (v & ~1u) < DOS_CODE_BASE + size) {
            *p = (uint32_t)(v + offset);
            patched++;
        }
    }
    return patched;
}

static void dos_relocate_xip(uint8_t *buffer, uint32_t length, uint32_t offset_in_file,
                             uint8_t *file_address, uint32_t file_size)
{
    (void)offset_in_file;
    int32_t offset = (int32_t)((uint32_t)file_address - DOS_CODE_BASE);
    patch_dos_sentinels((uint32_t *)buffer, (uint32_t *)(buffer + (length & ~3u)),
                        offset, file_size);
}

static void dos_xip_code_progress(uint32_t total, uint32_t done, uint8_t pct)
{
    (void)total; (void)done; (void)pct;
    wdog_refresh();
}

/* 0 = ok, -1 = the core cannot run. */
static int dos_cache_xip_to_flash(void)
{
    uint32_t t0 = HAL_GetTick();
    int32_t  offset;
    int      n;

    g_dos_xip_size = 0;   /* 0 = "whole file"; the cache fills it in */
    g_dos_xip_addr = store_file_in_flash_relocate(DOS_XIP_PATH, &g_dos_xip_size,
                                                  false, &dos_xip_code_progress,
                                                  &dos_relocate_xip);
    if (g_dos_xip_addr == NULL || g_dos_xip_size == 0) {
        printf("DOS: %s missing or uncacheable - cannot start\n", DOS_XIP_PATH);
        return -1;
    }

    offset = (int32_t)((uint32_t)g_dos_xip_addr - DOS_CODE_BASE);
    n = patch_dos_sentinels((uint32_t *)_DOS_MAIN_CODE_END,
                            (uint32_t *)_OVERLAY_DOS_LOAD_END,
                            offset, g_dos_xip_size);
    __DSB();
    __ISB();

    /* The one fragile assumption, made loud rather than left implicit.
     *
     * main_dos.c CALLS into the blob (dos_osk_*, dos_xms_*), so ld synthesises
     * long-branch veneers for those calls, and each veneer carries a sentinel
     * literal. Measured on this link, ld places them immediately AFTER
     * main_dos.o -- i.e. inside the scanned window, at 0x24025c94 -- which is
     * why the pass works. Nothing in the ELF spec pins that down.
     *
     * So: the unscanned region [__ram_emu_dos_start__, _DOS_MAIN_CODE_END)
     * must contain no sentinel word except the DOS_CODE_BASE constant itself
     * (bare 0xDED00000, two copies: the range compare above and the offset
     * computation in dos_relocate_xip). Anything else there is a reference the
     * pass could not reach, and would fault on first use with no clue why.
     * Costs one 1,168-byte scan, once. */
    {
        const uint32_t *q = (const uint32_t *)__ram_emu_dos_start__;
        const uint32_t *qe = (const uint32_t *)_DOS_MAIN_CODE_END;
        int stranded = 0;
        for (; q < qe; q++) {
            uint32_t v = *q;
            if ((v & ~1u) >= DOS_CODE_BASE && (v & ~1u) < DOS_CODE_BASE + g_dos_xip_size
                && v != DOS_CODE_BASE)
                stranded++;
        }
        if (stranded) {
            printf("DOS: FATAL %d unrelocated xip refs ahead of the scan window\n",
                   stranded);
            return -1;
        }
    }

    printf("DOS: xip code at %p, %lu bytes, %d refs patched, %lu ms\n",
           (void *)g_dos_xip_addr, (unsigned long)g_dos_xip_size, n,
           (unsigned long)(HAL_GetTick() - t0));
    return 0;
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

/* THE TRIM ARMS ITSELF NOW, INSIDE dos_cpu_init().
 *
 * What used to be here: dos_trim_arm(), which looked for the trimmed tail's
 * bytes inside the CACHED .dsk in external flash. It needed DOS_XIP_CACHE=1
 * (off by default), then linearly SCANNED that image for a contiguous
 * dos_mem_trim-byte run of zeros, and when it could not find one the caller
 * REFUSED TO START THE TITLE. external/8086tiny/docs/memory/18-why-trim-is-
 * not-on.md section 4 is the post-mortem: BATTLECHESS.dsk is 66,060,288 B
 * against a 64 MB part and can never be cached, so turning the trim on would
 * have traded +32 KB of AXI headroom for a title that will not boot. The trim
 * was therefore never enabled and the headroom was never collected.
 *
 * dos_trim_arm_self() (external/8086tiny/8086tiny.c) needs none of it. Every
 * granule of the window points at ONE zero page carried in the AHB block --
 * 4 KB of AHB SRAM, a region AXI headroom does not pay for -- so 53,248 bytes
 * of guest address space cost 4,104 bytes of a different memory. No flash, no
 * cache, no scan, and nothing to refuse. It is called from dos_cpu_init()
 * itself, which is also the only place that can guarantee it happens before
 * the BIOS decode tables are read through the fold.
 *
 * The bytes are provably zero rather than assumed so: DOS_MEM_TRIM is capped
 * at 0xD000, so the trim starts at guest 0xF3000 at the lowest, and the BIOS
 * image is 8,096 bytes at 0xF0100 ending at 0xF203F. No legal trim can overlap
 * a byte the BIOS supplies.
 *
 * Evidence: external/8086tiny/test286/runtrimzero.sh -- all fourteen shipped
 * images at the 0xD000 maximum, each byte-identical to an untrimmed run with
 * two granules copied out and zero stores lost, against a pool-less negative
 * control that loses 38 stores per boot at default parameters. */

/* ---- The .xipimg state machine -------------------------------------------
 *
 * external/8086tiny/docs/improvement-brainstorming.md §H, and the answer to
 * docs/memory/04-xip-execute.md §5's "the firmware glue is not written".
 *
 * FIRST RUN of a title: observe the demand region at 4 KB granule resolution
 * over [T1, T2), take the longest run of granules the guest did not change, and
 * write /saves/dos/<title>.xipimg. Nothing is armed.
 *
 * EVERY RUN AFTER: store_file_in_flash() the sidecar, check its key, its ABI
 * word and its payload CRC, then at T2 byte-compare it against live guest RAM
 * and -- only on a match -- install the window over external flash.
 *
 * The design, the invalidation rules and the reason the window is armed
 * READ-ONLY-WITH-COW rather than read-only-hard are all in
 * external/8086tiny/dos_xipsm.h, which is worth reading before changing
 * anything here. The one line to carry away:
 *
 *     A WRONG SNAPSHOT COSTS POOL PAGES. IT CANNOT COST CORRECTNESS.
 *
 * because dos_mem_map_ro() puts DOS_COW_TRAP in the write map, so a guest store
 * into the window is copied to a pool page instead of being lost in NOR (where
 * erase-before-write means a write neither faults nor lands).
 *
 * FOUR THINGS THIS FILE OWES THE MECHANISM, none of which are in the submodule:
 *
 *  1. T1/T2 IN FRAMES. dos_xipsm.c compares the caller's monotonic measure of
 *     GUEST WORK against t1/t2 and has no clock. 04-xip-execute.md §3.3 is the
 *     reason it must be work and not wall time: an idle interval reported
 *     128 of 128 granules clean and a store landed in the very next one. On a
 *     handheld the user is playing, so frames are work.
 *
 *  2. THE KEY. Byte-identical to .dosmeta's -- CRC32 of the first
 *     DOS_META_CRC_SPAN bytes of the .dsk plus its size -- and computed ONCE,
 *     by dos_pool_reserve_from_meta(), which now hands it back rather than
 *     dropping it. Reading 64 KB of a 66 MB image twice during boot would be
 *     the second-most expensive thing this core does before the guest starts.
 *
 *  3. ONE fopen AT A TIME. MAX_OPEN_FILES is 8 (Core/Src/syscalls.c:51) and
 *     running out fails SILENTLY (../../../../CLAUDE.md). dos_cpu_init() holds
 *     three for the whole run, so the capture's handle is opened at T2 and
 *     closed before the function returns.
 *
 *  4. GRACEFUL DEGRADATION, WHICH IS NOT OPTIONAL. Missing, stale, corrupt,
 *     unwritable, or no flash at all: every one of them ends with the guest
 *     running from SD exactly as it does today. There is no failure return in
 *     this section and nothing below it consults the window.
 *
 * A FULL FLASH IS NORMAL, NOT AN ERROR: BATTLECHESS.dsk is 66,060,288 bytes
 * against a 64 MB part, so find_write_slot() (gw_flash_alloc.c:144) answering
 * "no" is an outcome the design owes an answer to, and the answer is "run from
 * SD" (02-guest-xip.md §5, "the one new obligation").
 */
#ifndef DOS_XIPSM_ENABLE
#define DOS_XIPSM_ENABLE 1
#endif

static void dos_cow_log(const char *when);   /* defined below; used at arm time */

/* The .dsk identity, computed once by dos_image_key_crc() before
 * dos_cpu_init() and consumed by dos_fbs_boot() after it. */
static unsigned long dos_fbs_boot_key;

#if DOS_XIPSM_ENABLE
/* The observation range: the whole conventional demand region. Deliberately
 * NOT a program image -- this port has no INT 21h hook and should not grow one
 * (02-guest-xip.md §10), and the longest-clean-run rule needs no idea where the
 * program is. Must match 8086tiny.c's DOS_DEMAND_BASE/TOP when demand paging is
 * built in; when it is not, this is still a legal range to observe. */
#define DOS_XIPSM_OBS_BASE 0x10000u
#define DOS_XIPSM_OBS_LEN  0x90000u

/* ~5 s and ~25 s at 60 Hz. T1 is late enough that DOS has booted and the title
 * has loaded (02-guest-xip.md §10 puts EXEC at ~13.5 M instructions), T2 far
 * enough past it that the interval contains real play. Both are frames, not
 * milliseconds: a slow frame is more guest work, not less. */
#ifndef DOS_XIPSM_T1_FRAMES
#define DOS_XIPSM_T1_FRAMES 300u
#endif
#ifndef DOS_XIPSM_T2_FRAMES
#define DOS_XIPSM_T2_FRAMES 1500u
#endif

/* 8086tiny.c's fold-aware guest pointer. Every read below is granule-aligned
 * and at most one granule long, which is what makes a single memcpy correct
 * despite the seam (a COW'd granule is a pool page and is NOT contiguous with
 * its neighbour). */
extern unsigned char *dos_mem_ptr(unsigned int guest_addr);

static dos_xipsm_t dos_sm;
static char dos_sm_path[128];
static uint8_t *dos_sm_flash;
static uint32_t dos_sm_flash_size;

static void dos_sm_read(unsigned int lin, unsigned char *dst, unsigned int len, void *ctx)
{
    (void)ctx;
    memcpy(dst, dos_mem_ptr(lin), len);
}

static unsigned long dos_sm_write(const void *src, unsigned long len, void *ctx)
{
    return (unsigned long)fwrite(src, 1, (size_t)len, (FILE *)ctx);
}

static void dos_sm_progress(uint32_t total, uint32_t done, uint8_t pct)
{
    static uint8_t last = 255;
    (void)total; (void)done;
    if (pct / 25 != last / 25 || last == 255) {
        last = pct;
        printf("DOS: xipimg caching %u%%\n", pct);
    }
    wdog_refresh();
}

/* "/roms/dos/KEEN4.dsk" -> "/saves/dos/KEEN4.xipimg". /saves/dos is where the
 * SD half lives because store_file_in_flash() takes a PATH: SD is where a
 * snapshot is produced, flash is where it is consumed, and keeping those
 * separate is what stops a guest store from ever reaching flash
 * (dos_xipimg.h, "SAFETY MODEL"). */
static int dos_sm_make_path(const char *dsk_path)
{
    const char *base = dsk_path, *p, *dot;
    size_t n;

    for (p = dsk_path; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    dot = 0;
    for (p = base; *p; p++)
        if (*p == '.')
            dot = p;
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n == 0 || n + sizeof("/saves/dos/.xipimg") >= sizeof dos_sm_path)
        return -1;
    strcpy(dos_sm_path, "/saves/dos/");
    memcpy(dos_sm_path + 11, base, n);
    strcpy(dos_sm_path + 11 + n, ".xipimg");
    return 0;
}

/* Boot half. Runs BEFORE dos_pool_reserve_from_meta() so that its answer --
 * "will a window be armed this run?" -- is available to dos_meta_pages().
 * Returns 1 when a snapshot passed every static check, i.e. when the window
 * WILL be armed at the gate unless guest RAM diverges from it.
 *
 * WHY THE WINDOW IS NOT ARMED HERE, AT T0, WHICH IS WHAT §H ASKED FOR.
 * Arming at boot was tried on paper and is UNSAFE, and the reason is worth
 * recording because it looks like it should work. A snapshot is the SETTLED
 * image -- what the guest's memory looks like after DOS has loaded the program
 * and the program has decompressed itself. Arming it at T0 would make the guest
 * READ those settled bytes before the loader has written them: DOS walks MCB
 * chains and free-block headers through exactly that range while it is coming
 * up, and it would see a program image where it expects its own allocator
 * state. Copy-on-write does not help, because COW protects STORES and this is a
 * LOAD returning the wrong bytes.
 *
 * Making a T0 arm correct is the LOAD-ELISION problem 04-xip-execute.md §3.1
 * names -- "the bytes must already be in flash and the INT 13h read that would
 * write them must become a remap instead of a copy" -- and that is not built.
 * So the arm is at the gate, and the reservation is told what is going to
 * happen rather than what has happened.
 *
 * That distinction costs nothing measurable today, and the measurement is in
 * external/8086tiny/docs/memory/17-xipimg-state-machine.md §5: pages_xip came
 * out EQUAL to pages_cold on both images tested, because arming a window at T2
 * cannot retroactively free pool pages that the load spike already took at T1.
 * The sidecar's second number is real, but it is load elision's to earn. */
static int dos_xipsm_boot(const char *dsk_path, uint32_t dsk_size, unsigned long key_crc)
{
    uint32_t t0 = HAL_GetTick();

    dos_xipsm_init(&dos_sm, DOS_XIPSM_OBS_BASE, DOS_XIPSM_OBS_LEN,
                   DOS_XIPSM_T1_FRAMES, DOS_XIPSM_T2_FRAMES,
                   key_crc, (unsigned long)dsk_size);
    if (dos_sm.st == DOS_XIPSM_OFF) {
        printf("DOS: xipimg disabled (observation range refused)\n");
        return 0;
    }
    if (dos_sm_make_path(dsk_path) != 0) {
        printf("DOS: xipimg path too long for %s\n", dsk_path);
        dos_sm.st = DOS_XIPSM_OFF;
        return 0;
    }

    /* store_file_in_flash() opens the file itself and closes it before
     * returning, so no handle is held across dos_cpu_init(). NULL is the
     * missing-file case AND the full-flash case, and neither is an error. */
    dos_sm_flash_size = 0;
    dos_sm_flash = store_file_in_flash(dos_sm_path, &dos_sm_flash_size,
                                       false, &dos_sm_progress);
    dos_xipsm_offer(&dos_sm, dos_sm_flash, (unsigned long)dos_sm_flash_size);

    printf("DOS: xipimg %s -> %s (%s), flash %p %lu B, %lu ms\n",
           dos_sm_path, dos_xipsm_state_name(dos_sm.st),
           dos_xipsm_reason_name(dos_sm.reason), (void *)dos_sm_flash,
           (unsigned long)dos_sm_flash_size,
           (unsigned long)(HAL_GetTick() - t0));

    return dos_sm.st == DOS_XIPSM_LOADED;
}

/* Frame half. One call per frame; everything below T1 is a single compare. */
static void dos_xipsm_frame(unsigned int frames)
{
    if (dos_sm.st == DOS_XIPSM_OFF)
        return;
    if (!dos_xipsm_tick(&dos_sm, (unsigned long)frames, &dos_sm_read, NULL))
        return;

    switch (dos_sm.st) {
    case DOS_XIPSM_ARMED:
        printf("DOS: xipimg ARMED %lu B at guest %05X from flash\n",
               dos_sm.win_len, dos_sm.win_base);
        dos_cow_log("armed");
        break;

    case DOS_XIPSM_REJECTED:
        printf("DOS: xipimg not armed (%s, first_diff %lu) - running from SD\n",
               dos_xipsm_reason_name(dos_sm.reason), dos_sm.first_diff);
        break;

    case DOS_XIPSM_CAPTURED: {
        /* The one visible cost of this feature: ~400 KB streamed to SD, once,
         * on the first run of a title. It is a hitch of a few hundred ms in one
         * frame, not a stall every boot. Writing it at teardown instead was
         * considered and is WRONG: the payload has to be the bytes as they were
         * at T2, because T2 is where the next run's gate compares. */
        uint32_t t0 = HAL_GetTick();
        unsigned long want = 64UL + dos_sm.win_len, got = 0;
        FILE *f = fopen(dos_sm_path, "wb");

        if (!f) {
            /* /saves/dos may not exist, or the card may be read-only. Neither
             * is worth failing a game over. */
            printf("DOS: xipimg cannot write %s - snapshot skipped\n", dos_sm_path);
            dos_sm.st = DOS_XIPSM_OFF;
            break;
        }
        got = dos_xipsm_capture(&dos_sm, &dos_sm_read, NULL, &dos_sm_write, f);
        fclose(f);
        if (got != want) {
            /* A truncated payload with a good header is the ONE shape this
             * design cannot detect on the way back in -- the payload CRC covers
             * `len` bytes that are no longer there -- so delete it rather than
             * leave it. */
            remove(dos_sm_path);
            printf("DOS: xipimg short write %lu of %lu - deleted\n", got, want);
        } else {
            printf("DOS: xipimg captured %lu B at guest %05X (%lu of %u granules "
                   "clean), %lu B written to %s in %lu ms\n",
                   dos_sm.win_len, dos_sm.win_base, dos_sm.clean_gran,
                   dos_sm.ngran, got, dos_sm_path,
                   (unsigned long)(HAL_GetTick() - t0));
        }
        dos_sm.st = DOS_XIPSM_OFF;      /* one capture per session */
        break;
    }

    default:
        break;
    }
}
static void dos_xipsm_report(void)
{
    printf("DOS: xipimg final state %s (%s), window %lu B at %05X\n",
           dos_xipsm_state_name(dos_sm.st), dos_xipsm_reason_name(dos_sm.reason),
           dos_sm.win_len, dos_sm.win_base);
}

/* FAST-BOOT SNAPSHOT -- boot straight into the game.
 *
 * The driver lives in its own translation unit, dos_fbs_glue.c, and that is not
 * organisation: main_dos.o is EXCLUDED from `.xip_dos` (ITCM->flash veneers fail
 * on arm-none-eabi 15.x for objects that section takes), so every byte of this
 * driver written here is a byte of AXI the guest cannot have -- and AXI headroom
 * was 1,200 B. Written inline first, it overflowed the link. Same reason
 * dos_arena.c exists as a file of its own (see the .xip_dos comment in
 * STM32H7B0VBTx_SDCARD.ld).
 *
 * Design: external/8086tiny/dos_fbs.h. Read §1 there first -- the one thing this
 * must not be mistaken for is a memory-reclaim mechanism. */
void dos_fbs_boot(const char *dsk_path, unsigned long dsk_size,
                  unsigned long key_crc, unsigned long mach_kb);
void dos_fbs_frame(unsigned int frames);

#else
static int dos_xipsm_boot(const char *dsk_path, uint32_t dsk_size, unsigned long key_crc)
{ (void)dsk_path; (void)dsk_size; (void)key_crc; return 0; }
static void dos_xipsm_frame(unsigned int frames) { (void)frames; }
static void dos_xipsm_report(void) { }
#endif

/* One line per session. `lost` is the only number that can be wrong silently:
 * it counts stores that reached a granule with no writable page behind it and
 * were routed to the scratch page. A nonzero value means the pool is too small
 * for what is mapped, and the fix is DOS_COW_POOL_PAGES -- subtracting 4,104
 * bytes per page from whatever the trim reclaimed. */
static void dos_cow_log(const char *when)
{
    unsigned faults = 0, pages = 0, pool = 0, lost = 0;
    unsigned arena = 0, resv = 0, live = 0, grows = 0;

    dos_cow_stats(&faults, &pages, &pool, &lost);
    if (pool == 0 && faults == 0)
        return;                         /* feature not built in; say nothing */
    dos_cow_pool_stats(&arena, &resv, &live, &grows);
    /* Both halves on one line, because they are only interpretable together.
     * `lost` must be 0 -- it counts stores that reached a granule with no
     * writable page behind it and were routed to the scratch page. `grows`
     * climbing is how a stale .dosmeta measurement announces itself, and
     * `faults` climbing on a title that has a .xipimg armed is the COST of a
     * stale snapshot becoming visible: dos_xipsm.h §2 says a wrong snapshot
     * buys pool pages instead of losing stores, and this is the bill. */
    printf("DOS: cow %s faults=%u pages=%u/%u lost=%u | pool arena=%u resv=%u "
           "live=%u grows=%u spare=%uB\n",
           when, faults, pages, pool, lost, arena, resv, live, grows,
           (unsigned)((arena > live ? arena - live : 0) * 4104u));
}

/* ---- The .dosmeta sidecar -------------------------------------------------
 *
 * `<image>.dsk` -> `<image>.dosmeta`, 64 bytes, written at package time by
 * tools/mkdosdisk.py. Read it, check its key against the image we are actually
 * booting, and hand the page count to dos_cow_reserve().
 *
 * EVERY failure here is silent-by-design in the sense that it changes nothing:
 * no file, a stale file, a file from another emulator ABI, a file for a
 * different disk -- all end with the full pool, i.e. exactly the behaviour of
 * the build before sidecars existed. That is why there is no error return. What
 * is NOT silent is the log line: a title that reserves and then grows says so,
 * and `grows` climbing every boot is how a stale measurement is noticed.
 *
 * ONE fopen, held for the length of this function. MAX_OPEN_FILES is 8 and
 * running out fails silently (../../../../CLAUDE.md); dos_cpu_init() takes
 * three afterwards, so this handle must be closed before it runs -- and it is.
 * The .dsk is opened a second time here for the key CRC, again transiently. */
/* The cache key both sidecars share: CRC32 of the first DOS_META_CRC_SPAN bytes
 * of the image. Bounded because BATTLECHESS.dsk is 66 MB and this runs during
 * boot; dsk_size carries the rest of the discrimination. It is a cache key, not
 * a correctness boundary -- being wrong about it costs a reservation and a
 * snapshot, never the guest.
 *
 * Computed ONCE per boot and handed to both dos_pool_reserve_from_meta() and
 * dos_xipsm_boot(). Reading 64 KB of a 66 MB image twice before the guest's
 * first instruction is not free on this device, and the two callers must agree
 * anyway or a .dosmeta and a .xipimg could disagree about which image is
 * mounted. */
static unsigned long dos_image_key_crc(const char *dsk_path)
{
    static unsigned char buf[1024];
    unsigned long left = DOS_META_CRC_SPAN;
    unsigned long acc = 0xFFFFFFFFUL;
    size_t got;
    FILE *f = fopen(dsk_path, "rb");

    if (!f)
        return 0;
    while (left && (got = fread(buf, 1, left < sizeof buf ? left : sizeof buf, f)) > 0) {
        /* Streamed CRC32: dos_meta_crc32() is one-shot, so fold by hand with
         * the same polynomial rather than buffering 64 KB. */
        size_t k;
        for (k = 0; k < got; k++) {
            int b;
            acc ^= buf[k];
            for (b = 0; b < 8; b++)
                acc = (acc >> 1) ^ (0xEDB88320UL & (unsigned long)(-(long)(acc & 1)));
        }
        left -= got;
    }
    fclose(f);
    return (acc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

static void dos_pool_reserve_from_meta(const char *dsk_path, uint32_t dsk_size,
                                       unsigned long crc, int xip_armed)
{
    unsigned char raw[DOS_META_BYTES];
    char path[256];
    dos_meta_t m;
    dos_meta_status_t st;
    unsigned arena = 0, resv = 0, live = 0, grows = 0;
    size_t n, i;
    FILE *f;

    n = strlen(dsk_path);
    if (n + 8 >= sizeof path)
        return;
    memcpy(path, dsk_path, n + 1);
    /* Replace the extension, not append: "KEEN4.dsk" -> "KEEN4.dosmeta". */
    for (i = n; i > 0; i--)
        if (path[i - 1] == '.') { n = i - 1; break; }
        else if (path[i - 1] == '/') break;
    strcpy(path + n, ".dosmeta");

    f = fopen(path, "rb");
    if (!f)
        return;                         /* no sidecar: full pool, as before */
    n = fread(raw, 1, sizeof raw, f);
    fclose(f);

    st = dos_meta_parse(raw, (unsigned long)n, dsk_size, crc, &m);
    if (st != DOS_META_OK) {
        printf("DOS: meta %s ignored (%s)\n", path, dos_meta_status_name(st));
        return;
    }
    /* `xip_armed` now carries an answer instead of a TODO. It is set by
     * dos_xipsm_boot(), which runs BEFORE this call and has already decided:
     * 1 means a .xipimg passed its magic, version, ABI, key, geometry and
     * payload CRC, so the window WILL be installed at the gate unless live
     * guest RAM diverges from it.
     *
     * That is one step short of "armed", and the step is deliberate --
     * dos_xipsm.h explains why a T0 arm is unsafe without load elision. The
     * risk the old comment named ("believing pages_xip without the window is
     * the one way to under-reserve on purpose") is bounded twice over:
     *
     *   - the number is a RESERVATION, NOT A CAP. With no borrower installed
     *     the spare belongs to nobody, dos_cow_grow() always succeeds, and a
     *     wrong number cannot lose a store (dos_meta.h; test286/runpool.sh with
     *     -DDOS_COW_GROW=0 as the negative control).
     *   - measured, pages_xip EQUALS pages_cold, because arming at the gate
     *     cannot retroactively free the pool pages the load spike already took.
     *     Every shipped .dosmeta carries pages_xip = 0 today, and
     *     dos_meta_pages() falls back to pages_cold when it is 0 -- so this
     *     argument currently selects nothing at all.
     *     (external/8086tiny/docs/memory/17-xipimg-state-machine.md §5.)
     */
    /* The machine size, before the pool: it is what dos_cpu_init() reads, and
     * it is the only number here that cannot be applied late. 0 means the
     * title was never profiled (and every v1 sidecar says 0 by construction),
     * in which case the guest keeps the 640 KB it has always had. */
    {
        unsigned long kb = dos_meta_mach_kb(&m);

        if (kb && dos_mach_set_kb((unsigned)kb))
            printf("DOS: machine size %lu KB from %s\n", kb, path);
    }

    if (dos_cow_reserve((unsigned)dos_meta_pages(&m, xip_armed)) == 0) {
        printf("DOS: meta pool reservation refused\n");
        return;
    }
    dos_cow_pool_stats(&arena, &resv, &live, &grows);
    printf("DOS: pool reserved %u of %u pages (%u B spare, xip_armed=%d, "
           "cold=%lu xip=%lu)\n",
           resv, arena, (unsigned)((arena - resv) * 4104u), xip_armed,
           m.pages_cold, m.pages_xip);
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

    /* FIRST thing after system init, and before anything can reach the cold
     * half: until this returns, every reference to the font, the OSK and the
     * XMS driver still holds a 0xDED0xxxx sentinel. Also before the .dsk cache
     * below, so the blob is live_add()'ed first and find_write_slot() cannot
     * later erase it. */
    if (dos_cache_xip_to_flash() != 0) {
        printf("DOS: refusing to start without %s\n", DOS_XIP_PATH);
        return;
    }

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
    /* BEFORE dos_cpu_init() as well, and for a harder reason than the trim's:
     * dos_cow_reserve() refuses once a pool page has been handed out, and
     * dos_cpu_init() runs guest-visible BIOS setup. There is no failure path --
     * every way this can go wrong ends with the full pool, which is what the
     * build was linked for. */
    {
        /* One 64 KB read of the image, shared by both sidecars. Measured at
         * 43 ms when each computed its own; the duplicate read is gone. */
        uint32_t t0_meta = HAL_GetTick();
        unsigned long key = dos_image_key_crc(ACTIVE_FILE->path);
        /* BEFORE the reservation, so its answer can select pages_xip -- that is
         * the whole reason these two are adjacent. Also before dos_cpu_init()
         * for the fopen budget: store_file_in_flash() opens and closes the
         * .xipimg itself, and dos_cpu_init() then holds three handles out of
         * MAX_OPEN_FILES = 8 for the whole run. */
        int xip = dos_xipsm_boot(ACTIVE_FILE->path, (uint32_t)st.size, key);
        /* The same key serves the .fbs, which is restored after
         * dos_cpu_init() and so cannot recompute it there without a second
         * 64 KB read of the image. */
        dos_fbs_boot_key = key;
        dos_pool_reserve_from_meta(ACTIVE_FILE->path, (uint32_t)st.size, key, xip);
        printf("DOS: meta+xipsm path took %lu ms\n",
               (unsigned long)(HAL_GetTick() - t0_meta));
    }

    /* NO TRIM ARMING HERE ANY MORE, AND NO REFUSAL. dos_cpu_init() arms the
     * trimmed tail itself, from a zero page in the AHB block, before it reads
     * the BIOS decode tables through the fold -- see the note further up this
     * file. A title can no longer fail to start because its .dsk was too large
     * to cache, which is what BATTLECHESS.dsk would have done. */
    int init_rc = dos_cpu_init(argc, argv);
    if (init_rc != 0) {
        printf("DOS: BIOS load failed (%d) - expected %s\n", init_rc, dos_bios_path);
        return;
    }

    /* FAST-BOOT SNAPSHOT, and AFTER dos_cpu_init() is the whole design: init
     * has opened the disks, loaded the BIOS blob and built the decode tables --
     * all of which a restore needs and none of which a snapshot carries -- and
     * has not run one guest instruction. See the block above dos_fbs_boot(). */
    dos_fbs_boot(ACTIVE_FILE->path, (unsigned long)st.size, dos_fbs_boot_key,
                 (unsigned long)dos_mach_kb);
    
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

        /* The .xipimg state machine. Below T1 this is one compare; at T1 and T2
         * it is 144 granule CRC32s (~576 KB read, once each); at T2 on a hit it
         * is the ~100 KB memcmp against memory-mapped OSPI that decides whether
         * the window is armed. Nothing here runs after the machine settles. */
        dos_xipsm_frame(dbg_frames);
        /* The fast-boot snapshot's capture mark. One compare below it, and
         * nothing at all once it has fired. */
        dos_fbs_frame(dbg_frames);
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
                       "putc=%lu tick=%lu/%lu rs=%lu frames=%lu/%lums xms=w%lu/%luK r%lu/%luK g%luK\n",
                       dos_cpu_profile_name(), (unsigned long)s.mhz,
                       (unsigned long)s.insn_per_frame, (unsigned long)s.insn_per_sec,
                       (unsigned long)s.cyc_per_insn,
                       s.cpu_pct, (unsigned long)s.cpu_us_frame,
                       s.blit_pct, (unsigned long)s.blit_us, (unsigned long)s.blits,
                       s.idle_pct, s.other_pct, (unsigned long)s.putchars,
                       (unsigned long)s.int8_fired, (unsigned long)s.int8_due,
                       (unsigned long)s.int8_resync,
                       (unsigned long)s.frames, (unsigned long)s.ms,
                       dos_xms_write_calls, dos_xms_written_bytes / 1024UL,
                       dos_xms_read_calls, dos_xms_read_bytes / 1024UL,
                       dos_xms_grown_bytes / 1024UL);
            }
#if DOS_INT13_OBS
            /* Retire the open INT 13h run and print the running totals, on the
             * same cadence. Without this the last run of a boot never prints:
             * the observer only retires a run when a *later* transfer breaks
             * it, and the interesting one -- the program image -- is usually
             * the last thing read. */
            dos_int13_obs_report();
#endif
            /* Alongside the prof line, on the same cadence and for the same
             * reason it is not behind a debug flag: an undersized pool, a stale
             * .dosmeta and a stale .xipimg all show up here and nowhere else,
             * and a diagnostic that has to be specially built is one nobody
             * runs. Silent when no window is armed, so a build without COW
             * costs one call and prints nothing. */
            dos_cow_log("run");
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

    /* Teardown. Reached only when the guest halts (the loop's `break` above);
     * a return to the launcher via the pause menu does not come through here.
     * Worth having anyway: it is the one reading taken after everything the
     * session was going to do has happened, and `lost` at teardown is the
     * number that decides whether the pool was big enough. */
    dos_cow_log("final");
    dos_xipsm_report();
}
