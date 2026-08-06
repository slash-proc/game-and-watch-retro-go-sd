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
#include "dos_mouse_ui.h"
#include "dos_cpu.h"
#include "dos_audio.h"
#include "dos_ospi_bench.h"
#include "gw_malloc.h"   /* ahb_calloc() */
#include "gw_flash_alloc.h" /* store_file_in_flash() -- DOS_XIP_CACHE */
#include "dos_meta.h"    /* the per-title COW pool sidecar (external/8086tiny) */
#include "dos_xms.h"     /* dos_xms_grown_bytes, and dos_pgf_install() -- see below */
#include "dos_zram.h"    /* dos_zram_pgf_lower() -- the COW pool's tier 2 */
#include "dos_xipsm.h"   /* the .xipimg state machine (external/8086tiny) */
#include "dos_fbs.h"     /* the FAST-BOOT SNAPSHOT container (external/8086tiny) */
#include "dos_perf.h"    /* bucketed cycle accounting, OFF by default (external/8086tiny) */
/* PER-TITLE USER SETTINGS, and note that this is NOT dos_meta.h above it. That
 * one is packaging-time CONTENT about a title, keyed on the .dsk's bytes and
 * correctly invalidated when the image is rebuilt; this one is what the USER
 * chose, keyed on the title's path so that rebuilding the image does not throw
 * it away. dos_settings.h has the full argument. */
#include "dos_settings_rg.h"
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
extern const unsigned int dos_fold_table_required;
extern void dos_fold_set_table(void *p);
/* The register file: regs8/regs16, read and written several times per emulated
 * instruction and the hottest data in the machine. Same hand-over shape as the
 * fold table and the AHB block -- 8086tiny.c exports the size so the porting
 * layer cannot drift from it, and falls back to calloc() (the retro-go heap) if
 * nobody hands a block over. That fallback exists for the ~40 host harnesses in
 * test286/; on the target taking it would silently move the hottest data in the
 * emulator out of zero-wait-state DTCM, which is why the call site checks. */
extern const unsigned int dos_regs_required;
extern int dos_regs_set_storage(void *p, unsigned int bytes);
/* THE COW POOL IS REGISTERED, NOT LINKED. Demand paging took guest
 * [0x10000,0xA0000) out of mem[] at the link (8086tiny.o BSS 790,934 ->
 * 238,824 B), so the backing store for a first store into that range has to
 * come back from the AXI those 589,824 bytes became -- and only this side of
 * the fence knows where that AXI is. A page costs 4,114 B and buys 4,096 B of
 * guest address space. Returns the number of pages it took, 0 if it refused. */
extern unsigned int dos_cow_pool_add(unsigned char *base, unsigned int bytes);
/* THE EGA RESERVE, WHICH IS ALSO THE POOL'S COLDEST TIER. One block, two jobs:
 * until a title sets a planar mode it is registered as pages the evictor can
 * demote into instead of writing to SD, and the moment a write to 0x3C2 says
 * "EGA" the emulator purges it and hands the WHOLE block -- page tables
 * included -- to the planes (dos_ega_cold_claim/dos_ega_cold_vram, from
 * dos_ega_set_active, which clears the planes anyway, so the metadata costs
 * nothing in the state this memory was reserved for).
 *
 * MUST BE CALLED AFTER EVERY dos_cow_pool_add() AND BEFORE dos_cpu_init():
 * dos_cow_cap is deliberately NOT advanced over the cold pages, which is what
 * keeps the hot allocator out of them, so a hot region registered afterwards
 * would be handed indices that overlap the tier. The emulator refuses that
 * outright rather than trusting the caller.
 *
 * Returns the pages it lends -- 0 under DOS_COW_EVICT=0, where there is no
 * victim hand to demote with, and where the block is still REMEMBERED so EGA
 * still gets its full 256 KB. The card lands either way; only the tier is
 * conditional. This build is DOS_COW_EVICT=1 (Makefile.common). */
extern unsigned int dos_cow_cold_add(unsigned char *base, unsigned int bytes);
/* 256 KB = four 64 KB planes, which is the whole EGA/VGA planar address space a
 * guest can reach through the A000 aperture -- not a budget, a completeness
 * figure, so it is a constant and not an option. As a cold tier the same block
 * yields 63 pages (262,144 / 4,114) with 2,962 B unused; that waste is the
 * price of the tier reusing the pool's carve instead of growing a second,
 * differently-shaped allocator. */
#define DOS_EGA_RESERVE_BYTES 262144u
extern unsigned short *regs16;
extern unsigned char *regs8;
extern unsigned char io_ports[];

extern int dos_cpu_init(int argc, char **argv);   /* 0 = ok, <0 = BIOS load failed */
extern int dos_cpu_frame(int cycles);

/* ---- The AXI SRAM the mapping fold actually gives back ---------------------
 *
 * NO MORE dos_mem_trim / dos_mem_trim_base. The trim shortened mem[] by
 * removing guest [base, 0x100000); demand paging removed guest
 * [0x10000, 0xA0000) instead, which is an order of magnitude more, so the trim
 * and the arena that funded its pool were deleted together. Those two constants
 * no longer exist in 8086tiny.c, and a stale reference here is a LINK ERROR by
 * design -- silently keeping a dead knob is the failure this project keeps
 * paying for (external/8086tiny/docs/traps.md).
 *
 * dos_mem_map_ro() survives, and is still the read-only form of dos_mem_map():
 * the granules it covers get the real pointer in the READ map and DOS_COW_TRAP
 * in the WRITE map. dos_xipsm.c is its only caller now. dos_cow_stats() is what
 * makes an undersized pool visible -- `lost` is the count of stores that had
 * nowhere to go, and it must be 0. */
extern int dos_mem_map_ro(unsigned int gbase, unsigned int len, unsigned char *host);
extern void dos_cow_stats(unsigned *faults, unsigned *pages_used,
                          unsigned *pool_pages, unsigned *lost);

