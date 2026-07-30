/* Guest CPU speed profiles + frame-time profiling for the DOS core.
 *
 * Why profiles exist at all: most DOS games have no frame limiter and run at
 * whatever speed the CPU provides -- the turbo-button problem. Alley Cat on a
 * too-fast machine is unplayable, so per-title speed selection is a
 * requirement, not a nicety.
 *
 * The mapping is a data table for the same reason dos_key_map[] is
 * (dos_input.c): it has one job, it will be retuned, and retuning it should be
 * editing one row rather than touching control flow.
 *
 * IMPORTANT CAVEAT, stated once here so nobody reads more into these numbers
 * than they carry: 8086tiny counts *instructions*, not cycles
 * (`++executed >= cycles`, 8086tiny.c:1119), and tracks no per-instruction
 * cost. A real 8086 spends 2 cycles on a register MOV and 100+ on a DIV. So a
 * profile is an approximation of an *average* instruction, and a
 * division-heavy or a MOV-heavy program will sit at the wrong relative speed
 * regardless of how well the table is tuned. Fixing that means per-opcode cycle
 * costs in the interpreter, which is a separate piece of work.
 */

#include "dos_cpu.h"
#include "common.h"
#include "main.h"        /* wdog_refresh(), SystemCoreClock via CMSIS */
#include <stdio.h>

/* 8086tiny. dos_cpu_frame(n) executes n instructions and returns 1, or returns
 * 0 early when CS:IP folds to 0 (guest halted). inst_counter is bumped once per
 * instruction (8086tiny.c:1053), which is what we difference to find out how
 * many instructions a frame actually got. */
extern int dos_cpu_frame(int cycles);
extern unsigned int inst_counter;
extern unsigned int dos_putchar_count;

/* ---- The profile table ----------------------------------------------------
 *
 * insn_per_frame is at 60 fps, so instructions/second = insn_per_frame * 60.
 * The MIPS figures in the comments are what that works out to; the machine
 * names are the era those rates land in, not a claim of accuracy.
 *
 * 8088 @ 4.77 MHz is usually put near 0.33-0.4 MIPS: the 8088's 8-bit bus and
 * 4-cycle bus access dominate, giving an average of roughly 12-14 cycles per
 * instruction. 6,700/frame = 402,000/s sits at the top of that range, which is
 * the right side to err on for a period game.
 *
 * 20,000 is the historical hardcoded value from main_dos.c and is kept as a
 * profile so behaviour is unchanged by default and TOPBENCH scores stay
 * comparable across this change. Nobody derived it; the "286" label is
 * retrofitted from where 1.2 MIPS lands, not from measurement.
 *
 * insn_per_frame == 0 means MAX -- run to the frame deadline, see
 * dos_cpu_run_frame(). It must be the *last* entry only by convention; nothing
 * depends on its position.
 */
typedef struct {
    const char  *name;
    unsigned int insn_per_frame;   /* 0 = MAX (deadline-driven) */
} dos_cpu_profile_t;

static const dos_cpu_profile_t dos_cpu_profiles[] = {
    { "XT 4.77MHz",  6700 },   /* ~0.40 MIPS -- 8088-era, for 1984 titles   */
    { "Turbo 8MHz", 11000 },   /* ~0.66 MIPS -- 8086 turbo XT               */
    { "286 12MHz",  20000 },   /* ~1.20 MIPS -- the historical default      */
    { "MAX",            0 },   /* whatever the STM32H7B0 sustains           */
};

#define DOS_CPU_PROFILE_COUNT ((int)(sizeof dos_cpu_profiles / sizeof dos_cpu_profiles[0]))

/* Default index 2 == 20000, i.e. exactly the pre-existing behaviour.
 *
 * PER-GAME PERSISTENCE IS OUT OF SCOPE and this variable is where it would
 * land. The mechanism to use is odroid_settings_app_get/set_int32() keyed on
 * the ROM (see how other cores persist per-app options); the read belongs in
 * dos_cpu_speed_init() and the write in dos_cpu_speed_update_cb(), so nothing
 * else in the core would change. */
static int dos_cpu_profile_idx = 2;

