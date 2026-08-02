// dos_fbs_glue.c -- the firmware half of the FAST-BOOT SNAPSHOT.
//
// Read external/8086tiny/dos_fbs.h first, and §1 of it before anything else:
// this is NOT a memory-reclaim mechanism. It arms no window, rewrites no fold
// entry and installs no copy-on-write trap. A restore is a straight decompress
// into ordinary writable guest RAM. It buys BOOT TIME and zero bytes of AXI.
//
// WHY THIS IS A TRANSLATION UNIT OF ITS OWN, and it is the same reason
// dos_arena.c is one. main_dos.o is EXCLUDED from `.xip_dos` in
// STM32H7B0VBTx_SDCARD.ld -- ITCM->flash veneers fail on arm-none-eabi 15.x for
// the objects that section takes -- so anything written inside main_dos.c is
// charged to `.overlay_dos`, i.e. to AXI, i.e. to guest RAM in 4 KB quanta.
// This driver was written inline there first and OVERFLOWED THE LINK against
// 1,200 B of headroom. Here it is cold code in external flash and costs nothing.
//
// Everything below runs at most twice a session: once to restore, before the
// guest's first instruction, and once to capture, at one frame boundary.

/* ==========================================================================
 * FAST-BOOT SNAPSHOT -- boot straight into the game.
 *
 * external/8086tiny/dos_fbs.h is the design; read §1 there first, because the
 * one thing this must not be mistaken for is a memory-reclaim mechanism. It
 * arms no window, rewrites no fold entry and installs no COW trap. A restore is
 * a straight decompress into ordinary writable guest RAM. It buys BOOT TIME and
 * ZERO BYTES of AXI.
 *
 *   first run of a title   boot as always; at DOS_FBS_AT_FRAMES write
 *                          /saves/dos/<title>.fbs (plus the XMS pool beside it)
 *                          and carry on. Nothing about the run changes.
 *   every run after        dos_cpu_init() as always -- it opens the disks,
 *                          loads the BIOS blob and builds the decode tables,
 *                          none of which a snapshot carries -- then restore,
 *                          and the guest's next instruction is the one after
 *                          the capture point. DOS never boots.
 *
 * THE DECOMPRESSOR IS FREE HERE. LzmaDecode() is already linked into internal
 * flash for the other cores, so wiring it costs one thunk and no XIP blob.
 * ========================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "main.h"                 /* HAL_GetTick, wdog_refresh */
#include "gw_flash_alloc.h"       /* store_file_in_flash() */
#include "dos_fbs.h"              /* the container (external/8086tiny) */
#include "dos_xms.h"              /* dos_xms_store_path -- the pool file, §7 */

#ifndef DOS_FBS_ENABLE
#define DOS_FBS_ENABLE 1
#endif

#if DOS_FBS_ENABLE
#include "lzma.h"

/* Where a capture is taken, in frames. Deliberately LATER than the .xipimg
 * marks: a base image only has to be settled, a savestate has to be somewhere
 * worth resuming. 1800 frames is ~30 s at 60 Hz. Frames and not milliseconds,
 * for the reason 04-xip-execute.md §3.3 gives: a slow frame is more guest work,
 * not less, and an interval containing no work produces a state that is
 * unearned. */
#ifndef DOS_FBS_AT_FRAMES
#define DOS_FBS_AT_FRAMES 1800u
#endif

static char     dos_fbs_path[128];
static uint8_t *dos_fbs_flash;
static uint32_t dos_fbs_flash_size;
static int      dos_fbs_restored;
static int      dos_fbs_captured;
static dos_fbs_key_t dos_fbs_key;

/* The XMS pool. dos_fbs.h §7: the snapshot carries the driver's handle table,
 * the CALLER carries the pool file. MS-DOS 6.22's own CONFIG.SYS takes 102 KB
 * of XMS during boot on every shipped image, so this is the common path. */
static char dos_fbs_xms_path[136];

/* store_file_in_flash()'s progress callback. Its own copy rather than
 * main_dos.c's: that one lives in .overlay_dos and this file exists to keep
 * cold code out of it. */
static void dos_fbs_progress(uint32_t total, uint32_t done, uint8_t pct)
{
    static uint8_t last = 255;
    (void)total; (void)done;
    if (pct / 25 != last / 25 || last == 255) {
        last = pct;
        printf("DOS: fbs caching %u%%\n", pct);
    }
    wdog_refresh();
}