/* ---- Per-title COW pool sizing --------------------------------------------
 *
 * The pool is one size for every title -- it is the AXI headroom, registered
 * below, and it cannot be per-title because it is handed over before anything
 * knows which title is about to run. external/8086tiny/dos_meta.h carries a
 * measured per-title page count from package time in a 64-byte `.dosmeta`
 * sidecar beside the `.dsk`; dos_cow_reserve() applies it, and everything the
 * reservation does not claim becomes the SPARE offered to EGA planes, page
 * flipping and the 4K decode cache.
 *
 * (The reclaim ARENA that used to fund the pool per-title out of a mach_kb hole
 * is gone. Its premise -- that a title's .dosmeta leaves a hole above the INT
 * 12h machine size -- was the part that kept failing, and a mach_kb swept on a
 * title that never launches measures the failure, not the title.)
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

/* ---- Per-title machine size (INT 12h): REMOVED -----------------------------
 *
 * `dos_mach_set_kb()`, `dos_mach_kb`, `dos_mach_applied` and `dos_mach_over`
 * are gone. The guest is told it has 640 KB, always, because that is what the
 * BIOS blob says and nothing lowers it any more.
 *
 * Do not reintroduce a per-title cap here. Three separate mechanisms have been
 * built on a measured machine size and all three were removed for the same
 * reason: the measurement cannot be trusted, and when it is wrong it caps the
 * guest below what the game needs and surfaces as an unrelated error. The
 * memory tiering makes the cap unnecessary. */
extern unsigned int dos_guest_hiwater;
extern unsigned int dos_cow_wedged(void);
extern void dos_cow_cold_stats(unsigned *pages, unsigned *used, unsigned *demotes,
                               unsigned *spills, unsigned *biased);
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
 * The cold half of the port does not live in RAM. It is linked at a sentinel
 * address instead, shipped as one file -- /cores/dos.xip -- cached into QSPI
 * NOR, and executed and read straight out of it. It started as three objects
 * (the CP437 glyphs, the OSK, the XMS driver) and is now eighteen; the
 * membership list and, more importantly, the POLICY that decides it live in the
 * .xip_dos comment in STM32H7B0VBTx_SDCARD.ld. Read that before adding a
 * build/dos/*.o anywhere.
 *
 * WHY, in bytes. .overlay_dos_bss holds mem[], 778 KB of guest RAM, and it
 * starts at the first 4 KB boundary after .overlay_dos's code and rodata. So
 * code removed from the overlay becomes guest memory -- but only in 4,096-byte
 * quanta, which is why objects move in GROUPS and not one at a time. First
 * measurement, when this landed: _OVERLAY_DOS_BSS_END 0x240fe010 -> 0x240fb018
 * against __RAM_EMU_END__ 0x24100000, i.e. 8,176 B of headroom -> 20,456 B.
 * That is what unblocked EGA 0Eh/0Fh/10h and the 4K decode cache
 * (external/8086tiny/docs/handover.md).
 *
 * Every byte of that has since been spent -- the INT 33h mouse driver, .dosset
 * per-title settings, DOS_SS_BIG_STACK and DOS_FOLD_DOMAIN at 8 MB. Current
 * link: _OVERLAY_DOS_BSS_END 0x240ff004, headroom 4,092 B, blob 40,672 B in a
 * 512 KB sentinel window. Do not quote the numbers above as if they were
 * today's; read them out of build/gw_retro_go.elf.
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
 * One thing that does NOT need handling here -- and the REASON changed once the
 * blob outgrew a single buffer. circular_flash_write() reads in 16 KB chunks
 * (gw_flash_alloc.c:302). The note here used to say the blob was ~10 KB so it
 * arrived in one call and no sentinel word could straddle a chunk boundary;
 * that stopped being true at 40,672 B, which is three chunks. It is still safe
 * for a different reason, the one that loop states about itself: `want` is
 * sizeof(buffer), a multiple of 4, except at end of file -- and the blob's own
 * size is 8-aligned by the ALIGN(8) in .xip_dos -- so no 32-bit word ever spans
 * two chunks. The `length & ~3u` truncation below is the belt for the odd tail
 * case, exactly as gba_relocate_xip() does it. */
#define DOS_CODE_BASE  0xDED00000u
#define DOS_XIP_PATH   "/cores/dos.xip"

static uint8_t *g_dos_xip_addr;
static uint32_t g_dos_xip_size;

/* skip_base: leave a word that is EXACTLY DOS_CODE_BASE alone. Set only for
 * main_dos.o's own region, which is where the defining constant lives. It must
 * stay 0 for the rest of the overlay: dos_video_blit legitimately references
 * __xip_dos_start__, i.e. base + 0, and that one has to be relocated. */
static int patch_dos_sentinels(uint32_t *start, uint32_t *end, int32_t offset, uint32_t size,
                               int skip_base)
{
    int patched = 0;
    for (uint32_t *p = start; p < end; p++) {
        uint32_t v = *p;
        if (skip_base && v == DOS_CODE_BASE)
            continue;
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
                        offset, file_size, 0);
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
                            offset, g_dos_xip_size, 0);

    /* ...and now main_dos.o's OWN region, which used to be scanned only to be
     * *rejected*. That was wrong, and it was already broken before anyone
     * noticed, because the link never got as far as running.
     *
     * The original reasoning was: main_dos.o defines DOS_CODE_BASE, a blind
     * scan cannot tell the defining constant from a reference to the blob, so
     * exclude the whole object and assert that ld happened to place the call
     * veneers just after it. Two things make that both unnecessary and unsafe:
     *
     *   - the constant IS distinguishable. DOS_CODE_BASE is the bare value
     *     0xDED00000; every genuine reference is base + a non-zero offset (or
     *     +1 for a Thumb pointer). The guard below has always relied on exactly
     *     that test -- it just used it to reject rather than to skip.
     *   - main_dos.c does not only *call* into the blob, it TAKES ADDRESSES
     *     into it: the odroid_dialog_choice_t option table is built here and
     *     holds &dos_settings_ssbig_update_cb, &dos_cpu_speed_update_cb and
     *     &dos_screen_freq_update_cb. Those land in main_dos.o's literal pool,
     *     not in a veneer, so no placement luck can bring them inside the old
     *     window. Verified on the merged tree at 18165bfa, before this branch
     *     moved anything: one stranded ref (0xded08439, the ssbig callback the
     *     per-title settings feature added). The core would have printed
     *     "FATAL 1 unrelocated xip refs" and returned to the launcher on every
     *     single launch.
     *
     * So patch this region too, skipping only the exact bare constant. The
     * blob is at 0x90xxxxxx once relocated, so a patched word can never fall
     * back inside the sentinel range and no word is visited twice.
     *
     * The one thing this DOES give up is the "a code word can never spell a
     * sentinel" argument, since main_dos.o's instructions are now scanned as
     * well. That argument is upheld statically instead: the sentinel's high
     * halfword lives in Thumb's permanently-undefined 0xDE00-0xDEFF space,
     * which gcc never emits, and scripts/check_xip_sentinels.py now scans the
     * whole of .overlay_dos rather than stopping at _DOS_MAIN_CODE_END. */
    n += patch_dos_sentinels((uint32_t *)__ram_emu_dos_start__,
                             (uint32_t *)_DOS_MAIN_CODE_END,
                             offset, g_dos_xip_size, 1);
    __DSB();
    __ISB();

    /* Belt and braces, and cheap: nothing but the bare constant may be left in
     * main_dos.o's region. If this ever fires again the pass has a hole in it,
     * and the alternative is a fault on first use with no clue why. Costs one
     * ~2.8 KB scan, once. */
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

