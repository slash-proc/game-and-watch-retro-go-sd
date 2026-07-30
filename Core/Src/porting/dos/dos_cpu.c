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
#include "gw_lcd.h"      /* lcd_set_refresh_rate(), lcd_is_swap_pending()   */
#include "gw_audio.h"    /* AUDIO_SAMPLE_RATE, AUDIO_BUFFER_LENGTH          */
#include "odroid_settings.h"
#include <stdio.h>

/* 8086tiny. dos_cpu_frame(n) executes n instructions and returns 1, or returns
 * 0 early when CS:IP folds to 0 (guest halted). inst_counter is bumped once per
 * instruction (8086tiny.c:1053), which is what we difference to find out how
 * many instructions a frame actually got. */
extern int dos_cpu_frame(int cycles);
extern unsigned int inst_counter;
extern unsigned int dos_putchar_count;
extern unsigned int dos_int8_due, dos_int8_fired, dos_int8_resync;

/* ---- The profile table ----------------------------------------------------
 *
 * The field is instructions per *second*, and the per-frame budget is derived
 * at runtime as insn_per_sec / guest fps (dos_cpu_derive()). It used to be
 * instructions per frame with a hidden "/60"; that silently redefined what
 * "286 12MHz" means the moment the screen frequency became user-selectable
 * (SCREEN-FREQ-PLAN.md section 2). The emulated machine must not change
 * identity behind the user's back, so the table states the rate and the frame
 * budget follows the refresh rate.
 *
 * The MIPS figures in the comments are what these work out to; the machine
 * names are the era those rates land in, not a claim of accuracy.
 *
 * 8088 @ 4.77 MHz is usually put near 0.33-0.4 MIPS: the 8088's 8-bit bus and
 * 4-cycle bus access dominate, giving an average of roughly 12-14 cycles per
 * instruction. 402,000/s sits at the top of that range, which is the right side
 * to err on for a period game.
 *
 * 1,260,000/s (21,000/frame at 60 Hz) is the historical value from main_dos.c
 * and is kept as a profile so behaviour is unchanged by default and TOPBENCH
 * scores stay comparable across this change. Nobody derived it; the "286" label
 * is retrofitted from where 1.26 MIPS lands, not from measurement.
 *
 * insn_per_sec == 0 means MAX -- run to the frame deadline, see
 * dos_cpu_run_frame(). It must be the *last* entry only by convention; nothing
 * depends on its position.
 */
typedef struct {
    const char  *name;
    unsigned int insn_per_sec;     /* 0 = MAX (deadline-driven) */
} dos_cpu_profile_t;

