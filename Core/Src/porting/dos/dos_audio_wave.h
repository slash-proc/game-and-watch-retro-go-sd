#ifndef _DOS_AUDIO_WAVE_H_
#define _DOS_AUDIO_WAVE_H_

/* PC-speaker square-wave generation -- the pure arithmetic half of dos_audio.c.
 *
 * Split into its own header for one reason: it is the only part of the audio
 * path that can be tested without hardware. dos_audio.c pulls in gw_audio.h ->
 * main.h -> the whole STM32 HAL, which cannot be built on the host; this header
 * depends on nothing but <stdint.h>, so external/8086tiny/test286/host_audio.c
 * compiles it natively and checks real divisors against known frequencies.
 * Keep it that way -- no HAL, no firmware headers, no globals.
 *
 * Design: docs/audio/01-pc-speaker.md in the 8086tiny submodule.
 */

#include <stdint.h>

/* PIT input clock. 14.31818 MHz / 12, the number every PC tone table divides. */
#define DOS_PIT_HZ 1193182u

/* Numerator of the phase increment, in 32-bit-fraction-per-sample units:
 *
 *     inc = 2^32 * f_hz / rate,   f_hz = DOS_PIT_HZ / divisor
 *         = (2^32 * DOS_PIT_HZ / rate) / divisor
 *
 * A phase accumulator rather than upstream's `54 * counter / divisor`. That 54
 * is 2*1193182/44100 rounded -- it is baked to SDL's 44.1 kHz and we run at 48
 * kHz, so copying it lands every note ~1.5 semitones sharp (see the roadmap).
 * Here the sample rate is a visible argument instead of a magic constant.
 *
 * The 64-bit divide runs once per buffer fill (60-120 times a second), not per
 * sample, so it is cheaper than upstream's per-sample mul+div as well.
 */
#define DOS_PHASE_NUM(rate) (((uint64_t)DOS_PIT_HZ << 32) / (uint64_t)(rate))

/* Below this divisor the tone is above Nyquist and the accumulator would fold
 * it back down as a loud, tuneless alias. Real speakers could not reproduce
 * these either, so they are silence. 1193182 / 24000 = 49.7 -> divisor < 50. */
#define DOS_SPKR_MIN_DIVISOR(rate) (uint32_t)((2u * DOS_PIT_HZ) / (uint32_t)(rate))

/* Phase increment for one PIT divisor, or 0 for "no tone".
 * divisor 0 means the guest never programmed channel 2. */
static inline uint32_t dos_spkr_phase_inc(uint32_t divisor, uint32_t rate)
{
    if (divisor == 0 || divisor < DOS_SPKR_MIN_DIVISOR(rate))
        return 0;
    return (uint32_t)(DOS_PHASE_NUM(rate) / (uint64_t)divisor);
}

/* The frequency a divisor represents, for tests and logging only. */
static inline uint32_t dos_spkr_freq_hz(uint32_t divisor)
{
    return divisor ? (DOS_PIT_HZ + divisor / 2) / divisor : 0;
}

/* Fill `len` samples of a 50% square wave, advancing *phase.
 *
 * Sign comes from the top bit of the accumulator, so the wave starts HIGH at
 * phase 0 -- which is what makes the anti-click reset in dos_audio.c work: the
 * caller zeroes *phase on every rising edge of the speaker gate, so a beep
 * always begins at the same point of the cycle instead of mid-swing. That is
 * option (2) from docs/audio/02-output-bridge.md; no amplitude ramp.
 *
 * `amp` is the peak magnitude, already attenuated by the caller.
 */
static inline void dos_spkr_render(int16_t *buf, uint32_t len,
                                   uint32_t inc, int16_t amp, uint32_t *phase)
{
    uint32_t p = *phase;
    if (inc == 0 || amp == 0) {
        for (uint32_t i = 0; i < len; i++)
            buf[i] = 0;
        return;                 /* phase deliberately left where it was */
    }
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = (p & 0x80000000u) ? (int16_t)-amp : amp;
        p += inc;
    }
    *phase = p;
}

#endif /* _DOS_AUDIO_WAVE_H_ */
