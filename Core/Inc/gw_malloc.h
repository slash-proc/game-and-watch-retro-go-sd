#ifndef _GW_MALLOC_H_
#define _GW_MALLOC_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

extern uint32_t ram_start;

/* Generation counter for the bump allocators below.
 *
 * ram_malloc()/ahb_malloc()/ahb_calloc() cannot free, so the one event that
 * invalidates every outstanding pointer at once is ahb_init() -- which the
 * launcher calls both immediately before and immediately after every core run
 * (emulator_start, rg_emulators.c). A holder that survives a core run holds a
 * pointer into memory the core has since used, which historically presented as
 * "a random BusFault" in whatever unrelated code dereferenced it first,
 * several frames away from the mistake.
 *
 * Stamp this beside any pointer kept across a core launch and compare before
 * use. tab_t.alloc_generation / gui_get_tab() is the reference use.
 *
 * Starts at 0 == "no allocation was ever valid", so a stamp sitting in zeroed
 * .bss -- or in RAM a core has just wiped -- can never equal a live
 * generation. */
extern uint32_t ram_alloc_generation;

void ahb_init();
void *ahb_malloc(size_t size);
void *ahb_only_malloc(size_t size);
void *ahb_calloc(size_t count,size_t size);

void itc_init();
void *itc_malloc(size_t size);
void *itc_calloc(size_t count,size_t size);

size_t ram_get_free_size();
void *ram_malloc(size_t size);
void *ram_calloc(size_t count,size_t size);

/* DTCM stdlib heap (_heap_start.._heap_end). Use for emulator overlays
 * (PICO-8 p8ram, PCE work RAM, etc.) that need free/realloc. */
void *dtcm_malloc(size_t size);
void *dtcm_calloc(size_t count, size_t size);
void dtcm_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif
