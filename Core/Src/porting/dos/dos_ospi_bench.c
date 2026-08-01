/* OSPI read-latency benchmark — the kill-switch measurement for guest XIP.
 *
 * external/8086tiny/docs/memory/02-guest-xip.md §5 estimates a cold 32-byte
 * cache-line fill from external flash at ~280 core cycles. That figure is
 * ARITHMETIC from datasheet-level facts (four IO lines, the OSPI clock in
 * Core/Src/main.c:457-460, 32-byte lines) and has never been measured. §7
 * phase 1 makes measuring it the precondition for the whole feature: "if the
 * real figure is materially worse this feature is dead".
 *
 * This file answers that, plus the three questions the design silently assumes:
 *
 *   1. cold line-fill cost from 0x90000000, in core cycles
 *   2. whether the D-cache absorbs a sequential stream (§5's other assumption)
 *   3. whether 0x90000000 is cacheable AT ALL in this firmware. No MPU region
 *      covers it (Core/Src/main.c MPU_Config), so it inherits the Cortex-M7
 *      default map, where 0x80000000-0x9FFFFFFF is Normal write-back. That is
 *      a claim about the default map, not about this board -- so the bench
 *      reports the warm/cold ratio and lets the number settle it. A ratio near
 *      1.0 means uncached, and XIP would be dead on arrival.
 *
 * Method notes that matter for believing the output:
 *
 *   - Cold means cold. SCB_CleanInvalidateDCache() before each cold pass, and
 *     the stride is one access per 32-byte line so no access is ever a hit on
 *     a line a previous access filled.
 *   - The loop reads through a volatile pointer and accumulates into a value
 *     that is printed, so neither the loads nor the loop can be elided.
 *   - The instrument's own cost is measured the same way 8086tiny.c's
 *     dwt_calibrate() does, and subtracted.
 *   - AXI SRAM runs the identical loop as the control. The difference between
 *     the two is the OSPI penalty; the absolute numbers include loop overhead
 *     that the subtraction removes.
 *
 * Enable with DOS_CFLAGS_EXTRA=-DDOS_OSPI_BENCH=1. Off by default and costs
 * nothing when off.
 */

#include "dos_ospi_bench.h"

#if DOS_OSPI_BENCH

#include <stdio.h>
#include "stm32h7xx_hal.h"
#include "gw_flash.h"

#define DWT_CYCCNT_R (*(volatile unsigned int *)0xE0001004u)
#define DWT_CTRL_R   (*(volatile unsigned int *)0xE0001000u)
#define DEMCR_R      (*(volatile unsigned int *)0xE000EDFCu)

