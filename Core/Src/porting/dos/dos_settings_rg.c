/* dos_settings_rg.c -- see dos_settings_rg.h, and dos_settings.h (in
 * external/8086tiny) for the format and the reasoning behind it.
 */

#include <stdio.h>
#include <string.h>

#include "odroid_system.h"
#include "odroid_overlay.h"
#include "dos_settings.h"      /* the format, from external/8086tiny */
#include "dos_settings_rg.h"

/* The per-title 32-bit-stack switch, defined in 8086tiny.c beside the macro it
 * replaces. Declared here rather than in a shared header because it is the only
 * thing this file needs out of the emulator and 8086tiny.c ships no header at
 * all -- main_dos.c declares `mem[]`, `inst_counter` and `reg_ip` the same way,
 * for the same reason. */
extern unsigned char dos_ss_big_cfg;

/* Fork-only, and NOT declared in odroid_system.h -- the vendored upstream header
 * declares only the strdup'ing odroid_system_get_path(). The buffer form is what
 * this file wants (a path that lives for three statements has no business on the
 * heap, and every caller of the strdup form has to remember to free it), so it
 * is declared here rather than added to a shared header: three other changes are
 * in flight in this tree and a new line in odroid_system.h buys a conflict for
 * no benefit. Definition: Core/Src/porting/odroid_system.c:236. */
extern void odroid_system_get_path_to_buf(emu_path_type_t type,
                                          const char *romPath,
                                          char *buf, int buf_size);

/* Whether load() has run. Guards apply()/save() against being called on a
 * table that still holds the PREVIOUS title's answers -- which cannot happen
 * today (the core is loaded fresh per launch) but is one refactor away from
 * being able to, and "wrong title's settings" is not a failure anyone would
 * spot from the outside. */
static bool dos_settings_ready;

/* ---- Where the file lives -------------------------------------------------
 *
 * `<saves>/<rom-relative-path>.dosset`, i.e. exactly where a title's .sram and
 * .sav already live, keyed on the title's PATH and nothing else.
 *
 * WHY THE PATH AND NOT THE CONTENT. The .dosmeta sidecar next door is keyed on
 * (dsk_size, dsk_crc) so that regenerating a .dsk invalidates it -- correct,
 * because a measurement taken against different bytes is a wrong number. A
 * PREFERENCE is the opposite: rebuilding the disk image must not throw away what
 * the user chose. Keying settings on content would reproduce, exactly, the trap
 * where mach_kb silently zeroed itself on regeneration and four subsystems
 * switched off with nothing logged (docs/traps.md).
 *
 * WHY IT IS DERIVED FROM ODROID_PATH_SAVE_SRAM RATHER THAN A NEW PATH TYPE.
 * The rom-relative trimming, the ODROID_BASE_PATH_SAVES prefix and the
 * RG_PANIC on a nonsense path all live in one switch in odroid_system.c, and
 * duplicating that here would be a second copy to keep in step. Asking for the
 * .sram path and swapping the suffix reuses all of it and touches no shared
 * file -- which matters more than usual right now, with three other changes in
 * flight in this tree. If a `ODROID_PATH_DOS_SETTINGS` is ever added to that
 * enum, this function is the only thing that changes.
 *
 * Returns false if the path could not be built, in which case there are no
 * settings and that is not an error. */
#define DOS_SETTINGS_SUFFIX ".dosset"

static bool dos_settings_path(const char *rom_path, char *out, size_t cap)
{
    size_t n;

    if (!rom_path || !out || cap == 0)
        return false;

    odroid_system_get_path_to_buf(ODROID_PATH_SAVE_SRAM, rom_path, out, (int)cap);
    n = strlen(out);

    /* Belt and braces: if the shell ever stops ending that path in ".sram" this
     * must not silently start writing over somebody's save file. */
    if (n < 5 || strcmp(out + n - 5, ".sram") != 0) {
        printf("DOS: settings path '%s' is not a .sram path -- no settings\n", out);
        return false;
    }
    if (n - 5 + sizeof DOS_SETTINGS_SUFFIX > cap)
        return false;
    strcpy(out + n - 5, DOS_SETTINGS_SUFFIX);
    return true;
}

void dos_settings_rg_load(const char *rom_path)
{
    char path[RG_PATH_MAX + 1];
    unsigned char buf[DOS_SETTINGS_MAX_BYTES];
    size_t n = 0;
    FILE *f;

    /* Unconditionally, before anything can go wrong below: defaults. */
    dos_settings_reset();
    dos_settings_ready = true;

    if (!dos_settings_path(rom_path, path, sizeof path))
        return;

    /* OPEN, READ, CLOSE. See the header on why no handle survives this call. */
    f = fopen(path, "rb");
    if (f) {
        n = fread(buf, 1, sizeof buf, f);
        fclose(f);
    }
    /* n == 0 covers both "no file" and "empty file", and dos_settings_load()
     * treats that as EABSENT and prints nothing -- a title nobody has configured
     * is the common case, not an event worth a line in a 4 KB log ring. */
    dos_settings_load(buf, (unsigned long)n);
}