/* ---- MAX mode -------------------------------------------------------------
 *
 * Do NOT implement MAX by passing a huge count to dos_cpu_frame(): it executes
 * the whole budget before returning, so one frame would run for hundreds of
 * milliseconds, wreck audio/video pacing, and -- because wdog_refresh() is
 * called once per frame in main_dos.c -- trip WWDG1, whose window is a few
 * hundred ms (Core/Src/main.c:247; see docs/traps.md, which is there because
 * DOS already BSOD'd this way once with PC=0/LR=0).
 *
 * Instead: small chunks against a DWT deadline, refreshing the watchdog every
 * chunk, so a long frame is still safe.
 *
 * Chunk size is deliberately not a power of two and not a multiple of
 * KEYBOARD_TIMER_UPDATE_DELAY (2048, 8086tiny.c:94). That coupling has bitten
 * before: when the frame budget equalled the keyboard/timer period, every frame
 * ended at the same point in the tick cycle and the CS:IP diagnostic sampled
 * one instruction forever, making a healthy guest look wedged (docs/traps.md,
 * *Guest time*). 1500 is coprime with 2048.
 */
#define DOS_MAX_CHUNK_INSN 1500

/* Share of the frame period MAX mode may spend inside the CPU. The rest pays
 * for the blit, the launcher's input/menu work, and leaves common_emu_sound_sync
 * something to wait on -- if the CPU eats the whole period, audio pacing has no
 * slack and the frame integrator starts declaring skips. */
#define DOS_MAX_FRAME_PCT 85

/* ---- Profiling accumulators ----------------------------------------------
 *
 * Permanently enabled, per docs/video/09-video-profiling.md: reading
 * DWT->CYCCNT is a single load, and a profiling build that has to be specially
 * produced is a profiling build nobody runs. Only the *printing* is behind
 * DOS_DEBUG_STATUS.
 *
 * All arithmetic is uint32 subtraction of a free-running counter, so it is
 * correct across the CYCCNT wrap (~15 s at 280 MHz -- which is why the sample
 * window is ~1 s and the accumulators are 64-bit).
 */
static uint64_t prof_cpu, prof_blit, prof_idle;
static uint32_t prof_frames, prof_insn, prof_blits;
static uint32_t prof_win_ms, prof_win_cyc;      /* window start marks */
static uint32_t prof_blit_t0, prof_idle_t0;
static uint32_t prof_putchar0;

/* Last completed sample, kept so the menu can show the achieved rate without a
 * debug build. 0 until the first window closes. */
static uint32_t prof_last_ips;

/* Read DWT->CYCCNT directly rather than through common_emu_get_dwt_cycles():
 * that one is `inline __attribute__((always_inline))` in common.c with a plain
 * prototype in common.h, so from another translation unit it is an out-of-line
 * call, not the single load 09-video-profiling.md counts on. The counter itself
 * is enabled by common_emu_enable_dwt_cycles(). */
#define DOS_DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)
static inline uint32_t dos_cyc(void) { return DOS_DWT_CYCCNT; }

void dos_cpu_speed_init(void)
{
    common_emu_enable_dwt_cycles();
    prof_cpu = prof_blit = prof_idle = 0;
    prof_frames = prof_insn = 0;
    prof_win_ms  = (uint32_t)HAL_GetTick();
    prof_win_cyc = dos_cyc();
    prof_putchar0 = dos_putchar_count;
}

const char *dos_cpu_profile_name(void)
{
    return dos_cpu_profiles[dos_cpu_profile_idx].name;
}

int dos_cpu_run_frame(void)
{
    const unsigned int budget = dos_cpu_profiles[dos_cpu_profile_idx].insn_per_frame;
    uint32_t t0 = dos_cyc();
    unsigned int insn0 = inst_counter;
    int alive;

    if (budget) {
        alive = dos_cpu_frame((int)budget);
    } else {
        /* MAX: chunk to the deadline. SystemCoreClock is read live rather than
         * hardcoded because the firmware's PLL config is user-selectable
         * (Core/Src/main.c:480-499 -- 280 MHz stock, up to ~354 MHz
         * overclocked), so a constant here would silently mis-size the budget
         * on an overclocked unit. */
        uint32_t deadline_cyc = (uint32_t)((SystemCoreClock / 60u) / 100u * DOS_MAX_FRAME_PCT);
        alive = 1;
        do {
            alive = dos_cpu_frame(DOS_MAX_CHUNK_INSN);
            /* Once per chunk, not once per frame. This is the whole reason MAX
             * mode is safe: see the WWDG1 note above. */
            wdog_refresh();
        } while (alive && (uint32_t)(dos_cyc() - t0) < deadline_cyc);
    }

    prof_cpu   += (uint32_t)(dos_cyc() - t0);
    prof_insn  += (uint32_t)(inst_counter - insn0);
    prof_frames++;
    return alive;
}

