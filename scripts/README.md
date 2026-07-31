The scripts in this directory are intended to be executed from the repository root. e.g. `./scripts/saves_erase.sh`.

## The ones you are most likely to want

| Script | What it is for | Documented in |
|---|---|---|
| `get_toolchain.sh` | Installs the **pinned** `arm-none-eabi` toolchain (must match `ARM_COMPILER_VERSION` in `Dockerfile`) into `~/opt`, no admin rights, Linux and macOS. Verifies the download — note Arm's checksum files are misnamed, so it picks the algorithm by digest length rather than trusting the extension. | [docs/gwemu.md](../docs/gwemu.md) — *Dependencies* |
| `run_gwemu.sh` | Launches the firmware under gwemu with GDB attached; log forwarding and fault trapping are always on. `--keep-on-fault` resumes after a fault so the firmware's own BSOD paints (and the exit code stops being a fault signal). | [docs/gwemu.md](../docs/gwemu.md) |
| `make_sdcard_image.py` | Builds the emulated SD card image. `--size-mb N` fixes the size; `--fit DIR` (repeatable) measures the content, rounds each file up to the FAT cluster size, and snaps to the next real card size (256M/512M/1G/2G/4G/8G). Replaces `parted`, which is Linux-only. | [docs/gwemu.md](../docs/gwemu.md) |
| `gwharness.py` | Replays a gwemu `.tl` timeline **on real hardware** and captures the log in one probe session — `gnwmanager monitor` and `remote_input.py` spawn their own OpenOCD and evict each other, so this is the only way to have buttons and logs at once. Needs firmware built with `REMOTE_INPUT=1`. | [docs/gwemu.md](../docs/gwemu.md) — *When the emulator and hardware disagree* |
| `remote_input.py`, `remote_control.py` | Button injection and device control on their own, for when nothing needs to capture at the same time. | — |
| `gwemu_log.gdb` | The GDB script `run_gwemu.sh` runs: breaks on `_write()` to forward logs and on `common_fault_handler_c()` / `__assert_func` to dump faults. | [docs/gwemu.md](../docs/gwemu.md) |
| `size.sh`, `extflash_size.sh` | Build size reporting, invoked from the Makefile. | `make help` |
| `gen_frogfs_image.py`, `gen_littlefs_image.py`, `sd_cores_pack.py`, `update_rom_files.sh` | Media generation for the two build variants. | [docs/gwemu.md](../docs/gwemu.md) — *The two build variants* |
| `gen_release_package.sh`, `update_gittag.sh` | Release packaging and tag stamping, invoked from the Makefile. | — |

Core-specific developer tooling lives with its core, not here. For MS-DOS that is
`external/8086tiny/push-testdisk.sh` (push the FreeDOS test floppy to a device, or into a
gwemu SD image via `mcopy` at the `@@1M` partition offset) and `external/8086tiny/test286/`
(the host harness and its eleven tests) — see
[external/8086tiny/docs/testing.md](../external/8086tiny/docs/testing.md).