void dos_settings_rg_apply(void)
{
    int ss;

    if (!dos_settings_ready)
        return;

    ss = dos_settings_get(DOS_SET_SS_BIG_STACK);
    dos_ss_big_cfg = (unsigned char)(ss ? 1 : 0);

    /* SAID OUT LOUD, ONCE, WHEN IT IS NOT THE DEFAULT. A per-title setting that
     * silently changes how the CPU behaves is the thing that turns a bug report
     * into an afternoon. The default case prints nothing, because a line per
     * launch that says "everything is normal" is how a log ring that wraps at
     * 4 KB loses the line that mattered. */
    if (!dos_ss_big_cfg)
        printf("DOS: settings -- 32-bit stack OFF for this title"
               " (SS.B will be ignored)\n");

    /* HOOKS. dos_settings_get(DOS_SET_CPU_PROFILE / _MOUSE_MODE / _MOUSE_CURSOR
     * / _BOOT_SNAPSHOT) already return persisted, range-checked values. Wiring
     * one up is a line here plus a menu row; it is deliberately NOT done, because
     * "the value is persisted" and "the value is honoured" are different claims
     * and this tree has three subsystems on record that passed their tests while
     * no guest could reach them. */
}

void dos_settings_rg_save(void)
{
    char path[RG_PATH_MAX + 1];
    unsigned char buf[DOS_SETTINGS_MAX_BYTES];
    unsigned long n;
    FILE *f;

    if (!dos_settings_ready || !dos_settings_dirty())
        return;   /* see the header: writing an unchanged file is not free */

    n = dos_settings_save(buf, sizeof buf);
    if (!n)
        return;

    /* NULL == "the app's current ROM" (odroid_system.c:86, `_romPath ?:
     * currentApp.romPath`), which is the same title load() was given. Passing
     * NULL rather than caching the path keeps a RG_PATH_MAX buffer out of
     * .overlay_dos_bss, which is the tightest budget in this core. */
    if (!dos_settings_path(NULL, path, sizeof path))
        return;

    f = fopen(path, "wb");
    if (!f) {
        /* LOUD. This is the one failure a user can actually observe -- they
         * changed a setting and it did not stick -- so it must not be silent,
         * and the likely cause (the fopen budget) is worth naming at the point
         * of failure rather than in a document nobody is reading at the time. */
        printf("DOS: settings -- cannot write %s"
               " (card full, or out of file handles?)\n", path);
        return;
    }
    if (fwrite(buf, 1, n, f) != n)
        printf("DOS: settings -- short write to %s\n", path);
    fclose(f);
}

/* ---- Options menu row ----------------------------------------------------- */

bool dos_settings_ssbig_update_cb(odroid_dialog_choice_t *option,
                                  odroid_dialog_event_t event, uint32_t repeat)
{
    (void)repeat;
    int v = dos_settings_get(DOS_SET_SS_BIG_STACK);

    if (event == ODROID_DIALOG_PREV || event == ODROID_DIALOG_NEXT) {
        dos_settings_set(DOS_SET_SS_BIG_STACK, v ? 0 : 1);
        /* PERSIST IMMEDIATELY, in the callback, exactly as the screen-frequency
         * row does. There is no core-exit hook this core reliably reaches --
         * the launcher can tear a core down through a savestate, a sleep or a
         * reset -- and a setting that only survives one of those three exits is
         * worse than one that survives none, because it is intermittent. One
         * fopen/fwrite/fclose per keypress on a menu row is affordable. */
        dos_settings_rg_save();
    }

    /* NOT dos_settings_rg_apply(): see 8086tiny.c's dos_seg_load_pm(). The
     * width of the stack is latched into the descriptor when SS is loaded, so
     * flipping this mid-run would take effect at the next SS reload and not
     * before -- i.e. at an unpredictable moment, which is the worst of both.
     * The value string says "restart" rather than pretending otherwise; a menu
     * row that looks like it applied and did not is how a working feature gets
     * reported as broken. */
    v = dos_settings_get(DOS_SET_SS_BIG_STACK);
    snprintf(option->value, DOS_SETTINGS_VALUE_LEN, "%s",
             v ? "On (restart)" : "Off (restart)");
    return event == ODROID_DIALOG_ENTER;
}