void dos_prof_blit_begin(void) { prof_blit_t0 = dos_cyc(); }
void dos_prof_blit_end(void)   { prof_blit += (uint32_t)(dos_cyc() - prof_blit_t0); prof_blits++; }
void dos_prof_idle_begin(void) { prof_idle_t0 = dos_cyc(); }
void dos_prof_idle_end(void)   { prof_idle += (uint32_t)(dos_cyc() - prof_idle_t0); }

bool dos_prof_take_sample(dos_prof_sample_t *out)
{
    uint32_t now_ms  = (uint32_t)HAL_GetTick();
    uint32_t now_cyc = dos_cyc();
    uint32_t ms      = now_ms - prof_win_ms;
    uint32_t win     = now_cyc - prof_win_cyc;   /* total cycles in the window */

    if (!ms || !win || !prof_frames) return false;

    out->frames         = prof_frames;
    out->ms             = ms;
    out->insn_per_frame = prof_insn / prof_frames;
    out->insn_per_sec   = (uint32_t)(((uint64_t)prof_insn * 1000u) / ms);

    /* Percentages of the *window*, not of each other. Their sum falling short
     * of 100 is the interesting case: the missing time is firmware -- menus, SD
     * access, input -- which docs/video/09-video-profiling.md calls out as a
     * signal in its own right, so it is reported as `other` rather than hidden. */
    out->cpu_pct  = (uint8_t)((prof_cpu  * 100u) / win);
    out->blit_pct = (uint8_t)((prof_blit * 100u) / win);
    out->idle_pct = (uint8_t)((prof_idle * 100u) / win);
    {
        int rest = 100 - out->cpu_pct - out->blit_pct - out->idle_pct;
        out->other_pct = (uint8_t)(rest > 0 ? rest : 0);
    }

    /* Absolute microseconds, so the cpu-vs-blit split can be compared against
     * the 16,667 us frame period directly rather than only as a ratio. */
    {
        uint32_t mhz = SystemCoreClock / 1000000u;   /* 280 stock */
        out->blits        = prof_blits;
        out->mhz          = mhz;
        out->cpu_us_frame = (uint32_t)(prof_cpu / prof_frames / mhz);
        out->blit_us      = prof_blits ? (uint32_t)(prof_blit / prof_blits / mhz) : 0;
        out->cyc_per_insn = prof_insn ? (uint32_t)(prof_cpu / prof_insn) : 0;
        out->putchars     = dos_putchar_count - prof_putchar0;
        prof_putchar0     = dos_putchar_count;
    }

    prof_last_ips = out->insn_per_sec;

    prof_cpu = prof_blit = prof_idle = 0;
    prof_frames = prof_insn = prof_blits = 0;
    prof_win_ms  = now_ms;
    prof_win_cyc = now_cyc;
    return true;
}

/* ---- Options menu row ---------------------------------------------------- */

bool dos_cpu_speed_update_cb(odroid_dialog_choice_t *option,
                             odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    int max = DOS_CPU_PROFILE_COUNT - 1;

    if (event == ODROID_DIALOG_PREV)
        dos_cpu_profile_idx = dos_cpu_profile_idx > 0 ? dos_cpu_profile_idx - 1 : max;
    if (event == ODROID_DIALOG_NEXT)
        dos_cpu_profile_idx = dos_cpu_profile_idx < max ? dos_cpu_profile_idx + 1 : 0;

    /* PER-GAME PERSISTENCE would be written here (see dos_cpu_profile_idx). */

    const dos_cpu_profile_t *p = &dos_cpu_profiles[dos_cpu_profile_idx];
    if (p->insn_per_frame) {
        /* Show the rate the profile asks for, in thousands of instructions per
         * second -- the number the label is actually promising. */
        snprintf(option->value, DOS_CPU_VALUE_LEN, "%s", p->name);
    } else if (prof_last_ips) {
        /* MAX is worthless without a number, and requiring a debug build to see
         * it would mean nobody sees it. Two decimal places of MIPS. */
        unsigned int centi = prof_last_ips / 10000u;   /* hundredths of a MIPS */
        snprintf(option->value, DOS_CPU_VALUE_LEN, "MAX %u.%02u M", centi / 100u, centi % 100u);
    } else {
        snprintf(option->value, DOS_CPU_VALUE_LEN, "MAX");
    }
    return event == ODROID_DIALOG_ENTER;
}