/* ---- THE SHARED PAGEFILE: ONE HANDLE, TWO CLIENTS -------------------------
 *
 * external/8086tiny/docs/memory/22-one-pagefile-handle.md, "What is left".
 * This is the firmware half of that document, and until it existed the COW
 * pool's tier 2 had never executed on the device: dos_zram.c's lower tier is
 * complete, the 4 KB slot allocator in dos_xms.c is complete, XMS has already
 * been folded onto the same handle -- and nothing opened a file, so
 * dos_zram_pgf_lower() returned NULL and 8086tiny.c installed tier 1 only.
 *
 * WHY THE PORTING LAYER OWNS THE HANDLE AND NOT THE EMULATOR. MAX_OPEN_FILES
 * is 8 (Core/Src/syscalls.c:51) and the DOS core already holds THREE for its
 * whole run: disk[0] (hard disk), disk[1] (floppy), disk[2] (the BIOS blob),
 * all opened by dos_cpu_init(). Running out is SILENT -- every other fopen()
 * in the firmware starts returning NULL and callers read that as "asset
 * missing", so rg_i18n.c:245 falls back to the unknown glyph and the entire UI
 * renders as diamonds with nothing logged and nothing faulting. That is the
 * failure that kept tier 2 dark. The way out is that there is exactly ONE
 * file for both clients, it is opened HERE, once, before dos_cpu_init(), and
 * it is held for the run.
 *
 * THE NET HANDLE COUNT DOES NOT RISE. Before this change XMS opened its own
 * /dos_xms.swp lazily on the first extended-memory write (dos_xms.c:220), so a
 * title that used XMS -- which is every title booting MS-DOS 6.22 with
 * HIMEM.SYS in CONFIG.SYS -- was already at four. dos_pgf_install() makes XMS
 * a client of this handle instead and it never opens anything again, so the
 * count is 3 + 1 = 4 with tier 2 running, exactly what it was with tier 2
 * dark. See dos_xms.h, "ONLY USED WHEN NO SHARED PAGEFILE HAS BEEN INSTALLED".
 *
 * FAILURE IS NOT FATAL AND IS NOT SILENT. A missing card, a full card or a
 * read-only card means no tier 2: the pool keeps its compressed tier and its
 * cold tier and behaves exactly as this build did before, which is a working
 * configuration that has shipped. It says so on one line. What it must never
 * do is refuse to start a title, and it cannot -- nothing below consults the
 * return value.
 *
 * THE FILE IS SCRATCH. "w+b" is FA_CREATE_ALWAYS|FA_READ|FA_WRITE
 * (syscalls.c:93), i.e. it truncates on open, so a crash or a pause-menu exit
 * on the previous run cannot leave megabytes of stale swap to be inherited --
 * and the file is removed at teardown as well. Truncation is also what
 * dos_pgf_install(&io, 0) asserts by being passed a current length of 0.
 *
 * UNBUFFERED, AND THAT IS A CORRECTNESS REQUIREMENT rather than a tuning
 * choice. dos_xms.h's contract on these two callbacks is "position and
 * transfer, and must NOT buffer": the slot allocator hands out byte ranges and
 * relies on a write having landed before the matching read is issued, and a
 * stdio buffer would also cost BUFSIZ of heap for no benefit -- every transfer
 * here is already a whole 4 KB slot or a whole XMS move.
 *
 * The path is at the card root rather than under /saves/dos, for two reasons:
 * /saves/dos is created by the savestate machinery and may not exist at core
 * entry (dos_fbs_glue.c:268 handles exactly that case), and this file is not a
 * save -- it must not survive a run, let alone be backed up beside one. The
 * root is where XMS's private swap lived for the same reason.
 */
#define DOS_PGF_PATH "/dos_pgf.swp"

static FILE *dos_pgf_fp;

/* Both return NONZERO ON SUCCESS -- dos_xms.h's struct dos_pgf_io. A short
 * transfer is a failure, not a partial success: the caller has no way to
 * resume half a granule and counts the failure as a lost store, which is a
 * legitimate outcome the pool already handles. */
static int dos_pgf_read_cb(void *ctx, unsigned long off, void *buf, unsigned int len)
{
    /* THE SD LATENCY BUCKET, and the reason it is HERE rather than in
     * dos_pgf_read(): this is the only place in the pagefile path that actually
     * touches the card. dos_pgf_read() also serves reads past the end of the
     * file out of its own bookkeeping, and counting those as SD time would
     * make the tier-2 cost look larger than it is. */
    DOS_PERF_SCOPE(DOS_PB_PGFR);
    FILE *f = (FILE *)ctx;

    if (!f || fseek(f, (long)off, SEEK_SET) != 0)
        return 0;
    return fread(buf, 1, len, f) == len;
}

static int dos_pgf_write_cb(void *ctx, unsigned long off, const void *buf, unsigned int len)
{
    DOS_PERF_SCOPE(DOS_PB_PGFW);
    FILE *f = (FILE *)ctx;

    if (!f || fseek(f, (long)off, SEEK_SET) != 0)
        return 0;
    return fwrite(buf, 1, len, f) == len;
}

/* Call BEFORE dos_cpu_init(). Two orderings depend on it and both are silent
 * when broken:
 *
 *  - dos_cpu_init() is where 8086tiny.c's DOS_ZRAM_AUTO block runs, and that
 *    block asks dos_zram_pgf_lower() for the lower tier. No handle installed
 *    by then means NULL means tier 1 only for the whole run.
 *  - dos_xms_reset() runs in there too, and in standalone mode it remove()s
 *    dos_xms_store_path. With a handle installed it leaves the shared file
 *    alone (docs/memory/22-one-pagefile-handle.md step 2).
 */
static void dos_pgf_open(void)
{
    struct dos_pgf_io io;

    dos_pgf_fp = fopen(DOS_PGF_PATH, "w+b");
    if (!dos_pgf_fp) {
        /* Loud, because "the pool is quietly one tier smaller" is
         * indistinguishable from "tier 2 is working badly" in a cow log. */
        printf("DOS: pagefile %s could not be opened - COW tier 2 and XMS "
               "backing store are OFF for this run (pool keeps tiers 1 and 3)\n",
               DOS_PGF_PATH);
        return;
    }
    setvbuf(dos_pgf_fp, NULL, _IONBF, 0);

    io.read  = dos_pgf_read_cb;
    io.write = dos_pgf_write_cb;
    io.ctx   = dos_pgf_fp;
    dos_pgf_install(&io, 0);            /* 0 = we just truncated it */

    printf("DOS: pagefile %s open (handle 4 of %d; disks+BIOS take 3)\n",
           DOS_PGF_PATH, 8);
}