static const dos_cpu_profile_t dos_cpu_profiles[] = {
    { "XT 4.77MHz",  402000 },   /* ~0.40 MIPS -- 8088-era, for 1984 titles */
    { "Turbo 8MHz",  660000 },   /* ~0.66 MIPS -- 8086 turbo XT             */
    /* 21000/frame at 60 Hz was measured, not guessed. Sweeping ipf on hardware
     * at the MS-DOS idle prompt (cpi 208): 20000 -> cpu 89%/idle 7%; 21000 ->
     * cpu 93%/idle 2-3%; 21500 -> cpu 96%/idle 0% (the true edge); 22000 ->
     * cpu 98% and the blit count collapses to 33-35 of 64, i.e. HALF THE VISUAL
     * FRAMES DROPPED. 21000 is the highest step that keeps real margin.
     *
     * Note the ceiling is a *per-frame* one, so it moves with the refresh rate:
     * at 50 Hz the same 1.26 MIPS is 25,200 instructions in a 20 ms frame,
     * which is the same duty cycle. At 75 Hz it is 16,800 in 13.3 ms. The
     * headroom argument is unchanged; only the absolute ipf moves.
     *
     * Watch the blit count, not frames=N/Mms, when re-tuning this:
     * common_emu_frame_loop() skips the screen draw when it falls behind, so
     * frames= stays a healthy 64/1066ms right through the drops. */
    { "286 12MHz",  1260000 },   /* ~1.26 MIPS -- the historical default    */
    { "MAX",              0 },   /* whatever the STM32H7B0 sustains         */
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

/* ---- Screen frequency -----------------------------------------------------
 *
 * The single source of truth for "how many guest frames per second are we
 * producing". The panel rate and the guest frame rate are the same number here
 * (there are no half-rate modes; see SCREEN-FREQ-PLAN.md stage 3, deliberately
 * not implemented). Everything that used to hardcode 60 -- the frame budget,
 * the MAX deadline, frame_time_10us, the audio DMA length -- reads it.
 *
 * Default 60 so the de-hardcoding is a provable no-op against the old build. */
#define DOS_SCREEN_HZ_DEFAULT 60u
static uint32_t dos_screen_hz_sel = DOS_SCREEN_HZ_DEFAULT;

/* The only rates lcd_set_refresh_rate() can reach. Each is an exact PLL3 (N,R)
 * pair against the fixed 392x255 LTDC frame (gw_lcd.c:549-567, main.c:764-768),
 * so there is no fractional-N drift. Anything else falls into that function's
 * `else { return; }` and SILENTLY leaves the panel where it was -- which is why
 * this is a table of the reachable set rather than a free-form number.
 *
 * Deliberately no 25/30 entries: there is no 25 or 30 Hz LTDC clock, so those
 * would have to be half-rate guest generation on a 50/60 Hz panel, which is a
 * separate change (SCREEN-FREQ-PLAN.md stage 3) carrying an audio-buffer hazard
 * -- 48000/25 = 1920 overruns audiobuffer_dma. See DOS_AUDIO_LEN below. */
static const uint16_t dos_screen_rates[] = { 50, 60, 72, 75 };
#define DOS_SCREEN_RATE_COUNT ((int)(sizeof dos_screen_rates / sizeof dos_screen_rates[0]))
static int dos_screen_rate_idx = 1;      /* 60 Hz */

/* App-scoped, not per-ROM: the panel rate is a display-comfort and throughput
 * choice about the user's hardware, whereas CPU speed is a per-title
 * compatibility choice. Deliberately NOT in the savestate, so the savestate
 * format is untouched.
 *
 * !!! THIS DOES NOT ACTUALLY PERSIST YET, and the reason is not in this file.
 * The generic key/value settings API is a STUB in this fork:
 *   Core/Src/porting/odroid_settings.c:410  app_int32_get -> return default_value;
 *   Core/Src/porting/odroid_settings.c:277  int32_set     -> empty body
 * so the set is discarded and the get always answers the default. (The vendored
 * upstream copy at retro-go-stm32/components/odroid/odroid_settings.c:189-199 IS
 * cJSON-backed and does work -- but Makefile.common:441 compiles the Core/Src
 * one, not that. Do not be misled by the header, which declares both alike.)
 *
 * Real persistence in this fork means adding a field to persistent_config_t
 * (odroid_settings.c:58-88), which is CRC'd and version-gated: bumping
 * `version` (currently 8) makes odroid_settings_init() take the "New config
 * version, resetting settings" path and WIPE every user's launcher settings.
 * That is a launcher-wide call, not a DOS-core one, so it is left to the
 * reviewer. Measured consequence today: the row works and applies immediately,
 * but the core comes up at 60 Hz on every launch.
 *
 * The calls below are kept because they are the correct API and become correct
 * for free the moment a real store exists. They cost two no-op calls per launch. */
#define DOS_SCREEN_HZ_KEY "dos_screen_hz"

/* Instructions per frame for the active (profile, rate) pair. Cached because
 * both inputs change only from a menu callback, and dos_cpu_run_frame() is the
 * hot path. 0 means MAX. */
static unsigned int dos_insn_per_frame = 1260000u / DOS_SCREEN_HZ_DEFAULT;

static void dos_cpu_derive(void)
{
    unsigned int ips = dos_cpu_profiles[dos_cpu_profile_idx].insn_per_sec;
    dos_insn_per_frame = ips ? ips / dos_screen_hz_sel : 0u;
}

uint32_t dos_screen_hz(void) { return dos_screen_hz_sel; }

/* Audio DMA length for a given panel rate.
 *
 * SAFETY: the DOS core emits no samples, but audio_start_playing() is the frame
 * pacer -- common_emu_sound_sync() waits on the SAI DMA half-buffer counter --
 * so the length still has to be right. audio_start_playing(len) fills
 * audiobuffer_dma[AUDIO_BUFFER_LENGTH * 2] (gw_audio.h:15,23), so len must
 * never exceed AUDIO_BUFFER_LENGTH == 1077. 48000/50 = 960 is the largest value
 * any reachable rate produces, but the clamp stays: overrunning this buffer is
 * memory corruption past the end of .audio, not a glitch, and the next person
 * to add a slower mode (the half-rate modes want 48000/25 = 1920) must hit the
 * clamp rather than the corruption. */
#define DOS_AUDIO_LEN(hz) ((uint32_t)AUDIO_SAMPLE_RATE / (uint32_t)(hz))
_Static_assert(DOS_AUDIO_LEN(50) <= AUDIO_BUFFER_LENGTH,
               "DOS audio DMA length would overrun audiobuffer_dma");

/* Apply the selected rate: panel PLL, guest frame period, audio pacer.
 *
 * Deliberately NOT lcd_init() and NOT mpu_set_lcd_pool_uncached_range():
 * lcd_set_refresh_rate() reprograms PLL3 only (gw_lcd.c:548-591) and neither
 * the framebuffer footprint nor its addresses move, while re-running the MPU
 * setup would rewrite regions 3-6 underneath this very overlay (main_dos.c
 * warns about exactly that). gwenesis switches the same way
 * (main_gwenesis.c:162-180). */
void dos_screen_apply_rate(void)
{
    uint32_t hz = dos_screen_hz_sel;

    dos_cpu_derive();

    /* Do not move the pixel clock with a swap in flight. */
    if (lcd_is_swap_pending())
        lcd_sleep_while_swap_pending();

    lcd_set_refresh_rate(hz);
    common_emu_state.frame_time_10us = (int16_t)(100000u / hz);

    {
        uint32_t len = DOS_AUDIO_LEN(hz);
        if (len > AUDIO_BUFFER_LENGTH) len = AUDIO_BUFFER_LENGTH;
        audio_start_playing((uint16_t)len);
    }

    /* Without this the first frame after a change looks like a huge stall and
     * the frame integrator clamps into skip mode (common.c:141-155). */
    common_emu_frame_loop_reset();
}

/* Restore the persisted rate. Must run before the first dos_screen_apply_rate()
 * so the core comes up at the user's rate rather than switching one frame in.
 *
 * NOTE: today this always yields the default, because the settings store is a
 * stub in this fork -- see DOS_SCREEN_HZ_KEY above before assuming a bug here.
 * Verified on hardware: set(50) immediately followed by get() returned -1.
 *
 * The stored value is the Hz number, not the table index: an index would
 * silently mean a different rate if the table ever gains or reorders an entry,
 * and a stale index is unrecoverable for the user. An unrecognised Hz falls back
 * to the default rather than being trusted -- lcd_set_refresh_rate() would
 * ignore it and leave the panel and the frame budget disagreeing. */
void dos_screen_freq_init(void)
{
    int32_t hz = odroid_settings_app_int32_get(DOS_SCREEN_HZ_KEY,
                                               (int32_t)DOS_SCREEN_HZ_DEFAULT);
    for (int i = 0; i < DOS_SCREEN_RATE_COUNT; i++) {
        if (dos_screen_rates[i] == (uint16_t)hz) {
            dos_screen_rate_idx = i;
            dos_screen_hz_sel   = dos_screen_rates[i];
            return;
        }
    }
    /* Unknown/absent: leave the compiled-in default in place. */
}

bool dos_screen_freq_update_cb(odroid_dialog_choice_t *option,
                               odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    int max = DOS_SCREEN_RATE_COUNT - 1;
    bool changed = false;

    if (event == ODROID_DIALOG_PREV) {
        dos_screen_rate_idx = dos_screen_rate_idx > 0 ? dos_screen_rate_idx - 1 : max;
        changed = true;
    }
    if (event == ODROID_DIALOG_NEXT) {
        dos_screen_rate_idx = dos_screen_rate_idx < max ? dos_screen_rate_idx + 1 : 0;
        changed = true;
    }

    if (changed) {
        dos_screen_hz_sel = dos_screen_rates[dos_screen_rate_idx];
        odroid_settings_app_int32_set(DOS_SCREEN_HZ_KEY, (int32_t)dos_screen_hz_sel);
        /* Applied immediately, like every other option row in this tree. It is a
         * PLL3 write plus an audio restart; there is nothing to defer to a core
         * restart. Safe from inside the menu: the overlay repaints through the
         * core's repaint callback afterwards, and apply_rate() drains any
         * pending swap before moving the pixel clock. */
        dos_screen_apply_rate();
    }

    snprintf(option->value, DOS_SCREEN_FREQ_VALUE_LEN, "%u", (unsigned)dos_screen_hz_sel);
    return event == ODROID_DIALOG_ENTER;
}

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
 * slack and the frame integrator starts declaring skips.
 *
 * It is a percentage rather than an absolute deadline precisely so it survives
 * the refresh rate becoming user-selectable: the period it scales is
 * SystemCoreClock / dos_screen_hz(), so the duty cycle is the same at 50 as at
 * 75 and the sweep below stays valid. The us figures quoted are at 60 Hz.
 *
 * 88 was measured on hardware at the MS-DOS idle prompt (cpi 204). It was 85,
 * which made MAX *slower than the 286 profile*: 85% of 16667us is a 14167us
 * deadline, a chunk costs ~1090us, so the loop stopped after 13 chunks =
 * ipf 19500 against the 286 profile's 21000. Sweep (ipf / cpu% / blit count of
 * 64): 85 -> 19500 / 85% / 61-64; 88 -> 20950 / 92% / 60-64; 90 -> 21000 / 92%
 * / 60-64; 92 -> 21300 / 93% / 50-64 (dips under putchar load); 94 -> 22450 /
 * 98% / 16-30, i.e. two thirds of the visual frames dropped. frames=64 and rs=0
 * at every point -- as ever, the blit count is the frame-drop indicator, not
 * frames=.
 *
 * 90 is the highest fully clean step; 88 is one step back for margin and costs
 * nothing, because chunk quantisation puts 88 and 90 on the same 14-chunk
 * operating point. A 15th chunk needs a deadline past ~92.3%, so 88 sits 4.3
 * points below the next step up and 6 points below the measured failure.
 *
 * The chunk loop itself was left alone deliberately. Trimming the final chunk
 * to fit the deadline would remove the up-to-one-chunk overshoot, but the
 * overshoot is what currently reaches 92% CPU from an 88% deadline -- a precise
 * loop would need the constant raised to the same measured-safe 92% to break
 * even. No throughput to gain, so no extra arithmetic on the hot path. */