static unsigned long dos_fbs_write_cb(const void *src, unsigned long len, void *ctx)
{
    wdog_refresh();
    return (unsigned long)fwrite(src, 1, (size_t)len, (FILE *)ctx);
}

/* LzmaDecode() sets p.dic = dest and p.dicBufSize = outSize, so the DESTINATION
 * IS THE DICTIONARY and a ~780 KB section decompresses straight into mem[] with
 * no staging buffer -- which is the only reason compression is expressible at
 * all against ~1.2 KB of AXI headroom.
 *
 * The 16 KB probability array is malloc()'d from the FIRMWARE HEAP (DTCM), not
 * from AXI and not from the stack: it is 15,980 bytes for lc=3 lp=0, the stack
 * is 20 KB and belongs to the firmware, and this runs once before the guest's
 * first instruction. It is freed before returning. */
static unsigned long dos_fbs_unlzma(unsigned char *dst, unsigned long dst_len,
                                    const unsigned char *src, unsigned long src_len,
                                    const unsigned char *props)
{
    ISzAlloc al;
    ELzmaStatus status;
    SizeT dl = (SizeT)dst_len, sl = (SizeT)src_len;
    SRes res;
    uint8_t *heap = malloc(LZMA_BUF_SIZE);

    if (!heap)
        return 0;
    lzma_init_allocs(&al, heap);
    /* Both terminal statuses accepted: a raw LZMA1 stream written by
     * test286/fbspack.py carries no end marker, and the output length is known
     * exactly. dos_fbs.c re-CRCs the result, so the end marker is not the check
     * that has to be strict. */
    res = LzmaDecode(dst, &dl, src, &sl, props, 5, LZMA_FINISH_END, &status, &al);
    free(heap);
    if (res != SZ_OK)
        return 0;
    if (status != LZMA_STATUS_FINISHED_WITH_MARK
        && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK)
        return 0;
    return (unsigned long)dl;
}

/* "/roms/dos/KEEN4.dsk" -> "/saves/dos/KEEN4.fbs". Same rule and same directory
 * as the .xipimg sidecar, for the same reason: SD is where a snapshot is
 * produced, flash is where it is consumed. */
static int dos_fbs_make_path(const char *dsk_path)
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
    if (n == 0 || n + sizeof("/saves/dos/.fbs.xms") >= sizeof dos_fbs_path)
        return -1;
    strcpy(dos_fbs_path, "/saves/dos/");
    memcpy(dos_fbs_path + 11, base, n);
    strcpy(dos_fbs_path + 11 + n, ".fbs");
    snprintf(dos_fbs_xms_path, sizeof dos_fbs_xms_path, "%s.xms", dos_fbs_path);
    return 0;
}

/* Copy a file. Two handles at once against MAX_OPEN_FILES = 8, and it runs
 * before dos_cpu_init() takes its three -- see the fopen-budget note above. */
static int dos_fbs_copy(const char *from, const char *to)
{
    static uint8_t buf[512];            /* .bss of main_dos.o, which is NOT in
                                         * .xip_dos; 512 B, sized to a sector */
    FILE *a, *b;
    size_t n;
    int ok = 1;

    if (!(a = fopen(from, "rb")))
        return 0;                        /* nothing to carry: not an error */
    if (!(b = fopen(to, "wb"))) { fclose(a); return 0; }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) {
        wdog_refresh();
        if (fwrite(buf, 1, n, b) != n) { ok = 0; break; }
    }
    fclose(a);
    fclose(b);
    return ok;
}

/* Boot half. AFTER dos_cpu_init(), which is the whole point: init has opened
 * the disks, loaded the BIOS blob and read the decode tables out of it, and has
 * not yet run one guest instruction. Overwriting the state now means DOS never
 * boots and the executable is never loaded. */