static void dos_pgf_close(void)
{
    if (!dos_pgf_fp)
        return;
    /* Uninstall before the close, so a late transfer from a teardown path
     * cannot reach a FILE* that has already gone. */
    dos_pgf_install(NULL, 0);
    fclose(dos_pgf_fp);
    dos_pgf_fp = NULL;
    remove(DOS_PGF_PATH);
}

/* ------------------------------------------------------------ guest XIP ---
 * Phase 3 of external/8086tiny/docs/memory/02-guest-xip.md: cache the whole
 * .dsk into the 64 MB of OSPI NOR that an SD_CARD=1 build leaves entirely
 * unused at runtime (docs/gwemu.md:194), and hand back the memory-mapped
 * pointer.
 *
 * ON BY DEFAULT SINCE LOAD ELISION WAS WIRED UP, and the reason it was off
 * before no longer holds. Phase 3 was specified as a pure observer, and a
 * multi-second "first boot of this image" stall bought nothing but a pointer
 * nobody dereferenced, so paying it by default would have been a behaviour
 * change with no benefit. There is now a consumer: dos_elide_set_image()
 * below. Elision maps a guest granule straight at the NOR copy instead of
 * spending a COW pool page on bytes that came off the disk unmodified, and
 * WITHOUT THIS CACHE THERE IS NO IMAGE TO MAP AT -- elision compiles in,
 * declines everything and reports zeros. So the stall is now the entry price
 * for pool pages the demanding titles do not have (WOLF3D wants ~156 against
 * 67 hot + 63 cold, and drops stores it cannot place).
 *
 * The stall is paid ONCE PER IMAGE, not once per launch:
 * store_file_in_flash() recognises a file already resident in NOR and returns
 * its address. Turn it back off with DOS_CFLAGS_EXTRA=-DDOS_XIP_CACHE=0
 * (which is NOT a make dependency -- touch the file, see docs/testing.md
 * section 4); that is the negative control for any elision measurement.
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
#define DOS_XIP_CACHE 1
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

/* LOAD ELISION. Declared here rather than pulled from a header because
 * 8086tiny.c exports it without one (test286/host_main.c:297 declares it the
 * same way). "The bytes of this guest granule are at offset X of that image",
 * recorded on an INT 13h read that filled a whole aligned granule from a
 * sector run -- so the granule can be mapped at NOR and cost no COW pool page
 * at all until the guest writes to it.
 *
 * THE DRIVE INDEX IS NOT DECORATION. 8086tiny.c compares the FILE* of every
 * INT 13h transfer against disk[dos_elide_drive] and declines anything that
 * does not match (8086tiny.c:6144). Hand it the wrong index and elision
 * silently declines every read for the whole run, which looks exactly like
 * elision being useless rather than mis-wired. disk[0] is the hard disk,
 * disk[1] the floppy, disk[2] the BIOS image (8086tiny.c:1856), and which of
 * the first two this title is depends on the argv slot chosen below from
 * st.size. */
extern void dos_elide_set_image(const unsigned char *base, unsigned int len, int drive);