#define DOS_MAX_FRAME_PCT 88

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
static uint32_t prof_int8_due0, prof_int8_fired0, prof_int8_resync0;

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
    dos_cpu_derive();
    prof_cpu = prof_blit = prof_idle = 0;
    prof_frames = prof_insn = 0;
    prof_win_ms  = (uint32_t)HAL_GetTick();
    prof_win_cyc = dos_cyc();
    prof_putchar0 = dos_putchar_count;
    prof_int8_due0 = dos_int8_due;
    prof_int8_fired0 = dos_int8_fired;
    prof_int8_resync0 = dos_int8_resync;
}

const char *dos_cpu_profile_name(void)
{
    return dos_cpu_profiles[dos_cpu_profile_idx].name;
}

int dos_cpu_run_frame(void)
{
    const unsigned int budget = dos_insn_per_frame;
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
        uint32_t deadline_cyc = (uint32_t)((SystemCoreClock / dos_screen_hz_sel)
                                           / 100u * DOS_MAX_FRAME_PCT);
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
     * the frame period directly rather than only as a ratio. That period is
     * 1,000,000 / dos_screen_hz() us -- 16,667 at 60 Hz, 20,000 at 50,
     * 13,333 at 75 -- not a constant any more. */
    {
        uint32_t mhz = SystemCoreClock / 1000000u;   /* 280 stock */
        out->blits        = prof_blits;
        out->mhz          = mhz;
        out->cpu_us_frame = (uint32_t)(prof_cpu / prof_frames / mhz);
        out->blit_us      = prof_blits ? (uint32_t)(prof_blit / prof_blits / mhz) : 0;
        out->cyc_per_insn = prof_insn ? (uint32_t)(prof_cpu / prof_insn) : 0;
        out->putchars     = dos_putchar_count - prof_putchar0;
        prof_putchar0     = dos_putchar_count;
        out->int8_due     = dos_int8_due    - prof_int8_due0;
        out->int8_fired   = dos_int8_fired  - prof_int8_fired0;
        out->int8_resync  = dos_int8_resync - prof_int8_resync0;
        prof_int8_due0    = dos_int8_due;
        prof_int8_fired0  = dos_int8_fired;
        prof_int8_resync0 = dos_int8_resync;
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

    dos_cpu_derive();

    const dos_cpu_profile_t *p = &dos_cpu_profiles[dos_cpu_profile_idx];
    if (p->insn_per_sec) {
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
