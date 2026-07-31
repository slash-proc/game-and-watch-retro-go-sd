#ifndef _DOS_AUDIO_H_
#define _DOS_AUDIO_H_

/* PC-speaker audio for the MS-DOS core. Design lives in the submodule:
 * external/8086tiny/docs/audio-roadmap.md and docs/audio/01,02. */

/* Call once at core start, before the frame loop, after audio_start_playing(). */
void dos_audio_init(void);

/* Fill ONE SAI DMA half-buffer with the current speaker output.
 *
 * Must be called exactly once per half-buffer edge, immediately before the
 * matching common_emu_sound_sync(): it samples and clears the emulator's
 * spkr_en latch (dos_spkr_take()), so calling it twice per edge drops short
 * beeps and calling it zero times leaves the speaker stuck on. */
void dos_audio_submit(void);

#endif /* _DOS_AUDIO_H_ */
