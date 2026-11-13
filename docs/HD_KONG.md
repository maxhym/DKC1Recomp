# HD Donkey Kong (Mandro)

An optional, presentation-only layer that draws an original high-resolution
character in Donkey Kong's place at window resolution. It is the DKC1 side of
the HD Kong layer in DKC2Recomp and reads the same pack file, so the Mandro
pack built there works here unchanged.

It is off unless a pack is present: with no `mods/kong-hd.dkc2hd` beside the
executable (or with `DKC1_HD=0`) every hook returns at once, nothing is
captured and the stock path is byte-identical. Only the Windows host draws
the sprite; the other hosts never load a pack. The Dixie variant keeps its
own art.

## How it works

1. `Dkc1HdPrepareFrame` (after the Baby Kong hook, which releases the OBJ
   overlay surface) finds the actor with sprite ID 1, locates his contiguous
   OAM run as the Baby Kong mod does, and asks the PPU's overlay capture to
   take those pieces out of the composite (`kPpuOverlayFlag_RemoveFromGame`).
   Donkey is replaced whether he leads or follows.
2. Donkey's semantic animation ID (`$01`-`$68`) picks a pose slot through a
   table in `runner/dkc1_hd.c`: idle, walk, run, jump or fall (a negative Y
   velocity is upward), roll, carry, throw, ride on an animal buddy, ropes,
   swim, hurt, victory, crouch (ducking and crawlspaces, `$54`-`$5A`) and
   knockout (a lost life, `$10`). The names of the IDs come from a public
   disassembly. Animation ID zero falls back to the actor state, then
   the velocity. Resting poses are relieved by a fidget after 5 s; the fidget
   slots take turns. The ground slap and the chest beat at the start of a
   level use the first fidget. Donkey carries
   overhead, so the pack's front-carry slots (DKC2's Kongs) are not used.
3. After each `ppu_runLine`, `Dkc1HdObserveLine` records where the main
   screen's priority buffer holds a background layer above Donkey's OBJ
   priority. Other sprites never cover the pose: between sprites the OAM
   order decides, not the priority bits, and an animal buddy would otherwise
   hide its rider.
4. `Dkc1HdFinishFrame` reads the captured native sprite's first and last
   rows. Ground poses put their anchor row under the native sprite; rope,
   water, air and hurt poses put their own middle on the native sprite's
   middle. A frame where the game hides Donkey (blinking) hides the pose. The
   pose is cut against the recorded occluders, cross-faded from the previous
   pose for 9 eased ticks, styled, and handed to the presenter. Between a
   standing pose and the four-legged walk or run, the pack's `stand_to_fours`
   clip plays first, forwards or backwards.
5. `runner/windows_graphics.c` draws it as one alpha-blended quad after the
   flat or CRT passes.

WRAM, VRAM, OAM, collision and timing are not modified.

## Using it

Put a pack at `mods/kong-hd.dkc2hd` beside `DKC1Recomp.exe`. DKCountry's
Atelier builds it from the Mandro frames bundled with DKC2Recomp; by hand,
run DKC2Recomp's `scripts/build_hd_kong_pack.py mods/mandro-hd <output>`.
**F8** switches between the HD character and the original sprite.

| Variable | Effect |
| --- | --- |
| `DKC1_HD=0` | Do not load the pack. |
| `DKC1_HD_PACK=path` | Load another `.dkc2hd` file. |
| `DKC1_HD_OFFSET=dx,dy` | Move the character, in SNES pixels. |
| `DKC1_HD_SCALE=4` | HD pixels per SNES pixel; larger draws smaller. |
| `DKC1_HD_FLIP=1` | Invert the facing. |
| `DKC1_HD_STYLE=hd\|doux\|retro\|crt` | Presentation style. |
| `DKC1_HD_SIZE=100` | Size in percent of the pack's (50 to 200). |
| `DKC1_HD_TRACE=1` | Log each pose change with the native IDs. |

## Validation and limits

`tests/test_hd_kong.py` compiles `tests/test_dkc1_hd.c`: pack loading, the
pose table, feet and centre placement, widescreen offset, occlusion,
brightness, fidgets, the cross-fade and the disabled paths, on synthetic
frames. The Windows build is compiled by DKCountry's CI.

Not yet checked with a ROM: the placement in game, the pose chosen for each
animal-buddy animation, Donkey as the follower, and the Windows drawing path
itself. Palette effects (water tint, colour math) and the CRT curvature do
not reach the HD sprite; master brightness does. Near a screen edge the
captured native sprite is clipped, which can shift the pose by a few pixels.
