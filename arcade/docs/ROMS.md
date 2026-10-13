# Your games and BIOS files

Put your own ROMs and disc images in the folder for each system (subfolders are fine). Each time you open RetroArch from the tablet, Echo Arcade rebuilds RetroArch's **Playlists** from these folders, with each game already paired with its core. With the tablet pad, go **Playlists → system → game → Run**.

Up to three of your games also get tiles of their own on the launcher, in place of BALATRO, DUCK HUNT and DOOM: the ones named in `tiles.txt` in this folder (one file name, or part of one, per line: for example `melee` and `brawl`), else the newest game files here. A tile starts the game straight away with its core.

| Folder | System | Core | File types |
| --- | --- | --- | --- |
| `atari2600` | Atari 2600 | Stella | a26 bin zip |
| `atari7800` | Atari 7800 | ProSystem | a78 bin zip |
| `lynx` | Atari Lynx | Handy | lnx zip |
| `nes` | NES / Famicom | Nestopia | nes fds unf zip |
| `snes` | SNES / Super Famicom | Snes9x | sfc smc zip |
| `n64` | Nintendo 64 | Mupen64Plus-Next | z64 n64 v64 zip |
| `gb` | Game Boy / Color | Gambatte | gb gbc zip |
| `gba` | Game Boy Advance | mGBA | gba zip |
| `nds` | Nintendo DS | melonDS DS | nds zip |
| `3ds` | Nintendo 3DS | Citra | 3ds cci cxi 3dsx (decrypted) |
| `gamecube` | GameCube | Dolphin | iso rvz gcz ciso gcm |
| `wii` | Wii | Dolphin | iso rvz wbfs wad |
| `virtualboy` | Virtual Boy | Beetle VB | vb vboy |
| `mastersystem` | Master System | Genesis Plus GX | sms zip |
| `gamegear` | Game Gear | Genesis Plus GX | gg zip |
| `genesis` | Genesis / Mega Drive | Genesis Plus GX | md gen smd bin zip |
| `segacd` | Sega CD / Mega-CD | Genesis Plus GX | cue chd m3u |
| `32x` | Sega 32X | PicoDrive | 32x zip |
| `saturn` | Saturn | Beetle Saturn | cue chd ccd m3u |
| `dreamcast` | Dreamcast | Flycast | gdi cdi chd m3u |
| `psx` | PlayStation | PCSX ReARMed | cue chd pbp m3u |
| `ps2` | PlayStation 2 | LRPS2 (PCSX2) | iso chd cso |
| `psp` | PSP | PPSSPP | iso cso pbp chd |
| `pcengine` | PC Engine / TurboGrafx-16 | Beetle PCE Fast | pce zip |
| `pcenginecd` | PC Engine CD | Beetle PCE Fast | cue chd m3u |
| `ngp` | Neo Geo Pocket / Color | Beetle NeoPop | ngp ngc zip |
| `wonderswan` | WonderSwan / Color | Beetle Cygne | ws wsc zip |
| `msx` | MSX / MSX2 | blueMSX | rom mx1 mx2 dsk zip |
| `arcade` | Arcade + Neo Geo | FinalBurn Neo | zip (current FBNeo sets) |
| `mame` | Arcade (older sets) | MAME 2003-Plus | zip (MAME 2003-Plus sets) |
| `dos` | DOS | DOSBox Pure | zip dosz |
| `doom` | DOOM engine games | PrBoom | wad |

Multi-disc games work best with an `.m3u` list. When a folder has one, its discs aren't listed separately. `.bin` tracks are hidden when a `.cue` is present.

## BIOS files

Copy BIOS files you dumped from your own consoles into `apps\retroarch\system\`. File names must match exactly.

| System | Needed | File(s) |
| --- | --- | --- |
| PlayStation 2 | **required** | any PS2 BIOS in `system\pcsx2\bios\` |
| Saturn | **required** | `sega_101.bin` (JP) and/or `mpr-17933.bin` (US/EU) |
| Sega CD | **required** | `bios_CD_U.bin`, `bios_CD_E.bin`, `bios_CD_J.bin` |
| PC Engine CD | **required** | `syscard3.pce` |
| Atari Lynx | **required** | `lynxboot.img` |
| PlayStation | recommended | `scph5501.bin` (US) / `scph5500.bin` (JP) / `scph5502.bin` (EU) |
| Dreamcast | recommended | `dc\dc_boot.bin`, `dc\dc_flash.bin` |
| Nintendo DS | optional | `bios7.bin`, `bios9.bin`, `firmware.bin` |
| Game Boy Advance | optional | `gba_bios.bin` |

The engine support files (Dolphin, PPSSPP, LRPS2, blueMSX and the arcade data) are already installed by `setup_apps.py`.

## Performance

Echo VR is rendering in the headset at the same time as the emulator. 8-/16-bit systems, GBA, DS, PS1 and N64 are easy. GameCube, Wii, PS2, Saturn, Dreamcast, 3DS and PSP use a lot more GPU and CPU, so try them and lower the internal resolution in RetroArch's Quick Menu → Core Options if VR frame rate drops.

## Controls on the tablet

Or tap **FREEZE** and play with your Touch controllers (README: *FREEZE*). The side panels are a RetroPad: D-pad, A/B/X/Y, L/R, L2/R2, Start/Select. **STICK** switches the left pad to an analog stick (N64, PS1/PS2, GameCube and similar), and **MENU** opens RetroArch's menu. **HOME** gets you Resume / Quit.
