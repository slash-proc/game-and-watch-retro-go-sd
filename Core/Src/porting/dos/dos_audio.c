/* PC-speaker audio for the MS-DOS (8086tiny) core.
 *
 * Scope is deliberately one square wave and nothing else -- see
 * external/8086tiny/docs/audio-roadmap.md. AdLib, Sound Blaster and direct
 * port-0x61 bit-banged speech are all explicitly out of scope for v1; the last
 * of those cannot be done from a per-frame sample of the state at all, it needs
 * the port write history with timing.
 *
 * The emulator side is dos_spkr_take() in 8086tiny.c, which owns the spkr_en
 * latch. This file owns the waveform and the DMA buffer. The generator itself
 * is in dos_audio_wave.h so it can be unit-tested on the host
 * (external/8086tiny/test286/host_audio.c) -- the arithmetic from divisor to
 * samples is the part most likely to be silently wrong, and a wrong pitch is
 * not visible in a screenshot.
 */

/* Provided by 8086tiny.c -- see
 * external/8086tiny/docs/audio/03-sound-blaster.md section 4. */
extern unsigned int dos_sb_mix(short *buf, unsigned int len,
                              unsigned int out_rate, int amp);

#include "gw_audio.h"
#include "common.h"
#include "dos_audio.h"
#include "dos_audio_wave.h"

/* Provided by 8086tiny.c. Reading it clears the speaker latch -- see its
 * comment block before adding a second caller. */
extern unsigned int dos_spkr_take(void);

/* Output level.
 *
 * common_emu_sound_get_volume() returns 0..255 from the launcher's volume
 * table. Tama scales a square wave with `factor * SHRT_MAX >> 10`, i.e. 25% at
 * full volume, with the comment "100% is REALLY, REALLY loud"
 * (main_tama.c:298). A PC-speaker square is a 50%-duty full-swing wave with no
 * envelope at all, so it carries more energy than that, and DOS beeps are short
 * and startling rather than continuous music. Start one binary step lower --
 * >> 11, about 12.5% -- which docs/audio/02-output-bridge.md's open question
 * recommends ("at or below tama's 25%, adjust on hardware"). gwemu is not a
 * reliable guide to perceived loudness, so this is the number to revisit first
 * if the device says otherwise. */
#define DOS_SPKR_VOL_SHIFT 11

/* Fixed-point phase, 32-bit fraction of one cycle. Two words of state is the
 * entire RAM cost of this feature -- the overlay's BSS headroom is a few KB, so
 * anything buffer-shaped was never an option. Samples are written straight into
 * the firmware's DMA buffer; we allocate none of our own. */
static uint32_t dos_spkr_phase;
static uint8_t  dos_spkr_was_on;

void dos_audio_init(void)
{
    dos_spkr_phase  = 0;
    dos_spkr_was_on = 0;
    /* Drain whatever the guest latched during BIOS POST (MS-DOS and FreeDOS
     * both beep on boot) so the first rendered frame starts from a known
     * state rather than inheriting a stale gate. */
    (void)dos_spkr_take();
}

void dos_audio_submit(void)
{
    /* Sample-and-clear the latch FIRST, unconditionally. This has to happen on
     * every call including the muted and clamped paths below: dos_spkr_take()
     * is the only thing that clears spkr_en, so an early return that skips it
     * leaves the speaker latched on forever and the next unmuted frame plays a
     * tone the guest stopped asking for long ago. */
    uint32_t divisor = dos_spkr_take();
    uint32_t inc     = dos_spkr_phase_inc(divisor, AUDIO_SAMPLE_RATE);

    /* Anti-click, option (2) from docs/audio/02-output-bridge.md: restart the
     * wave at phase 0 on each rising edge of the gate, so a beep always begins
     * at the same point in the cycle instead of stepping from an arbitrary
     * mid-swing value. One assignment, and it removes the worst of the
     * transient. An amplitude ramp (option 3) is deliberately not implemented;
     * real PC speakers clicked, and the remaining step is at the END of a beep,
     * where the wave is cut wherever it happens to be. If hardware says that is
     * objectionable, a few-sample ramp is the next step. */
    if (inc && !dos_spkr_was_on)
        dos_spkr_phase = 0;
    dos_spkr_was_on = inc ? 1 : 0;

    /* Muted still had to take the latch above; now just leave silence. */
    if (common_emu_sound_loop_is_muted()) { /* clears the active buffer itself */
        /* THE MUTED PATH MUST STILL PUMP THE SOUND BLASTER. A single-cycle DMA
         * transfer is a CURSOR advanced by dos_sb_mix(); if it is not advanced
         * the transfer never completes, the end-of-transfer IRQ never fires, and
         * a guest waiting on it HANGS -- silently, and ONLY when muted. Same
         * rule as the speaker latch above, found the same way: mutate.sh ran
         * with no audio sink and the guest sat at IRQ=00 CNT=00FF forever.
         * amp 0 renders silence while still advancing the cursor.
         * See external/8086tiny/docs/audio/03-sound-blaster.md section 4. */
        dos_sb_mix(audio_get_active_buffer(), audio_get_buffer_length(),
                   AUDIO_SAMPLE_RATE, 0);
        return;
    }

    int16_t *buf = audio_get_active_buffer();
    uint16_t len = audio_get_buffer_length();

    /* THE 1077-SAMPLE CEILING.
     *
     * audiobuffer_dma is int16_t[AUDIO_BUFFER_LENGTH * 2] (gw_audio.c:6) and
     * NOTHING in the firmware bounds-checks it -- audio_start_playing_full_length()
     * hands the length straight to HAL_SAI_Transmit_DMA(). Writing past the end
     * walks out of the 8 KB .audio reserve at the top of AHB SRAM and into the
     * savestate slot table; this was a real, painful bug in the gnw-doom port.
     *
     * audio_get_buffer_length() derives from the firmware's own configured
     * length so it should already be in range, and dos_cpu.c clamps what it
     * passes to audio_start_playing(). This clamp is the third one on purpose:
     * it is the only one at the actual point of the write, it costs a compare,
     * and the failure mode of not having it is silent memory corruption rather
     * than an audio glitch. Do not remove it because "the caller is careful". */
    if (len > AUDIO_BUFFER_LENGTH)
        len = AUDIO_BUFFER_LENGTH;

    if (!inc) {
        /* Silence. Cheaper than running the loop, and matches the tree's habit
         * of clearing the whole half in one go rather than storing zeroes. */
        audio_clear_active_buffer();
        /* Speaker silent does not mean the card is: pump the cursor. */
        dos_sb_mix(audio_get_active_buffer(), audio_get_buffer_length(),
                   AUDIO_SAMPLE_RATE,
                   (int16_t)((common_emu_sound_get_volume() * (int32_t)INT16_MAX)
                             >> DOS_SPKR_VOL_SHIFT));
        return;
    }

    int16_t amp = (int16_t)((common_emu_sound_get_volume() * (int32_t)INT16_MAX)
                            >> DOS_SPKR_VOL_SHIFT);

    dos_spkr_render(buf, len, inc, amp, &dos_spkr_phase);
    /* The Sound Blaster mixes into the SAME buffer: the two are independent
     * sources and a DMA-audio title usually leaves the speaker gated off. */
    dos_sb_mix(buf, len, AUDIO_SAMPLE_RATE, amp);
}