#define OSPI_BASE_ADDR 0x90000000u
#define LINE_BYTES     32u      /* Cortex-M7 D-cache line */
#define LINES          256u     /* 8 KB swept. Deliberately UNDER the 16 KB
                                 * D-cache: the warm pass is only meaningful if
                                 * the sweep is fully resident, otherwise "warm"
                                 * measures a mix of hits and refills and the
                                 * cold/warm ratio understates the penalty. The
                                 * cold pass is still genuinely cold because it
                                 * is preceded by a full clean+invalidate.
                                 * (8 KB also keeps the control buffer inside
                                 * the overlay's AXI headroom -- 32 KB failed
                                 * the linker's DOS-overflow ASSERT.) */

/* AXI control buffer. Deliberately not static-const: it must be real RAM that
 * behaves like guest memory does. */
static volatile unsigned char axi_buf[LINES * LINE_BYTES]
    __attribute__((aligned(32)));

/* One timing pass: touch one byte per cache line across `lines` lines starting
 * at `base`, return raw CYCCNT delta. `sum` is returned to the caller so the
 * loads cannot be optimised away. */
static unsigned int sweep(volatile const unsigned char *base, unsigned int lines,
                          unsigned int *sum)
{
    unsigned int acc = 0, t0, t1, i;

    t0 = DWT_CYCCNT_R;
    for (i = 0; i < lines; i++)
        acc += base[i * LINE_BYTES];
    t1 = DWT_CYCCNT_R;

    *sum = acc;
    return t1 - t0;
}

/* The same loop over a region small enough to be cache-resident, used to price
 * the loop itself (increment, compare, branch, accumulate) so the reported
 * per-line figures are memory cost rather than loop cost. */
static unsigned int loop_overhead(unsigned int lines)
{
    volatile const unsigned char *p = axi_buf;
    unsigned int sum, best = 0xFFFFFFFFu, r;

    /* Warm it, then take the best of three -- an interrupt landing inside a
     * pass inflates it, and the minimum is the least contaminated estimate. */
    for (r = 0; r < 4; r++) {
        unsigned int c = sweep(p, lines, &sum);
        if (r && c < best) best = c;
    }
    return best;
}

void dos_ospi_bench(void)
{
    unsigned int sum, cold_ospi, warm_ospi, cold_axi, warm_axi, ovh;
    unsigned int i, best, bad_overhead = 0;
    unsigned long ospi_hz;
    volatile const unsigned char *ospi = (volatile const unsigned char *)OSPI_BASE_ADDR;

    DEMCR_R |= (1u << 24);   /* TRCENA */
    DWT_CTRL_R |= 1u;        /* CYCCNTENA */

    /* Touch the control buffer so it is committed and its lines exist. */
    for (i = 0; i < sizeof(axi_buf); i += LINE_BYTES)
        axi_buf[i] = (unsigned char)i;

    ovh = loop_overhead(LINES);

    /* --- cold passes: invalidate, sweep once, take the best of several ----
     * Best-of rather than mean: a DMA burst or an interrupt only ever ADDS
     * cycles, so the minimum is the closest estimate of the true cost. */
    best = 0xFFFFFFFFu;
    for (i = 0; i < 4; i++) {
        SCB_CleanInvalidateDCache();
        __DSB(); __ISB();
        unsigned int c = sweep(ospi, LINES, &sum);
        if (c < best) best = c;
    }
    cold_ospi = best;

    /* Warm: immediately re-sweep without invalidating. If 0x90000000 is
     * cacheable this collapses to roughly the AXI warm figure. If it does not
     * collapse, the region is not being cached and XIP is dead. */
    best = 0xFFFFFFFFu;
    for (i = 0; i < 4; i++) {
        unsigned int c = sweep(ospi, LINES, &sum);
        if (c < best) best = c;
    }
    warm_ospi = best;

    best = 0xFFFFFFFFu;
    for (i = 0; i < 4; i++) {
        SCB_CleanInvalidateDCache();
        __DSB(); __ISB();
        unsigned int c = sweep(axi_buf, LINES, &sum);
        if (c < best) best = c;
    }
    cold_axi = best;

    best = 0xFFFFFFFFu;
    for (i = 0; i < 4; i++) {
        unsigned int c = sweep(axi_buf, LINES, &sum);
        if (c < best) best = c;
    }
    warm_axi = best;

    /* The OSPI clock is what makes cycles/line interpretable at all -- without
     * it there is no way to tell a slow bus from a fast one being polled badly.
     * HAL returned 0 for this under gwemu, so print the raw D1CCIPR selector
     * alongside it: a 0 with a plausible selector means "HAL cannot compute it
     * here", a 0 with a garbage selector means the clock tree is not set up.
     * (CDCCIPR, not D1CCIPR -- the H7B0 renamed the domain-1 register.) */
    ospi_hz = (unsigned long)HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_OSPI);
    printf("OSPIBENCH sysclk=%lu ospi_hz=%lu%s cdccipr=0x%08lx lines=%u line=%uB sweep=%uKB\n",
           (unsigned long)HAL_RCC_GetSysClockFreq(), ospi_hz,
           ospi_hz ? "" : " (HAL RETURNED 0 -- cycles/line not convertible to ns)",
           (unsigned long)RCC->CDCCIPR,
           (unsigned)LINES, (unsigned)LINE_BYTES,
           (unsigned)(LINES * LINE_BYTES / 1024));
    printf("OSPIBENCH loop_overhead=%u cyc total (%u.%02u cyc/line)\n",
           ovh, ovh / LINES, (ovh * 100 / LINES) % 100);

    /* If the calibrated loop cost is not comfortably below the smallest raw
     * sweep, the subtraction below is meaningless and every derived figure
     * collapses to zero -- which reads exactly like a real "uncached, zero
     * penalty" answer. Refuse to report derived numbers in that case rather
     * than printing a confident wrong one. (Observed under gwemu: ovh=252 with
     * every raw sweep below it, so all four nets clamped to 0 and the verdict
     * lines printed cold_fill=0.) */
    {
        unsigned int lo = cold_ospi;
        if (warm_ospi < lo) lo = warm_ospi;
        if (cold_axi  < lo) lo = cold_axi;
        if (warm_axi  < lo) lo = warm_axi;
        if (ovh >= lo) {
            printf("OSPIBENCH *** INVALID: loop_overhead=%u >= smallest raw sweep=%u.\n",
                   ovh, lo);
            printf("OSPIBENCH *** The instrument costs more than what it measures;\n");
            printf("OSPIBENCH *** every derived figure below would be zero. RAW ONLY.\n");
            bad_overhead = 1;
        }
    }

#define REPORT(name, raw)                                                     \
    do {                                                                      \
        unsigned int net = (raw) > ovh ? (raw) - ovh : 0;                     \
        if (bad_overhead)                                                     \
            printf("OSPIBENCH %-10s raw=%8u  %4u.%02u cyc/line RAW\n",        \
                   name, (unsigned)(raw), (raw) / LINES,                      \
                   ((raw) * 100 / LINES) % 100);                              \
        else                                                                  \
            printf("OSPIBENCH %-10s raw=%8u net=%8u  %4u.%02u cyc/line\n",    \
                   name, (unsigned)(raw), net, net / LINES,                   \
                   (net * 100 / LINES) % 100);                                \
    } while (0)

    REPORT("ospi_cold", cold_ospi);
    REPORT("ospi_warm", warm_ospi);
    REPORT("axi_cold",  cold_axi);
    REPORT("axi_warm",  warm_axi);
#undef REPORT

    /* The two derived numbers that decide the feature. Suppressed entirely when
     * the overhead calibration is unusable -- a printed 0 here is worse than no
     * line at all, because it is shaped like an answer. */
    if (bad_overhead) {
        printf("OSPIBENCH VERDICT suppressed -- overhead calibration invalid (see above).\n");
        return;
    }
    {
        unsigned int cn = cold_ospi > ovh ? cold_ospi - ovh : 0;
        unsigned int wn = warm_ospi > ovh ? warm_ospi - ovh : 0;
        unsigned int an = cold_axi  > ovh ? cold_axi  - ovh : 0;
        /* Fixed-point, not integer: an integer divide renders anything below
         * 1 cyc/line as a flat "0", which is the same false-zero that the
         * overhead guard above exists to prevent. Real hardware should land
         * near 280 and never exercise this, but a 0 must mean zero. */
        printf("OSPIBENCH VERDICT cold_fill=%u.%02u cyc/line (doc estimate 280)\n",
               cn / LINES, (cn * 100 / LINES) % 100);
        printf("OSPIBENCH VERDICT cold/warm=%u.%02ux %s\n",
               wn ? cn / wn : 0, wn ? (cn * 100 / wn) % 100 : 0,
               (wn && cn / wn >= 2) ? "(cached: warm is much cheaper)"
                                    : "(NOT CACHED or warm not resident -- suspect)");
        printf("OSPIBENCH VERDICT ospi/axi cold ratio=%u.%02ux\n",
               an ? cn / an : 0, an ? (cn * 100 / an) % 100 : 0);
    }
}

#endif /* DOS_OSPI_BENCH */
