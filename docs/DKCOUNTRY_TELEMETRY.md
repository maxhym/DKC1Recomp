# DKCountry telemetry bridge

`runner/dkc1_telemetry.{c,h}` lets the separate DKCountry launcher build
session statistics and achievements. It follows protocol version 1, which the
launcher repository specifies in `docs/TELEMETRY_PROTOCOL.md`.

## Activation and safety

- The launcher sets `DKCOUNTRY_TELEMETRY` to a file path when it starts the
  game. Without that variable the bridge opens nothing and writes nothing.
- `Dkc1TelemetryHostFrame` runs at the end of `Dkc1RunOneFrame`, after the
  cartridge frame completed. It only reads WRAM; it never writes emulated
  state, so cartridge behaviour, widescreen decisions, timing and saves are
  unchanged whether or not telemetry is enabled.
- `Dkc1OnStateLoaded` emits `state_load` and resets the comparison baseline,
  so a loaded state is never counted as progress.

## Fields

All addresses come from `runner/dkc1_wram_gen.h` (RAM map plus curated
overlay):

| Address | Name | Use |
|---|---|---|
| `$003E` | `Global_EntranceIDLo` | level/entrance changes |
| `$0575` | `Player_CurrentLifeCountLo` | a decrease of one is a death |
| `$057B` | `Player_CurrentBananaCountLo` | banana increases (binary; the HUD keeps separate digit bytes) |
| `$057F` | `Player_CollectedKONGLettersLo` | KONG letter mask changes |
| `$088B` | `Global_Layer1XPosLo` (`CameraX`) | approximate distance and death position |

## Attract demos

No DKC1 demo flag is known yet. Gameplay events are therefore only counted
while the host has forwarded real controller input within the last 30
seconds (`Dkc1TelemetryHostInput`, called by `sdl_host.c` and `win32_host.c`
just before `RtlRunFrame`). An unattended demo produces ticks marked
`"demo":true` and no gameplay events. Startup scripts and the headless host do
not forward input, so they never count gameplay.

## Verification status

- `tests/test_dkc1_telemetry.py` compiles `tests/test_dkc1_telemetry.c` with
  the bridge and checks decoding, banana wrap, life loss, KONG letters,
  distance, demo silence, the input window and state loads with synthetic
  data. It also checks that the hosts and CMake are wired.
- Not yet verified on the private ROM: that `$057B` is binary as assumed,
  the KONG letter bit order (the launcher only counts set bits), and event
  totals over a real play session.
