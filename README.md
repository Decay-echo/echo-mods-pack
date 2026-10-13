# Echo Mods Pack

A one-click install of two Echo VR mods for PC (community servers, the final Echo VR build):

- **Bottom HUD** (EchoHudPlus + HUD lines): a clean readout at the bottom of your view. Your current speed sits big in the middle, with your last run's top speed (10 m/s or more) under it, both in Echo's own font, and a mute icon and connection bars either side. A steady glow lights the edges of your view when your hands push lined up above 3.5 m/s, and a green/red flash shows when you're stunned (green = your shield was up). Echo's visor lines and its own speed/mute/connection icons are hidden so they don't overlap. At the top, a **pass counter**: "+1" for each pass inside a team (a catch after the disc bounced doesn't count), the count itself from the 6th pass, gone when the other team gets the disc, and 50% bigger and flashing on a goal. Gauges are off but still there if you want them: the wrist-thruster rings, speed rings 1–5 and a 7-second top speed.
- **Echo Arcade**: a gamepad tab on the hand tablet with **RetroArch** (30+ systems), your own games on the tiles, DOOM, **Minecraft (Java Edition)**, YouTube and Balatro (from your own Steam copy). It can show on the big lobby posters, and **FREEZE** holds you still while your Touch controllers play RetroArch or Minecraft; both hands above your head for 5 seconds lets you go again.

Everything runs on your own PC. Nothing here is networked and only you see your tablet.

## Install

1. Install **Python 3.10 or newer** from [python.org](https://www.python.org/downloads/). In its installer, tick **"Add python.exe to PATH"**.
2. Download **EchoModsPack.zip** and unzip it somewhere it can stay, for example `Documents\EchoModsPack`. The arcade runs its apps from this folder, so don't delete it afterwards.
3. **Close Echo VR**, then double-click **`INSTALL.cmd`**. It finds Echo VR by itself (or asks for the folder), downloads what the arcade needs (RetroArch and its cores: about 350 MB to download, 1.5 GB once unpacked), reads the HUD font from your own game files and installs everything. Takes a few minutes.
4. Start Echo VR.

Needs **EchoLoader** in the game. The pack carries a known-good copy (`loader\dbgcore.dll`) and INSTALL puts it in: when the game has none, and in place of a different build (the usual reason nothing of the pack shows up), which is kept as `dbgcore.dll.before-pack`. `INSTALL.cmd --keep-loader` leaves the game's own in. The HUD lines part also needs **NvrAssetPatches**; without it the installer skips that part and tells you.

**Already have another tablet mod** (TabletDoom, TabletMusic, anything that changes the tablet)? Just run INSTALL.cmd: it puts Echo's stock tablet data back first, from the backup that mod kept, removes that mod's files from the tablet data, and then installs Echo Arcade. If it can't find a stock copy it stops and tells you (run that mod's own restore, or reinstall Echo VR, then INSTALL again).

To remove everything, close Echo VR and double-click **`UNINSTALL.cmd`** (or, in the game, tablet **Settings > NORMAL > NORMAL TABLET**, which does it when you close Echo). Echo VR gets its own files back. Running `INSTALL.cmd` again updates or repairs the install. An update keeps your HUD settings; to switch to a new version's HUD layout, run `INSTALL.cmd --reset-hud` (your old settings are kept as `settings.ini.old`).

Only want one of them? `INSTALL.cmd --no-arcade` (HUD only) or `INSTALL.cmd --no-hud` (arcade only).

