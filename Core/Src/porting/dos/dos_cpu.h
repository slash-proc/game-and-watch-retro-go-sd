/* Guest CPU speed profiles and frame-time profiling for the DOS core.
 * See external/8086tiny/docs/cpu/01-speed-profiles.md.
 */
#ifndef DOS_CPU_H
#define DOS_CPU_H

#include <stdbool.h>
#include <stdint.h>
#include "odroid_overlay.h"

/* Call once before the frame loop. Enables DWT->CYCCNT and zeroes the
 * accumulators. Safe to call when the counter is already running. */
void dos_cpu_speed_init(void);

/* Run one frame's worth of guest instructions under the selected profile.
 * Returns 0 exactly when dos_cpu_frame() returns 0 (guest halted, CS:IP folded
 * to 0), 1 otherwise -- same contract as dos_cpu_frame(), so the caller's
 * "test for zero" break stays correct.
 *
 * In MAX mode this loops dos_cpu_frame() in small chunks against a cycle
 * deadline and refreshes the watchdog every chunk. */
int dos_cpu_run_frame(void);

/* Minimum size of the char buffer handed to the options row as `value`. */
#define DOS_CPU_VALUE_LEN 20

/* Options-menu row. Cycles the profile with left/right and shows the achieved
 * rate for MAX. */
bool dos_cpu_speed_update_cb(odroid_dialog_choice_t *option,
                             odroid_dialog_event_t event, uint32_t repeat);

/* ---- Profiling (permanently enabled; reading CYCCNT is one load) ---------- */

void dos_prof_blit_begin(void);
void dos_prof_blit_end(void);
void dos_prof_idle_begin(void);
void dos_prof_idle_end(void);

/* Snapshot of the last completed sample window, and a reset that opens a new
 * one. All values are for the window as a whole. */
typedef struct {
    uint32_t frames;
    uint32_t ms;             /* wall clock, HAL_GetTick() delta            */
    uint32_t insn_per_frame; /* mean instructions actually executed        */
    uint32_t insn_per_sec;   /* derived: insn / (ms/1000)                  */
    uint8_t  cpu_pct;        /* share of window spent in dos_cpu_frame()   */
    uint8_t  blit_pct;
    uint8_t  idle_pct;       /* waiting in common_emu_sound_sync()         */
    uint8_t  other_pct;      /* remainder: launcher, input, SD, menus      */
    uint32_t blits;          /* frames actually drawn (drawFrame true)     */
    uint32_t cpu_us_frame;   /* mean us in dos_cpu_frame() per frame       */
    uint32_t blit_us;        /* mean us per blit that actually happened    */
    uint32_t mhz;            /* core clock -- oc_level is user-selectable, */
                             /* so no number here means anything without it*/
    uint32_t putchars;       /* guest PUTCHAR_AL calls in the window -- see
                              * dos_putchar_count in 8086tiny.c. Proxy for how
                              * much of the guest's budget vmem_driver_entry is
                              * spending repainting a terminal we do not have. */
    /* Guest BIOS timer-tick accounting for the window. `due` is how many 55 ms
     * deadlines were reached, i.e. how far guest time SHOULD have advanced;
     * `fired` is how many int 0xA interrupts the guest actually took. They
     * differ because int8_asap is a flag rather than a count, so deadlines
     * passing while the guest has IF clear (or a prefix/REP pending) collapse
     * into one interrupt. `resync` counts the other loss path, where the
     * deadline is reset forward and the backlog is discarded.
     *
     * fired < due means the guest's clock is running slow, which inflates and
     * destabilises any guest-side benchmark. This exists because TOPBENCH
     * scored 19-83 while host-side ips held to +/-0.2%. */
    uint32_t int8_due;
    uint32_t int8_fired;
    uint32_t int8_resync;
    uint32_t cyc_per_insn;   /* ARM cycles spent per guest instruction.
                              * The headline efficiency number: comparable
                              * across clocks and platforms, unlike ips.   */
} dos_prof_sample_t;

/* Closes the current window and returns it. Returns false if no time has
 * elapsed (nothing meaningful to report yet). */
bool dos_prof_take_sample(dos_prof_sample_t *out);

/* Name of the active profile, for logging. */
const char *dos_cpu_profile_name(void);

#endif /* DOS_CPU_H */