void dos_fbs_boot(const char *dsk_path, unsigned long dsk_size,
                  unsigned long key_crc, unsigned long mach_kb)
{
    uint32_t t0 = HAL_GetTick();
    dos_fbs_hdr_t h;
    dos_fbs_status_t rs;

    dos_fbs_key.key_crc  = key_crc;
    dos_fbs_key.key_size = dsk_size;
    dos_fbs_key.mach_kb  = mach_kb;

    if (dos_fbs_make_path(dsk_path) != 0) {
        printf("DOS: fbs path too long for %s\n", dsk_path);
        dos_fbs_captured = 1;            /* and do not try to capture either */
        return;
    }
    dos_fbs_set_unlzma(dos_fbs_unlzma);
    dos_fbs_set_xms_carried(1);          /* dos_fbs.h §7 -- the copies below */

    /* store_file_in_flash() opens and closes the file itself. NULL is the
     * missing-file case AND the full-flash case, and neither is an error:
     * both mean "no snapshot, boot normally". */
    dos_fbs_flash_size = 0;
    dos_fbs_flash = store_file_in_flash(dos_fbs_path, &dos_fbs_flash_size,
                                        false, &dos_fbs_progress);
    if (!dos_fbs_flash || dos_fbs_flash_size == 0) {
        printf("DOS: fbs no snapshot at %s - booting normally\n", dos_fbs_path);
        return;
    }

    /* THE POOL BEFORE THE SNAPSHOT. Restoring the scalar block closes the XMS
     * driver's FILE* and drops its cached seek position precisely so the next
     * access reopens the file we have just put in place. */
    dos_fbs_copy(dos_fbs_xms_path, dos_xms_store_path);

    rs = dos_fbs_restore(dos_fbs_flash, (unsigned long)dos_fbs_flash_size,
                         &dos_fbs_key, &h);
    if (rs == DOS_FBS_OK) {
        dos_fbs_restored = 1;
        dos_fbs_captured = 1;            /* never re-capture over a hit */
        printf("DOS: fbs RESTORED from %s (%lu sections, capture_at %lu, "
               "mach_kb %lu) in %lu ms - no DOS boot\n",
               dos_fbs_path, h.nsect, h.capture_at, h.mach_kb,
               (unsigned long)(HAL_GetTick() - t0));
    } else {
        printf("DOS: fbs not restored (%s) - booting normally\n",
               dos_fbs_status_name(rs));
        /* A REJECTED snapshot does NOT put us back on the capture path: it
         * would immediately overwrite a file a different build or a different
         * image legitimately owns, which is a write loop rather than a cache.
         * Same rule dos_xipsm.h §3 states for the .xipimg sidecar. */
        dos_fbs_captured = 1;
    }
}

/* Frame half. One compare below the mark. */
void dos_fbs_frame(unsigned int frames)
{
    uint32_t t0;
    unsigned long want, got = 0;
    dos_fbs_status_t cs;
    FILE *f;

    if (dos_fbs_captured || frames < DOS_FBS_AT_FRAMES)
        return;
    dos_fbs_captured = 1;                /* one attempt per session, win or lose */

    /* AT A dos_cpu_frame() BOUNDARY, which is a precondition and not a
     * convenience (dos_fbs.h §6): the operand pointers are per-instruction
     * temporaries and the frame head is the one place dos_seam_flush()
     * guarantees no straddling operand is still held. This call site is that
     * boundary -- it is the same one dos_xipsm_frame() uses. */
    t0 = HAL_GetTick();
    want = dos_fbs_store_size();
    f = fopen(dos_fbs_path, "wb");
    if (!f) {
        /* /saves/dos may not exist, or the card may be read-only. Neither is
         * worth failing a game over. */
        printf("DOS: fbs cannot write %s - capture skipped\n", dos_fbs_path);
        return;
    }
    cs = dos_fbs_capture(&dos_fbs_key, (unsigned long)frames,
                         dos_fbs_write_cb, f, &got);
    fclose(f);
    if (cs != DOS_FBS_OK || got != want) {
        /* A truncated payload under a good header is the ONE shape a
         * length-and-CRC format cannot detect on the way back in, so the
         * partial file goes rather than being left to be trusted later. */
        remove(dos_fbs_path);
        printf("DOS: fbs capture failed (%s, %lu of %lu B) - deleted\n",
               dos_fbs_status_name(cs), got, want);
        return;
    }
    /* The XMS pool, beside the snapshot, and AFTER the .fbs is closed so a
     * failure here cannot also truncate the snapshot. */
    if (!dos_fbs_copy(dos_xms_store_path, dos_fbs_xms_path))
        remove(dos_fbs_xms_path);

    printf("DOS: fbs CAPTURED %lu B to %s at frame %u in %lu ms "
           "(uncompressed; run test286/fbspack.py to LZMA it)\n",
           got, dos_fbs_path, frames, (unsigned long)(HAL_GetTick() - t0));
}
#else
void dos_fbs_boot(const char *dsk_path, unsigned long dsk_size,
                  unsigned long key_crc, unsigned long mach_kb)
{ (void)dsk_path; (void)dsk_size; (void)key_crc; (void)mach_kb; }
void dos_fbs_frame(unsigned int frames) { (void)frames; }
#endif
