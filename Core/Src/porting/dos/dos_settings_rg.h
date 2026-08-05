/* dos_settings_rg.h -- the retro-go binding for the per-title settings file.
 *
 * dos_settings.c (external/8086tiny) owns the FORMAT and the policy: defaults,
 * ranges, what a corrupt file means, what is written and what is omitted. It
 * deliberately does no I/O and knows nothing about this firmware, which is what
 * lets test286/runsettings.sh test all of that without a machine.
 *
 * THIS file is the other half, and it is small on purpose: work out the path,
 * read the bytes, hand them over; and on the way back, take the bytes and write
 * them. Everything here is cold path -- once at launch, once per menu change.
 *
 * FILE HANDLES. Open, read, close. Never held. MAX_OPEN_FILES is 8 across the
 * whole firmware (Core/Src/syscalls.c:51), the DOS core already holds three for
 * its entire run (hard disk, floppy, BIOS blob), the pagefile wants another, and
 * exhaustion is SILENT -- every other fopen() in the firmware starts returning
 * NULL and the UI degrades with nothing logged (the whole launcher once rendered
 * as diamonds because of it). Both functions below take at most one handle and
 * give it back before they return.
 */

#ifndef DOS_SETTINGS_RG_H
#define DOS_SETTINGS_RG_H

#include <stdbool.h>
#include <stdint.h>
#include "odroid_overlay.h"

/* Load this title's settings, or leave every setting at its default.
 *
 * CANNOT FAIL IN A WAY THAT MATTERS. There is no return value because there is
 * no decision for the caller to make: a missing file, an unreadable card, a
 * corrupt header and a truncated write all mean "defaults", and none of them is
 * a reason not to start the title. dos_settings_load() prints one line naming
 * whichever it was.
 *
 * Call BEFORE dos_cpu_init(): dos_ss_big_cfg is read on the segment-load path,
 * so a setting applied after the guest has loaded SS would not take effect until
 * the next reload -- which is the kind of half-applied state that makes a
 * feature look intermittent. */
void dos_settings_rg_load(const char *rom_path);

/* Push the live settings into the emulator. Separate from load() so that the
 * menu callback can re-apply after a change without re-reading the card, and so
 * that "what the file said" and "what the emulator was told" are two steps that
 * can be reasoned about separately. */
void dos_settings_rg_apply(void);

/* Write the file, but only if anything actually differs from what was loaded.
 *
 * THE `dirty` GUARD IS NOT AN OPTIMISATION. Without it every title anyone ever
 * launched would grow a .dosset recording nothing, and -- worse -- a title that
 * had merely been launched would be pinned to today's defaults for ever, since
 * a written default is indistinguishable from a chosen one. Changing a default
 * in firmware has to move every title that never overrode it. */
void dos_settings_rg_save(void);

/* Options-menu row: "32-bit stack", the first per-title setting.
 *
 * The other settings named in dos_settings.h (CPU profile, mouse mode, mouse
 * cursor, snapshot boot) have NO row here yet, deliberately. The persistence
 * mechanism is generic; deciding how each of those is presented belongs with
 * whoever implements it. */
bool dos_settings_ssbig_update_cb(odroid_dialog_choice_t *option,
                                  odroid_dialog_event_t event, uint32_t repeat);

#define DOS_SETTINGS_VALUE_LEN 16

#endif
