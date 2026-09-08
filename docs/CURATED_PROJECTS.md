# Curated projects

Emulator cores and homebrew that are known to work with this firmware and that
publish under the [GWRG distribution spec](https://github.com/slash-proc/gwrg-dist-spec).

CI parses this file into `dist/<tag>/projects.json`, published alongside the
firmware manifest. An installer reads that file and follows each project's
`versions.json` from there; nothing here is fetched by the firmware itself.

**Origins are development repositories** and will be repointed at the upstream
projects before release.

Each row's `versions.json` is `https://slash-proc.github.io/<repo>/dist/versions.json`.

## Emulator cores

| Project | Title | Systems | Repo |
|---|---|---|---|
| `nes-fceu` | FCEUmm | Nintendo Entertainment System | `slash-proc/fceumm-retro-go-sd` |
| `tgb` | TGB Dual | Game Boy, Game Boy Color | `slash-proc/tgb-dual-retro-go-sd` |
| `sms` | SMS Plus GX | Master System, Game Gear, SG-1000, ColecoVision | `slash-proc/SMSPlusGX-retro-go-sd` |
| `pce` | PCE-GO | PC Engine, PC Engine CD | `slash-proc/pce-go-retro-go-sd` |
| `md` | Gwenesis | Sega Genesis | `slash-proc/gwenesis-retro-go-sd` |
| `snes` | lakesnes | Super Nintendo | `slash-proc/snes-retro-go-sd` |
| `gba` | gpSP | Game Boy Advance | `slash-proc/gba-retro-go-sd` |
| `msx` | blueMSX | MSX | `slash-proc/blueMSX-retro-go-sd` |
| `lynx` | Handy | Atari Lynx | `slash-proc/lynx-retro-go-sd` |
| `a2600` | Stella 2014 | Atari 2600 | `slash-proc/stella2014-retro-go-sd` |
| `a7800` | ProSystem | Atari 7800 | `slash-proc/prosystem-retro-go-sd` |
| `wsv` | Potator | Watara Supervision | `slash-proc/potator-retro-go-sd` |
| `amstrad` | Caprice32 | Amstrad CPC | `slash-proc/caprice32-retro-go-sd` |
| `pkmini` | PokeMini | Pokémon Mini | `slash-proc/PokeMini-retro-go-sd` |
| `tama` | TamaLIB | Tamagotchi | `slash-proc/tama-retro-go-sd` |
| `gw` | LCD Game Emulator | Game & Watch | `slash-proc/LCD-Game-Emulator-retro-go-sd` |
| `doom` | Doom | Doom I & II | `slash-proc/doom-retro-go-sd` |

## Homebrew

| Project | Title | From | Repo |
|---|---|---|---|
| `zelda3` | The Legend of Zelda: A Link to the Past | snes | `slash-proc/zelda3-retro-go-sd` |
| `ccleste` | Celeste Classic | pico8 | `slash-proc/ccleste-retro-go-sd` |
| `music` | Music | — | `slash-proc/music-retro-go-sd` |
| `snake` | Snake | — | `slash-proc/snake-retro-go-sd` |
| `pong` | Pong | — | `slash-proc/pong-retro-go-sd` |

## Not listed

- `pico8` — the PICO-8 core has no standalone project repository yet.
- `smw` and `minesweeper` publish under the spec but are not part of this
  working set; add them here once their origins are settled.