static void dos_xip_cache(const char *path, uint32_t known_size, int drive)
{
    uint32_t t0 = HAL_GetTick();

    dos_xip_size = 0;   /* 0 = "whole file"; the cache fills it in */
    dos_xip_base = store_file_in_flash(path, &dos_xip_size, false, &dos_xip_progress);

    if (dos_xip_base == NULL) {
        /* Expected, not exceptional, and NOT A REASON TO REFUSE THE TITLE --
         * that mistake has been made in this file before and BATTLECHESS would
         * not launch. BATTLECHESS.dsk is 66,060,288 bytes against a 64 MB part,
         * so it is uncacheable BY CONSTRUCTION and no amount of free flash will
         * change that; find_write_slot() (gw_flash_alloc.c:144) also answers
         * false for anything that cannot fit clear of the files already live
         * this boot. Either way the guest runs from SD with no elision, exactly
         * as it did before this was switched on -- 02-guest-xip.md section 5,
         * "the one new obligation".
         *
         * The size is in the log on purpose: "no elision" and "elision working
         * badly" are indistinguishable in a cow log, so the reason has to be
         * stated where it happens. */
        printf("DOS: xip NOT cached (%s, %lu bytes, too big or no room) - "
               "guest runs from SD, LOAD ELISION OFF for this title\n",
               path, (unsigned long)known_size);
        return;
    }
    printf("DOS: xip .dsk at %p, %lu bytes, %lu ms\n", (void *)dos_xip_base,
           (unsigned long)dos_xip_size, (unsigned long)(HAL_GetTick() - t0));

    /* AND HAND IT OVER. Until this call existed, dos_elide_set_image() was
     * reached from exactly one place in the tree -- test286/host_main.c's
     * DOS_ELIDE_IMAGE -- so load elision had never executed on the device
     * despite being complete and having three host tests (runelide, runelide2,
     * runelide3).
     *
     * BEFORE dos_cpu_init(), which the call site below guarantees, and that is
     * an ordering constraint rather than tidiness: this function clears the
     * whole per-granule offset table and the NOR residency bitmap, and
     * dos_cpu_init() is what rebuilds the fold map afterwards. Handing the
     * image over after init would wipe entries the rebuilt map still refers
     * to. */
    dos_elide_set_image(dos_xip_base, dos_xip_size, drive);
    printf("DOS: load elision armed on disk[%d] (%s)\n",
           drive, drive == 0 ? "hard disk" : "floppy");
}
#endif

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
 * program is. It matches 8086tiny.c's demand region, which is now fixed at
 * [0x10000, 0xA0000) rather than a pair of make flags. */
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
 * dos_paging.c is in `.xip_dos` rather than the overlay (see the comment in
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
 * for what is mapped. The fix is no longer a make flag: raise the page count
 * dos_cow_pool_add() is called with in app_main_dos(), at 4,114 B a page out of
 * the AXI headroom that call already prints. */
/* 8086tiny.c exports these without a header (test286/host_main.c declares them
 * the same way). Both arms are defined -- the DOS_ELIDE=0 arm returns zeros --
 * so this call site does not need a build-flag guard. */
extern void dos_elide_stats(unsigned int *mapped, unsigned int *declined,
                            unsigned int *replays, unsigned int *checked,
                            unsigned int *mismatch);

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

    /* THE HEADLINE, AND IT IS TELEMETRY -- NOTHING READS IT.
     *
     * "How much memory is the guest using, how much of it is hot, and where do
     * the cold pages live." That is the whole question this line answers, and
     * answering it is now the ONLY thing a memory measurement does here: no
     * per-title profile influences how the emulator starts or how much memory a
     * guest is offered. If you find yourself wanting to feed one of these
     * numbers back into a sizing decision, read the tombstone on mach_kb_DEAD
     * in external/8086tiny/dos_meta.h first.
     *
     * `used` is dos_guest_hiwater -- one past the highest conventional granule
     * the guest has ever taken a first store in, i.e. its real footprint rather
     * than what DOS offered it. `hot` is what is resident in the pool right now;
     * everything else is in one of the cold tiers, and the tier counters say
     * which. A wedged pool is called out by name because a counter alone has
     * been shown to sit unread behind a guest that still boots. */
    {
        unsigned zp = 0, zb = 0, zs = 0, zr = 0, zl = 0;
        unsigned cpages = 0, cused = 0, cdem = 0, cspill = 0, cbias = 0;

        dos_zram_stats(&zp, &zb, &zs, &zr, &zl);
        dos_cow_cold_stats(&cpages, &cused, &cdem, &cspill, &cbias);
        printf("DOS: mem used=%uK hot=%u/%u cold=%u zram=%u(%uB) pgf=%u%s\n",
               (unsigned)(dos_guest_hiwater >> 10), pages, pool, cused,
               zp, zb, zl,
               dos_cow_wedged() ? "  ** POOL WEDGED: stores discarded **" : "");
    }

    /* THE TWO SUBSYSTEMS THAT WERE DARK, ON THE SAME CADENCE AND FOR THE SAME
     * REASON THE COW LINE IS NOT BEHIND A DEBUG FLAG. Neither of these can be
     * evaluated from the cow line alone:
     *
     *  - Load elision. `mapped` is granules served straight out of NOR, i.e.
     *    pool pages not spent; `declined` is reads that reached the hook and
     *    were refused (unaligned, wrong drive, image stale after a disk write,
     *    no pool to serve a later COW fault). mapped=0 with declined climbing
     *    is mis-wiring -- most likely the drive index -- and mapped=0 with
     *    declined=0 means the hook is not being reached at all. `mismatch` MUST
     *    be 0: it is DOS_ELIDE_VERIFY comparing the mapped bytes against what
     *    the INT 13h copy actually delivered, and a nonzero value is the offset
     *    arithmetic being wrong, which is the one failure here that costs
     *    correctness rather than pages.
     *  - The pagefile. `lower` in the zram line counts granules that went to
     *    tier 2, which is the number that says whether opening the handle
     *    bought anything. len is the live set, not the total ever evicted:
     *    slots are freed on page-in.
     *
     * Both print unconditionally so that "off", "on and doing nothing" and "on
     * and working" are three distinguishable readings rather than one silence.
     */
    {
        unsigned emapped = 0, edecl = 0, erepl = 0, echk = 0, emis = 0;
        unsigned zpages = 0, zbytes = 0, zstored = 0, zrej = 0, zlower = 0;
        unsigned long pwr = 0, prd = 0, pzero = 0, plen = 0;
        unsigned pwrc = 0, prdc = 0, pslots = 0;

        dos_elide_stats(&emapped, &edecl, &erepl, &echk, &emis);
        dos_zram_stats(&zpages, &zbytes, &zstored, &zrej, &zlower);
        dos_pgf_stats(&pwr, &prd, &pzero, &pwrc, &prdc, &pslots, &plen);
        printf("DOS: elide %s mapped=%u declined=%u replays=%u checked=%u "
               "mismatch=%u | zram pages=%u bytes=%u stored=%u rejected=%u "
               "lower=%u | pgf %s len=%luB slots=%u w%u/%luB r%u/%luB zero=%luB\n",
               when, emapped, edecl, erepl, echk, emis,
               zpages, zbytes, zstored, zrej, zlower,
               dos_pgf_ready() ? "on" : "OFF", plen, pslots,
               pwrc, pwr, prdc, prd, pzero);
    }
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
    /* THE PER-TITLE MACHINE SIZE IS GONE, AND NOTHING REPLACES IT.
     *
     * This used to read `mach_kb` out of the .dosmeta sidecar and hand it to
     * dos_mach_set_kb(), which patched the BIOS table so INT 12h reported LESS
     * than 640 KB of conventional memory to the guest. Every title got whatever
     * a profiling run had once measured.
     *
     * It is deleted because the premise was wrong in both directions. A profile
     * taken on a title that did not launch measures the FAILURE and then caps
     * the guest below what the game needs -- which presents as an EMS error,
     * nowhere near its cause. And a profile that was right on the day is still
     * a number nobody can re-derive reliably, because truly profiling a DOS
     * game's memory use is not a thing this project can do.
     *
     * The tiering is what makes the number unnecessary: the guest is offered
     * everything, and the pool / LZ4 / pagefile ladder backs whatever it
     * actually touches. Running out degrades to SLOW, not to WRONG.
     *
     * Nothing measured about a title may influence how the emulator starts.
     * See dos_meta.h's tombstone on the struct field, which is retained only so
     * the on-card format does not change. */

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

    /* The fold map: the read/write offset table that dos_fold() indexes on
     * EVERY guest memory access. It was in .overlay_dos_bss (AXI SRAM,
     * multi-cycle); putting it in DTCM (zero-wait-state) removes the wait
     * states from the hottest load in the interpreter.
     *
     * BEFORE THE AHB BLOCK, AND THAT IS LOAD-BEARING: dos_mem_set_ahb() calls
     * dos_fold_map_build(), which writes to this table. If the table pointer is
     * NULL that is a wild store into address zero.
     *
     * dtcm_calloc rather than dtcm_malloc: the table must be zeroed before
     * dos_fold_map_build() is called, the same contract as the AHB block. The
     * size is domain-dependent (currently 8 KB at 4 MB domain, interleaved),
     * exported by 8086tiny.c so the two cannot drift apart. */
    if (dos_fold_table_required) {
        void *ft = dtcm_calloc(1, dos_fold_table_required);
        printf("DOS: fold table %u bytes from DTCM at %p\n",
               (unsigned)dos_fold_table_required, ft);
        dos_fold_set_table(ft);
    }

    /* The register file, same hand-over as the fold table above and for the same
     * reason: regs8/regs16 are touched several times per emulated instruction,
     * and in AXI SRAM every one of those is a multi-cycle access. It is only
     * dos_regs_required bytes (tens, not kilobytes -- the file is 8086 registers
     * plus flags, not a cache), so the DTCM budget question the fold table has
     * does not arise here.
     *
     * dtcm_calloc, not dtcm_malloc: the block must arrive ZEROED. 8086tiny.c
     * resets exactly two bytes of it on every boot (REG_ZERO and the always-zero
     * flag slot) and takes the rest on trust, which is the same contract the
     * fold table has and the same one a stale DTCM block would break.
     *
     * CHECKED, NOT ASSUMED: dos_regs_set_storage() returns 0 and changes nothing
     * if the block is short or not 8-aligned -- refusing beats a misaligned
     * `unsigned short *`, which faults on this target. A silent refusal would
     * leave the emulator on its calloc() fallback, i.e. the hottest data in the
     * core sitting in the retro-go heap with nothing in the log to say so. */
    if (dos_regs_required) {
        void *rf = dtcm_calloc(1, dos_regs_required);
        if (!rf || !dos_regs_set_storage(rf, dos_regs_required)) {
            printf("DOS: WARNING regs %u bytes NOT in DTCM (%p) - falling back to heap\n",
                   (unsigned)dos_regs_required, rf);
        } else {
            printf("DOS: regs %u bytes from DTCM at %p\n",
                   (unsigned)dos_regs_required, rf);
        }
    }

    /* ---- THE COW POOL, AND ITS POSITION IS LOAD-BEARING ---------------------
     *
     * BEFORE dos_cpu_init() (which binds the pool and would otherwise refuse
     * with DOS_INIT_ENOPOOL) and BEFORE dos_cow_reserve() further down (which
     * applies the .dosmeta per-title reservation and needs a capacity to apply
     * it to). Getting this order wrong cost a debug cycle already.
     *
     * WHERE THE MEMORY COMES FROM. Demand paging removed guest
     * [0x10000,0xA0000) from mem[] at the link, and those 589,824 bytes surfaced
     * as AXI headroom between the top of the DOS overlay's BSS and the top of
     * RAM_EMU. That interval is the pool.
     *
     * NOT `mem + dos_mem_required`, WHICH IS WHAT THIS WAS FIRST WRITTEN AS.
     * mem[] is NOT the last object in .overlay_dos_bss -- verified against
     * build/gw_retro_go.map on this very build: mem is 0x240451c4 + 0x30004 and
     * ends at 0x240751c8, while _OVERLAY_DOS_BSS_END is 0x2407b47c, so that
     * expression would have handed the pool ~25 KB of somebody else's live BSS
     * and then memset the owner table over the middle of it. The only correct
     * base is the linker symbol, which is why it is now in gw_linker.h.
     *
     * 96 PAGES WAS THE REQUEST AND NO LONGER FITS. The EGA reserve below takes
     * 262,144 B out of the same interval, and the two do not both fit:
     *
     *     headroom   __RAM_EMU_END__ 0x24100000
     *              - _OVERLAY_DOS_BSS_END 0x2407c4b4   = 539,468 B
     *     EGA                                          - 262,144 B
     *                                                  = 277,324 B for the pool
     *     277,324 / 4,114                              = 67 pages (275,638 B)
     *
     * (96 x 4,114 = 394,944, and 394,944 + 262,144 = 657,088 > 539,468. The
     * clamp below would have silently cut the pool to 277,324 B anyway; making
     * it 67 explicit is the difference between a decision and an accident.)
     *
     * WHY THE POOL IS THE ONE THAT GIVES. A plane size is not negotiable -- the
     * card is 256 KB or it is the shipped 32 KB, there is nothing in between
     * that a title asks for -- whereas the pool degrades smoothly, and it
     * degrades into the very block being taken from it. The measured ladder is
     * 96 pages = 0 evictions, 48 = 24, 40 = 228, 32 = 12,472 (39,889 stores
     * lost, memory diverged); 67 sits above 48, so the expected cost is under
     * two dozen evictions, and 63 of them land in the cold tier rather than on
     * SD -- at 22 of 44 hot pages the measurement was 22 demotions and ZERO
     * evictions to the backing store, i.e. the tier replaced SD traffic rather
     * than deferring it. Net capacity is 67 hot + 63 cold, not 67.
     *
     * If the headroom ever shrinks further the request is CLAMPED rather than
     * overrunning -- an assert here would trade a real title for a tidy
     * invariant, and dos_cow_stats()'s `lost` is what makes a short pool
     * visible in the log.
     *
     * The block is NOT zeroed by hand: dos_cow_pool_add() memsets the owner
     * table itself, precisely because a registered block carries no promise. */
    {
        unsigned char *pool_base = (unsigned char *)_OVERLAY_DOS_BSS_END;
        /* THROUGH uintptr_t, NOT POINTER ARITHMETIC ON &__RAM_EMU_END__. The
         * linker symbol has a one-element array type, so `&__RAM_EMU_END__ -
         * 262144` made gcc 15.2 emit "array subscript -65536 is outside array
         * bounds of 'uint32_t[1]'" -- and a -Warray-bounds hit here is not
         * cosmetic: the same object-size reasoning that produced the diagnostic
         * is entitled to fold the `ega_base < pool_base` sanity check below to
         * a constant, deleting the one thing standing between a bad base and
         * the launcher's memory. Laundering the address through an integer is
         * what stops the compiler reasoning about an "object" that is really
         * just an address. */
        uintptr_t      ram_top   = (uintptr_t)&__RAM_EMU_END__;
        unsigned char *ram_lim   = (unsigned char *)ram_top;
        /* THE EGA RESERVE IS PINNED TO THE TOP OF RAM_EMU, and the pool grows up
         * underneath it. Derived from the linker symbol, exactly as pool_base
         * is, and for the same reason -- see the mem[] note above; a base
         * computed from an object inside .overlay_dos_bss is not a base, it is
         * a coincidence. Anchoring the fixed-size block to the fixed end of the
         * region and letting the elastic one clamp also means a future BSS
         * growth costs pool pages, never a corrupted 256 KB card. */
        unsigned char *ega_base  = (unsigned char *)(ram_top - DOS_EGA_RESERVE_BYTES);
        unsigned char *pool_lim  = ega_base;
        unsigned int   avail     = (pool_lim > pool_base)
                                 ? (unsigned int)(pool_lim - pool_base) : 0u;
        unsigned int   want      = 67u * 4114u;
        unsigned int   pages, cold;

        if (want > avail)
            want = avail;
        pages = dos_cow_pool_add(pool_base, want);
        printf("DOS: cow pool %u pages, %u of %u B at %p (limit %p)\n",
               pages, want, avail, (void *)pool_base, (void *)pool_lim);
        /* The one thing that must not happen silently: the block running past
         * the top of RAM_EMU into whatever the launcher keeps there. */
        if (pool_base + want > pool_lim)
            printf("DOS: FATAL cow pool ends %p past RAM_EMU end %p\n",
                   (void *)(pool_base + want), (void *)pool_lim);

        /* ---- THE EGA 256 KB RESERVE, HANDED OVER LAST ----------------------
         *
         * LAST on purpose: after every dos_cow_pool_add() and before
         * dos_cpu_init(). See the declaration -- the emulator refuses a hot
         * region registered after the cold one, because the cold pages sit
         * above dos_cow_cap and a later hot region would be given overlapping
         * indices. Do not move this above the pool registration "for
         * symmetry".
         *
         * The same not-silently rule as the pool: if this block ever ends past
         * the top of RAM_EMU the planes would be handed launcher memory, and
         * an EGA title would corrupt it a plane clear at a time with nothing
         * in the log. It cannot happen while the base is derived by
         * subtraction from ram_lim, which is exactly why it is -- but the
         * pool's own base was also "obviously" right once (mem + required) and
         * was ~25 KB wrong, so the check is cheap insurance, not decoration. */
        if (ega_base < pool_base || ega_base + DOS_EGA_RESERVE_BYTES > ram_lim) {
            printf("DOS: FATAL EGA reserve %p..%p outside %p..%p - NOT registered\n",
                   (void *)ega_base, (void *)(ega_base + DOS_EGA_RESERVE_BYTES),
                   (void *)pool_base, (void *)ram_lim);
        } else {
            cold = dos_cow_cold_add(ega_base, DOS_EGA_RESERVE_BYTES);
            /* cold == 0 is NORMAL, not an error: it is what a DOS_COW_EVICT=0
             * build reports, and the 256 KB is remembered for EGA either way.
             * Logged as a number rather than a verdict for that reason. */
            printf("DOS: EGA reserve %u B at %p..%p, cold tier %u pages\n",
                   (unsigned)DOS_EGA_RESERVE_BYTES, (void *)ega_base,
                   (void *)(ega_base + DOS_EGA_RESERVE_BYTES), cold);
        }
    }

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
    /* ...and this is ALSO the elision drive index, which is why it is a named
     * variable now. dos_cpu_init() opens argv[3] as disk[0] (hard disk) and
     * argv[2] as disk[1] (floppy) -- 8086tiny.c:13017 -- and the elision code
     * matches transfers by FILE* against disk[dos_elide_drive]. Deriving the
     * index from the same `if` that chooses the argv slot is what keeps the two
     * from drifting; a hard-coded 0 would decline every read of a floppy title
     * and say nothing about it. */
    int dos_disk_index;
    if (st.size <= 2880 * 1024) {
        argv[2] = (char *)ACTIVE_FILE->path; // Floppy disk
        argv[3] = NULL;
        argc = 3;
        dos_disk_index = 1;                  // disk[1]
    } else {
        argv[2] = NULL; // No floppy
        argv[3] = (char *)ACTIVE_FILE->path; // Hard disk
        argv[4] = NULL;
        argc = 4;
        dos_disk_index = 0;                  // disk[0]
    }

#if DOS_XIP_CACHE
    /* Before dos_cpu_init(), for two reasons and both of them are load-bearing.
     *
     * The fopen budget: the cache opens the .dsk itself, and dos_cpu_init()
     * then holds three FILE* for the whole run. MAX_OPEN_FILES is 8
     * (Core/Src/syscalls.c:51) and running out fails SILENTLY -- see the fopen
     * entry in ../../../../CLAUDE.md. Doing the cache first keeps its handle
     * transient.
     *
     * And the elision hand-over inside it: dos_elide_set_image() clears the
     * per-granule table, while dos_cpu_init() is what rebuilds the fold map.
     * Ordering is the recurring bug in this function -- everything the emulator
     * has to be told is told before init, and dos_cow_cold_add() stays last of
     * the pool registrations. */
    dos_xip_cache(ACTIVE_FILE->path, (uint32_t)st.size, dos_disk_index);
#else
    (void)dos_disk_index;
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

    /* PER-TITLE USER SETTINGS, and BEFORE dos_cpu_init() for a concrete reason:
     * dos_ss_big_cfg is read on the segment-load path, and dos_cpu_init() runs
     * guest-visible BIOS setup. A setting applied afterwards would take effect
     * at whatever moment the guest next reloaded SS, which is exactly the kind
     * of half-applied state that makes a working feature look intermittent.
     *
     * There is no failure path. A missing card, a missing file and a corrupt
     * file all mean "defaults" and all say which; none of them stops a title.
     * The fopen is transient -- it is closed before dos_cpu_init() takes its
     * three long-lived handles out of MAX_OPEN_FILES = 8. */
    /* THE PER-GAME .cfg, and BEFORE dos_settings_rg_load() because that is the
     * layering: the .cfg supplies this title's DEFAULTS, and load() then resets
     * onto them and overlays whatever the user changed by hand. The reverse
     * order would let a file on the card overrule the user's own menu choices.
     *
     * Also before dos_cpu_init(), like everything else in this block, and for
     * the same two reasons: dos_ss_big_cfg is read on the segment-load path,
     * and the EMS reservation is claimed inside dos_cpu_init() itself. */
    dos_cfg_rg_load(ACTIVE_FILE->path);
    dos_settings_rg_load(ACTIVE_FILE->path);
    dos_settings_rg_apply();

    /* NO TRIM ARMING HERE ANY MORE, AND NO REFUSAL FOR IT. The trim is gone
     * entirely -- demand paging reclaims an order of magnitude more without
     * needing a cached .dsk to serve the tail from, so a title can no longer
     * fail to start because its image was too large to cache, which is what
     * BATTLECHESS.dsk would have done. */
    /* THE SHARED PAGEFILE. Last thing before dos_cpu_init(), because that is
     * where 8086tiny.c's DOS_ZRAM_AUTO block asks dos_zram_pgf_lower() for the
     * COW pool's tier 2 and where dos_xms_reset() decides whether it owns a
     * swap file of its own. See dos_pgf_open() for why the handle lives here
     * and not in the emulator, and for the MAX_OPEN_FILES accounting. */
    dos_pgf_open();

    int init_rc = dos_cpu_init(argc, argv);
    if (init_rc != 0) {
        /* -4 is DOS_INIT_ENOPOOL, and it gets its own line because it is a
         * WIRING error with an exact remedy, not a missing user file. It means
         * the dos_cow_pool_add() above returned no pages, so guest
         * [0x10000,0xA0000) has no backing store at all -- every first store
         * would be served from the scratch page and lost, counted in
         * dos_cow_lost and visible nowhere else. That silence is exactly the
         * failure mode 8086tiny.c added this return code to kill; do not fold
         * it back into the generic message. */
        if (init_rc == -4)
            printf("DOS: no COW pool registered (%d) - AXI headroom above "
                   "_OVERLAY_DOS_BSS_END is gone; see dos_cow_pool_add() above\n",
                   init_rc);
        else
            printf("DOS: BIOS load failed (%d) - expected %s\n", init_rc, dos_bios_path);
        return;
    }

    /* FAST-BOOT SNAPSHOT, and AFTER dos_cpu_init() is the whole design: init
     * has opened the disks, loaded the BIOS blob and built the decode tables --
     * all of which a restore needs and none of which a snapshot carries -- and
     * has not run one guest instruction. See the block above dos_fbs_boot(). */
    /* THE KEY'S MACHINE-SIZE FIELD IS NOW ALWAYS 0. It exists so the on-card
     * .fbs header keeps its layout; it no longer distinguishes anything,
     * because no title gets a per-title machine size any more. Passing 0 also
     * INVALIDATES every snapshot captured under the old per-title sizes, which
     * is the correct outcome -- those were captured on a machine that reported
     * a different amount of conventional memory than this one does. */
    dos_fbs_boot(ACTIVE_FILE->path, (unsigned long)st.size, dos_fbs_boot_key,
                 0ul);
    
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
    /* The first PER-TITLE setting, and the only one so far: the rest of the ids
     * in dos_settings.h are persisted but have no row and nothing reading them.
     * Unlike the two rows above it, this one is stored per title in a .dosset
     * beside the title's saves, and it takes effect at the next launch. */
    char dos_settings_ssbig_value[DOS_SETTINGS_VALUE_LEN];
    odroid_dialog_choice_t options[] = {
        ODROID_DIALOG_CHOICE_SEPARATOR,
        {200, "CPU speed",      dos_cpu_speed_value,       1, &dos_cpu_speed_update_cb},
        {201, "Screen Freq Hz", dos_screen_freq_value,     1, &dos_screen_freq_update_cb},
        {202, "32-bit stack",   dos_settings_ssbig_value,  1, &dos_settings_ssbig_update_cb},
        ODROID_DIALOG_CHOICE_LAST};
    /* Populate the value strings before the menu can be opened. */
    dos_cpu_speed_update_cb(&options[1], ODROID_DIALOG_INIT, 0);
    dos_screen_freq_update_cb(&options[2], ODROID_DIALOG_INIT, 0);
    dos_settings_ssbig_update_cb(&options[3], ODROID_DIALOG_INIT, 0);

    dos_cpu_speed_init();

    /* AFTER dos_cpu_speed_init(), which is where common_emu_enable_dwt_cycles()
     * is called. dos_perf_init() re-arms DWT itself and prints a live/dead
     * verdict on the counter, so the ordering is belt and braces rather than a
     * dependency -- but a table of zeros with no explanation is exactly the
     * silent failure this project keeps paying for, and the verdict line is the
     * positive indicator that rules it out.
     *
     * SystemCoreClock, not a constant: the PLL is user-selectable (280 MHz
     * stock, ~354 overclocked) and a hardcoded 340 would misreport every unit
     * that is not at 340. Expands to nothing unless DOS_PERF=1. */
    dos_perf_init((unsigned int)(SystemCoreClock / 1000000u));

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
        /* Folds the free-running 32-bit DWT counter into a 64-bit software
         * clock while the interval is ~5.6 M ticks. This is what makes the
         * reporting period free of the 12.6 s CYCCNT wrap -- see dos_perf.h.
         * Two loads, a subtract and a 64-bit add, once per frame; nothing at
         * all unless DOS_PERF=1. */
        dos_perf_frame();
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

        /* THE BUCKET REPORT, on its OWN cadence and not the prof line's.
         *
         * The device log ring is 4 KB and WRAPS TO INDEX 0 (Core/Src/main.c:94)
         * -- it is not a true ring, so anything printed too often erases the
         * evidence before `gnwmanager monitor` can read it. The perf line is
         * longer than the prof line, so it gets a longer period, and the period
         * is a knob rather than a constant because the right value depends on
         * what is being measured: a boot needs a short one, a TOPBENCH loop
         * wants a long one.
         *
         * DOS_PERF_PERIOD is in FRAMES. 256 is ~4.3 s at 60 Hz, which fits
         * roughly eight reports in the ring. Override with
         * DOS_CFLAGS_EXTRA="-DDOS_PERF=1 -DDOS_PERF_PERIOD=64".
         *
         * Compiles to nothing at all unless DOS_PERF=1: dos_perf_report() is
         * `((void)0)` and the compiler drops the test with it. */
#if DOS_PERF
#ifndef DOS_PERF_PERIOD
#define DOS_PERF_PERIOD 256
#endif
        if ((dbg_frames % (unsigned)DOS_PERF_PERIOD) == 0)
            dos_perf_report();
#endif

        if (!lcd_is_swap_pending() && drawFrame) {
            /* Bracketed for the cpu/blit/idle split (docs/video/09-video-profiling.md).
             * Note this only runs when common_emu_frame_loop() said so: a dropped
             * frame skips the blit entirely, which preserves guest speed at the
             * cost of visual smoothness -- so blit cost per *frame* and blit cost
             * per *blit* are different numbers, and both are reported. */
            dos_prof_blit_begin();
            dos_blit();
            /* The synthesised mouse pointer, between the guest image and the
             * OSK bars. It owns no rows of its own: it draws INTO rows 20-219,
             * over whatever dos_blit() just put there, which is why it has to
             * come after. Nothing to undraw -- dos_blit() is a full repaint of
             * those rows every painted frame, so a pointer never survives into
             * the next one and switching it off leaves no smear
             * (dos_mouse_ui.c says what would break that). */
            dos_mouse_ui_draw((uint8_t *)lcd_get_active_buffer());
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
    /* The pagefile is scratch and can be megabytes. Deleting it here covers the
     * guest-halt exit; the "w+b" truncation in dos_pgf_open() covers every
     * other way out of this core (the pause menu does not come through here),
     * so the card never carries a stale swap into the next session. */
    dos_pgf_close();
}