**Personal disc** (optional, a separate download): [EchoPersonalDisc.zip](https://github.com/Decay-echo/echo-personal-disc-addon/releases/latest/download/EchoPersonalDisc.zip). Put the zip in your EchoModsPack folder (or unzip it there, or next to the pack folder) and run **INSTALL.cmd**: the disc goes in with the rest, UNINSTALL takes it out, CHECK shows it. You get the personal disc everywhere but live play (lobby, timeouts, and from a goal until the launcher countdown ends), and **X on the left controller** brings it to your hand (the tablet's Settings > DISC changes the button). Nothing of it is installed when the zip isn't there.

**Something not showing up in the game?** Start Echo VR once, open the tablet, close Echo, then double-click **`CHECK.cmd`**: it writes `check.txt` (what is installed, whether EchoLoader loaded the plugins, the mods' own logs, antivirus detections). Paste that file when asking for help.

## Using it

**Performance.** The HUD paints on a thread of its own and costs Echo's render thread about 50 microseconds a frame. If Echo stutters with the pack installed, look at `%LOCALAPPDATA%\EchoHudPlus\hudplus.log`: once a minute it says how long the HUD took.

**Settings are on the tablet.** Open the hand tablet, press the arcade's **gear** tab: one page each for the tablet, the HUD, passes, the screen effects, Echo's voice chat mode (MUTED / OPEN MIC / PUSH TO TALK / PUSH TO MUTE as separate buttons, switched live) and NORMAL. Every switch applies right away (hiding Echo's visor lines: from the next start). **NORMAL > ALL OFF** puts everything back to Echo's normal at once and **ALL BACK ON** undoes it; **NORMAL TABLET** (tap twice) also takes the arcade, HUD and HUD lines out when you close Echo VR, and `INSTALL.cmd` brings them back.

**HUD.** It's at the bottom of your view, the pass counter at the top. The pass counter needs Echo's **Settings > Game > API access** on. Every setting (positions, sizes, colours, the glow) is also in `bin\win10\plugins\EchoHudPlus\settings.ini` in your Echo VR folder. Details: [hudplus/README.md](hudplus/README.md).

**Arcade.** Open the hand tablet and press the **gamepad** tab (far left). Tap a tile to play.

- **Your games:** put ROMs you own in `arcade\apps\roms\<system>\` (the folder names are listed in `arcade\apps\roms\README.md`). They show up under RetroArch's **Playlists**, and up to three of them get tiles of their own on the launcher (`arcade\apps\roms\tiles.txt` picks which; otherwise the newest).
- **MINECRAFT** (Java Edition): tap it, pick your launcher (Minecraft Launcher, CurseForge, Modrinth App or Prism Launcher), pick a profile and PLAY by tapping the tablet, and the game is on the tablet. Its menus take taps too (a tap is a real click at the middle of your fingertip; a slide scrolls the world and server lists), and the mouse pointer is drawn on the picture, so during FREEZE the right stick steers it through the menus and the right trigger clicks. FREEZE plays it with your Touch controllers in Bedrock's layout (left stick walks, right stick looks, A jumps, B sneaks, X drops, Y inventory, triggers attack/use, grips change the hotbar slot). Leaving FREEZE or a new level pauses it.
- **Echo in Echo** (off by default; `echo_in_echo=1` in `arcade\dist\EchoArcadercade.ini` adds an ECHO VR tile): a second Echo VR in its desktop mode on the tablet, taps as clicks, FREEZE with the Touch controllers like Minecraft.
- **CPU**: Echo runs above normal priority; Minecraft and its launchers run below it and only get normal priority while FREEZE plays them, and Minecraft's game is suspended outright (no CPU) whenever the tablet isn't showing it. It wakes the moment you look at it or play it.
- **Sound** goes to the headset (the Rift's headphones, or a Quest over Link) at 65% per app; `app_volume=` in `arcade\dist\EchoArcade\arcade.ini` changes that.
- **FREEZE** (top of the launcher, the HOME menu and the RetroArch screen): you're held still and your Touch controllers play RetroArch. Left stick = D-pad, right stick = analog, A/B/X/Y = face buttons, grips = L/R, triggers = L2/R2, left stick click = SELECT, right stick click = RetroArch menu, **menu button** = START. To move again, **hold both hands above your head for 5 seconds** or hold the menu button 1 second (the game pauses). It also switches off by itself when you change level. While frozen you can still grab the wrist tablet with your right hand and throw it away.
- **AUTO DOCK** (next to FREEZE) puts the arcade on the big lobby posters within 10 m of you, the big news board on the far wall included (never on the four tall boards along the lobby entrance). Undocking gives every poster its own picture back.
- **Videos and YouTube** fill the whole tablet; the small **≡** at the top-right brings the controls or YouTube's toolbar back, and again puts them away. While the tablet shows your YouTube window, it is kept on top of the desktop (Chrome blurs a covered window) and its sound goes to the headset (**Settings > TABLET > YouTube sound** switches it to Windows' default device); both go back when you close it on the tablet.

Details: [arcade/README.md](arcade/README.md).

## What's in the zip, and what isn't

The zip has the installers, the source of both mods, and their built plugin files (`hudplus\build\EchoHudPlus.dll`, `arcade\dist\`). **No Echo VR files are included** (the separate personal-disc add-on carries the two original script DLLs it may need to put back; see its README). The HUD font and the HUD-lines patch are made on your PC from your own install. RetroArch and its cores, LÖVE, mpv, yt-dlp, the DOOM shareware and the [Doom-on-EchoVR](https://github.com/heisthecat31/Doom-on-EchoVR) package tools download from their official sources while installing. No ROMs or commercial games are included: bring your own.

## Credits

- HUD, HUD lines and Echo Arcade by Decay-echo.
- [heisthecat31/Doom-on-EchoVR](https://github.com/heisthecat31/Doom-on-EchoVR): the tablet research and Echo package tools the arcade builds on (downloaded at install, pinned commit).
- [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-Clause, `LICENSES/MinHook.txt`), built into both plugins.
- [RetroArch / libretro](https://www.libretro.com), [LÖVE](https://love2d.org), [mpv](https://mpv.io), [yt-dlp](https://github.com/yt-dlp/yt-dlp): downloaded at install, under their own licenses.
