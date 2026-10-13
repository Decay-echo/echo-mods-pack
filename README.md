# Echo Mods Pack

A one-click install of two Echo VR mods for PC (community servers, the final Echo VR build):

- **Bottom HUD** (EchoHudPlus + HUD lines): a clean readout at the bottom of your view. Your current speed sits big in the middle, with your last run's top speed (10 m/s or more) under it, both in Echo's own font, and a mute icon and connection bars either side. A steady glow lights the edges of your view when your hands push lined up above 3.5 m/s, and a green/red flash shows when you're stunned (green = your shield was up). Echo's visor lines and its own speed/mute/connection icons are hidden so they don't overlap. At the top, a **pass counter**: "+1" for each pass inside a team, the count itself from the 6th pass, gone when the other team gets the disc, and 50% bigger and flashing on a goal. Gauges are off but still there if you want them: the wrist-thruster rings, speed rings 1–5 and a 7-second top speed.
- **Echo Arcade**: a gamepad tab on the hand tablet with **RetroArch** (30+ systems), DOOM, Duck Hunt, your movies, YouTube and Balatro (from your own Steam copy). It can show on the big lobby posters, and **FREEZE** holds you still while your Touch controllers play RetroArch.

Everything runs on your own PC. Nothing here is networked and only you see your tablet.

## Install

1. Install **Python 3.10 or newer** from [python.org](https://www.python.org/downloads/). In its installer, tick **"Add python.exe to PATH"**.
2. Download **EchoModsPack.zip** and unzip it somewhere it can stay, for example `Documents\EchoModsPack`. The arcade runs its apps from this folder, so don't delete it afterwards.
3. **Close Echo VR**, then double-click **`INSTALL.cmd`**. It finds Echo VR by itself (or asks for the folder), downloads what the arcade needs (RetroArch and its cores: about 350 MB to download, 1.5 GB once unpacked), reads the HUD font from your own game files and installs everything. Takes a few minutes.
4. Start Echo VR.

Needs **EchoLoader** in the game (the community Echo VR installer sets it up). The HUD lines part also needs **NvrAssetPatches**; without it the installer skips that part and tells you.

To remove everything, close Echo VR and double-click **`UNINSTALL.cmd`** (or, in the game, tablet **Settings > NORMAL > NORMAL TABLET**, which does it when you close Echo). Echo VR gets its own files back. Running `INSTALL.cmd` again updates or repairs the install. An update keeps your HUD settings; to switch to a new version's HUD layout, run `INSTALL.cmd --reset-hud` (your old settings are kept as `settings.ini.old`).

Only want one of them? `INSTALL.cmd --no-arcade` (HUD only) or `INSTALL.cmd --no-hud` (arcade only).

## Using it

**Performance.** The HUD paints on a thread of its own and costs Echo's render thread about 50 microseconds a frame. If Echo stutters with the pack installed, look at `%LOCALAPPDATA%\EchoHudPlus\hudplus.log`: once a minute it says how long the HUD took.

**Settings are on the tablet.** Open the hand tablet, press the arcade's **gear** tab: one page each for the tablet, the HUD, the screen effects and NORMAL. Every switch applies right away (hiding Echo's visor lines: from the next start). **NORMAL > ALL OFF** puts everything back to Echo's normal at once and **ALL BACK ON** undoes it; **NORMAL TABLET** (tap twice) also takes the arcade, HUD and HUD lines out when you close Echo VR, and `INSTALL.cmd` brings them back.

**HUD.** It's at the bottom of your view, the pass counter at the top. The pass counter needs Echo's **Settings > Game > API access** on. Every setting (positions, sizes, colours, the glow) is also in `bin\win10\plugins\EchoHudPlus\settings.ini` in your Echo VR folder. Details: [hudplus/README.md](hudplus/README.md).

**Arcade.** Open the hand tablet and press the **gamepad** tab (far left). Tap a tile to play.

- **Your games:** put ROMs you own in `arcade\apps\roms\<system>\` (the folder names are listed in `arcade\apps\roms\README.md`). They show up under RetroArch's **Playlists**.
- **FREEZE** (top of the launcher): you're held still and your Touch controllers play RetroArch. Left stick = D-pad, right stick = analog, A/B/X/Y = face buttons, grips = L/R, triggers = L2/R2, left stick click = SELECT, right stick click = RetroArch menu, **menu button** = START. **Hold the menu button 1 second** to move again (the game pauses). It also switches off by itself when you change level.
- **AUTO DOCK** (next to FREEZE) puts the arcade on the big lobby posters near you.
- **Videos and YouTube** fill the whole tablet; the small **≡** at the top-right brings the controls or YouTube's toolbar back, and again puts them away.

Details: [arcade/README.md](arcade/README.md).

## What's in the zip, and what isn't

The zip has the installers, the source of both mods, and their built plugin files (`hudplus\build\EchoHudPlus.dll`, `arcade\dist\`). **No Echo VR files are included.** The HUD font and the HUD-lines patch are made on your PC from your own install. RetroArch and its cores, LÖVE, mpv, yt-dlp, the DOOM shareware and the [Doom-on-EchoVR](https://github.com/heisthecat31/Doom-on-EchoVR) package tools download from their official sources while installing. No ROMs or commercial games are included: bring your own.

## Credits

- HUD, HUD lines and Echo Arcade by Decay-echo.
- [heisthecat31/Doom-on-EchoVR](https://github.com/heisthecat31/Doom-on-EchoVR): the tablet research and Echo package tools the arcade builds on (downloaded at install, pinned commit).
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause, `LICENSES/MinHook.txt`), built into both plugins.
- [RetroArch / libretro](https://www.libretro.com), [LÖVE](https://love2d.org), [mpv](https://mpv.io), [yt-dlp](https://github.com/yt-dlp/yt-dlp): downloaded at install, under their own licenses.
