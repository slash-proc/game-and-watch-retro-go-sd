#include <stdint.h>
#include <stdbool.h>

typedef void (*file_progress_cb_t)(uint32_t total_size, uint32_t total_processed, uint8_t progress);

void flash_alloc_reset();
uint8_t *store_file_in_flash(const char *file_path, uint32_t *file_size_p, bool byte_swap, file_progress_cb_t progress_cb);

// Session reservation (opt-in; the cache behaves exactly as before unless used).
// A running app can reserve [cache_base, floor) as un-evictable — e.g. a large
// shared asset blob staged once that must survive while smaller blobs are cycled.
// flash_cache_reserve_floor() marks everything below `floor` off-limits to the
// round-robin AND parks the recyclable-pool cursor at `floor`, so the next file
// staged lands there deterministically (re-call before each swap to overwrite the
// previous one). flash_cache_release() returns the whole cache to the system and
// rewinds the cursor to the base. The floor is RAM-only state — it auto-clears on
// reboot/app exit, so a crashed app never leaves a stale reservation.
void flash_cache_reserve_floor(uint32_t floor);
void flash_cache_release(void);
