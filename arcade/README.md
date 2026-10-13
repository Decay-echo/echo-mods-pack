# Echo Arcade

A new **Arcade** tab (gamepad icon) on Echo VR's hand tablet that runs **Balatro**, **Duck Hunt**, **RetroArch** (30+ systems, full menu, plus DOOM), your **movies** and **YouTube** on the tablet screen, all by touch. It's built for private community servers. Everything runs locally on your PC and nothing is networked: only you see your tablet.

```
┌──────────── Echo VR hand tablet ────────────┐
│  ECHO ARCADE                tap a game      │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ BALATRO  │ │DUCK HUNT │ │RETROARCH │     │
│  │ Steam    │ │ Zapper   │ │ 30+ sys. │     │
│  └──────────┘ └──────────┘ └──────────┘     │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ DOOM     │ │ MOVIES   │ │ YOUTUBE  │     │
│  └──────────┘ └──────────┘ └──────────┘     │
│ [pad] (home) (list) (emblem) (friends) [⚙] │
└─────────────────────────────────────────────┘
```

| Tile | What it runs | Controls |
| --- | --- | --- |
| Balatro: Steam original | Your own Steam copy, unmodified content, **your Steam saves and achievements** | Tap = click, slide = drag cards |
| Duck Hunt | *Duck Hunt (World).nes* from `apps\roms\nes` in Nestopia, NES Zapper on port 2 (its remap) | Dock the arcade and shoot the poster; tablet RetroPad for Start/Select |
| RetroArch | RetroArch 1.22 + cores for 30+ systems (Atari to PS2/GameCube/Dreamcast, arcade, DOS). Your ROMs appear under **Playlists**, already paired with a core | On-screen RetroPad: D-pad or **analog stick**, ABXY, L/R, L2/R2, Start/Select, MENU |
| DOOM | Shareware DOOM via the prboom core | Same RetroPad |
| Movies | Your video folders (`apps\movies` and Windows *Videos* by default) in mpv | Tap for controls: ±10/30 s, pause, seek bar, volume, subtitles, audio track |
| YouTube | YouTube inside the tablet, no browser: **HOME** (your own recommendations when `apps\youtube_cookies.txt` holds your exported YouTube cookies, else rows of videos for the topics in `apps\youtube_home.txt`), **SEARCH YOUTUBE** (on-screen keyboard), your saved links and mixes (`apps\youtube.txt`); every list row shows the video's picture, and a tap plays it in mpv on the tablet (yt-dlp finds the stream; it needs the Deno runtime in `apps\deno`, which the installer downloads). **YOUTUBE ON THIS PC** is the old way: the YouTube page open in your browser, shown on the tablet with taps and scrolling posted to its window (its sound goes to the headset while the tablet shows it: Settings > TABLET > YouTube sound). Whenever your browser has YouTube open, frozen tablet or not, skippable ads are skipped for you as soon as YouTube offers the Skip button (Settings > TABLET > Skip YouTube ads) | Lists: tap a row to play; mpv's player controls. Browser page: BACK, HOME, SEARCH, PREV, PLAY/PAUSE, NEXT, CLOSE |
| MEDIA | Whatever plays on this PC: Spotify, a browser's YouTube, mpv, anything that shows in Windows' media controls | Now playing (title, artist, app), PREV, PLAY/PAUSE, NEXT (Windows' media session), VOL −, MUTE, VOL + (the default output device's master volume) |
| SHARE MY TABLET | Your tablet streamed to friends: whatever is on it (a video, Minecraft, a list) as JPEG pictures up to 15 a second (only when the screen changes) plus the sound of the app on it (its own audio, captured apart from the rest of the PC; Windows 11). Any number of friends can watch at once. Tap: sharing starts and the tile shows your **code** (12 letters and digits: your internet address, the port and a secret). Tap again: the LAN code (same network / VPN), how many watch, STOP SHARING. The host asks the router to open the port (UPnP); without that, forward TCP `share_port` (6730) to the PC, or use the LAN code. Windows Firewall must let ArcadeHost.exe in (the installer adds the rule when it runs as administrator; otherwise Windows asks on the desktop the first time) | Viewers only look; nothing on your tablet is controlled from outside |
| WATCH A FRIEND | A friend's tablet on yours, with its sound in your headset. Enter the code with the on-screen keyboard (CONNECT); friends you watched are remembered (`%LOCALAPPDATA%\EchoArcade\friends.txt`, Name \| code) | Tap the picture for the bar: MY TABLET goes back to your own tablet and stays connected (the WATCH tile returns to the friend; no pictures are sent to you meanwhile), LEAVE disconnects |
| SPLIT SCREEN | A share where the friends who join with your code send their own tablets back (up to 8 pictures a second): their tablets, and yours while you are using it, are laid out together (2 side by side, 3 three ways, 4 in quadrants; up to four panes) and that layout is what everyone sees. A friend who is looking at the split screen shows the app running under it (a video keeps playing) or a note that they are looking; one who went back to their own tablet (MY TABLET) shows it | Tap for the bar: STOP ends it, MY TABLET lets you use your own tablet while it goes on (your tablet becomes a pane; the tile brings the split screen back). You hear your own app's sound; the friends' sound does not come up (yet) |

While the tablet shows your YouTube window, the host keeps that window on top of the desktop (without giving it focus): covered by Echo VR's window, Chrome stops rendering the page at full resolution and the tablet only gets a blurry version. It goes back to normal when you close YouTube on the tablet. If it still blurs, set `chrome://flags/#calculate-native-win-occlusion` to Disabled and restart Chrome. **Sound.** Each app's sound goes to the headset (the Rift's headphones, else a Quest over Link; `audio_device=` in `arcade.ini`) at `app_volume=65` percent of its own volume (Windows' per-app volume, so Echo's stays as it is).

**MINECRAFT (Java Edition).** Its menus take taps from the tablet (the touch grid is 64 x 36 cells of 16 px, and a tap lands at the middle of every cell the fingertip covers; the mouse pointer is drawn on the picture while Minecraft has the mouse, so the right stick steers it during FREEZE): a tap is a real click at that spot and a slide scrolls the world and server lists (the focus goes to Minecraft for the click and back to Echo right after; FREEZE keeps it there). The tile starts your launcher (the Minecraft Launcher; with CurseForge, Modrinth App or Prism Launcher installed too, it asks which). Pick a profile and PLAY by tapping the tablet, and the game's window takes the tablet. **FREEZE** then plays it with your Touch controllers in Bedrock's controller layout, as keyboard and mouse: left stick walks, right stick looks, A jumps, B sneaks, X drops, Y opens the inventory, right trigger attacks, left trigger uses/places, grips change the hotbar slot, left stick click sprints, right stick click sneaks too, the menu button pauses. The game window is in front while FREEZE is on; FREEZE off (or a new level) gives Echo VR the front again and Minecraft pauses itself. The mouse pointer is drawn on the picture only while Minecraft shows one (menus); in the world, where the game hides it to look around, nothing is drawn. Minecraft's `options.txt` is set when it starts: `fullscreen:false` (the tablet needs a window), `pauseOnLostFocus:true`, and `maxFps:60` with `enableVsync:false` (the tablet shows 60 pictures a second and never more, so Minecraft draws exactly that: the full rate the headset can show, and nothing above it taken from Echo). With **Settings > TABLET > Minecraft light settings** (on by default; `echo_tweaks.ini` `[minecraft] light=0`), render distance is capped at 8, simulation distance at 6, clouds and entity shadows off: only ever lowered, so a world that ran at 32 chunks does not take Echo's frames with it. `[minecraft] look_speed` in `arcade.ini` sets the turn speed; `[host] movies_tile=1` puts MOVIES back in its place.

**Another tablet mod already in the game** (TabletDoom, TabletMusic, anything that adds packages to the tablet data): INSTALL puts the stock tablet data back first, from the backup that mod kept (Doom-on-EchoVR's tools leave `manifests\<id>.orig`) or one of ours, removes that mod's packages, and then installs Echo Arcade. If no stock copy can be found it stops and says so (run that mod's own restore, or reinstall Echo VR).

**ECHO VR (Echo in Echo), off by default** (`arcade.ini` `[host] echo_in_echo=1` shows the tile). The tile starts a second Echo VR in its desktop mode (`-noovr`: no headset, a flat window with mouse and keyboard, its API on port 6722) and puts it on the tablet. Taps are real clicks, FREEZE plays it with the Touch controllers in the same layout as Minecraft (left stick WASD, right stick the mouse, triggers the mouse buttons). The pack's own plugins stay out of that second copy (they see `ECHO_INNER=1` and do nothing), so the tablet and the headset stay with the first. It uses the same login as your headset copy: whether the server lets the same player in twice is up to the server. QUIT closes it.

**CPU and GPU for Echo.** Echo VR itself runs at above-normal priority (`echo_tweaks.ini` `[cpu] echo_above_normal=0` leaves it alone). Minecraft, its launchers and Echo in Echo start at below-normal priority and only get normal priority while FREEZE plays them: Echo's threads still come first, so the headset keeps its frames and the tablet (your only way to control Minecraft) keeps working. The GPU is scheduled the same way: the game's drawing is put below normal GPU priority (Echo's frames go first; Echo itself gets above-normal GPU priority only when it runs as administrator, which Windows requires for that), and Minecraft's own frame rate is capped at the tablet's 60. Minecraft's game is suspended outright, no CPU at all, whenever the tablet does not show it and FREEZE is off; it runs again the moment you look at it or play it (a world you left pauses by itself, so nothing is lost). Launchers are never suspended, so a modpack keeps installing while you play Echo.

**Everything closes with Echo.** Whatever the tablet started (RetroArch, a game, a Minecraft launcher and the game it started, CurseForge's window inside Overwolf) is ended when you quit it on the tablet, and by a small watchdog when Echo VR closes or the arcade's host dies. YouTube is your own browser and is left alone.

**Out of FREEZE without a button:** hold both hands above your head for 5 seconds. (The menu button held 1 s and the FREEZE buttons still work.) **Throwing the tablet away while frozen** works as always: bring your right hand to your left wrist and grab; while your hands are together the right grip goes to Echo (the game doesn't see it).

**Taps.** A button fires the moment your finger lands on it (a poke in VR often lifts off somewhere else, or the game reports the release late or never, and waiting for it lost taps); one landing fires once however many touch cells the fingertip covers, and every button has half a touch cell of slack around it.

**Your games on the tiles.** MINECRAFT has the first tile; the launcher's BALATRO, DUCK HUNT and DOOM tiles make way for your own games: the ones named in `apps\roms\tiles.txt` (one file name, or part of one, per line), else the newest game files in your ROM folders, up to two. One game takes Balatro's tile, a second Duck Hunt's; DOOM stays as long as it fits. **FULL SCREEN** on the RetroArch screen puts the on-screen pad away so the game gets the whole tablet (FREEZE does the same by itself); the small **≡** at the top-right brings it back. Videos and YouTube get the whole tablet: the player controls and YouTube's toolbar stay away until you tap the small **≡** at the top-right (tap it again, or wait 8 s after the last control tap, and they go). A paused video shows **II** beside it. Tap **≡** (Balatro) or **HOME** (RetroArch) for Resume / Quit to launcher / Dock. The **gear** tab is **Settings**, one page per mod. Changes apply right away (the HUD and the disc button read their files again within a second); only the visor lines wait for Echo's next start.

- **TABLET**: tablet size (0.5x to 4x, live; experimental: it resizes the tablet's screens and touch areas, not its 3D body), AUTO DOCK, **Poster quality** (HIGH: the arcade is drawn at twice the detail on the posters: the host renders its screen a second time at 2048 x 1148 and streams it into a 2x poster texture of its own, and apps started from then on get a window twice as big; costs CPU every frame while docked, so off by default; `echo_tweaks.ini` `[poster] quality=high`), **Minecraft light settings** (render distance 8, simulation 6, no clouds or entity shadows when Minecraft starts; only lowered, never raised), **Skip YouTube ads** (your browser's YouTube, whether the tablet shows it or not), how FREEZE maps A/B/X/Y (by place like an Xbox pad, or by name), and **YouTube sound**: where your browser's sound goes while the tablet shows its YouTube (the headset, like the arcade's own apps, or Windows' default device; `echo_tweaks.ini` `[youtube] audio=headset|default`, else `arcade.ini` `youtube_audio=`). Windows keeps that choice per app, so it is put back when the tablet lets go of the window.
- **HUD** (when EchoHudPlus is installed): the HUD itself, the speeds, the speed size, the mute/network icons, the pass counter (off / my team / both teams) and the old gauges.
- **PASSES**: the pass counter (off / my team / both), how many bounces a pass may have (a bounce pass counts by default), the time limit for a pass (5 s), the pass the count starts showing from, and its size.
- **VOICE** (the first page): Echo's own voice chat mode as four buttons, MUTED / OPEN MIC / PUSH TO TALK / PUSH TO MUTE. The plugin switches the game's setting in memory while it runs (it hooks the game's settings reader at start to learn where that setting lives), and the host writes it into `settings_mp_v2.json` so it holds after a restart. The lit button is the mode the game reports.
- **SCREEN**: the thruster glow (on/off, brightness, the speed it starts above), the stun flash, and hiding Echo's visor lines (HUD lines, from the next start).
- **DISC** (when DiscButton is installed): the summon button (A, B, X, Y or OFF) and whether Echo still sees it.
- **NORMAL**: **ALL OFF** switches every one of those back to Echo's normal at once (tablet size 1x, no auto dock, no HUD, glow, visor-line patch or disc button) and remembers what was on: the same button then says **ALL BACK ON**. **NORMAL TABLET** (tap twice) does ALL OFF and takes Echo Arcade off the tablet when you close Echo VR (with the Echo Mods Pack: the HUD and HUD lines too). `tools/remove_on_exit.py` waits for Echo to close, then runs the uninstall and says so in a message box; tap **CANCEL** before then to keep it. INSTALL brings it all back as it was.

Saved in `echo_tweaks.ini` (tablet), `plugins\EchoHudPlus\settings.ini`, `plugins\DiscButton\settings.ini` and `asset_patches\manifest.json`. The view (FOV) is not on the tablet: set it in `echo_tweaks.ini` (below).

### FREEZE: play RetroArch with your Touch controllers

**FREEZE** (launcher header, next to AUTO DOCK; also in the HOME menu and on RetroArch's left panel) holds you still in Echo VR and hands your Touch controllers to RetroArch. It opens RetroArch if it isn't running, or picks up the one that is: pick a game from **Playlists** with the sticks and buttons, and play. Pair it with AUTO DOCK to play on the big lobby poster.

| Touch | RetroPad |
| --- | --- |
| Left stick | D-pad, or the left analog stick when **STICK** is on |
| Right stick | Right analog stick |
| A / B / X / Y | B / A / Y / X: by position, like an Xbox pad. `freeze_buttons=labels` in `arcade.ini` makes A = A, B = B and so on |
| Grips / triggers | L, R / L2, R2 |
| Left stick click | SELECT |
| Right stick click | RetroArch's menu (open / close) |
| ☰ (left menu button) | START on a tap. **Hold it 1 s to unfreeze** |

- **Every button at once.** The controllers are read every frame and everything you hold goes through together (run + jump + shoot). The on-screen pad only ever got one fingertip.
- **Getting out:** hold ☰ for a second, or tap **FREEZE** again. Echo gets your controllers back as if nothing happened, and a game in progress is **paused**. FREEZE again resumes it.
- **It ends by itself** when the level changes (lobby to match and back), when RetroArch closes, or if the arcade host stops. The game is paused in the first two cases.
- **How:** the plugin hooks `ovr_GetInputState`. While FREEZE is on, Echo VR sees neutral controllers (no buttons, triggers, grips or sticks), and the real ones go to the host. Hand tracking and finger poses are untouched, so you can still poke the tablet. The local player's velocity is set to zero after each frame's movement update, so you don't drift. The host plays the controllers on RetroArch's Network RetroPad (right stick included) and uses `GET_STATUS` / `PAUSE_TOGGLE` on its command port to pause and resume.
- **DiscButton** (the Personal Disc mod's summon button, X) steps aside while FREEZE is on: X plays RetroArch and doesn't summon the disc. They coordinate through the event `Local\EchoArcade.ControllerMode`.
- `patch_retroarch.py` also makes RetroArch read **every** RetroPad packet each frame (stock 1.22 reads one), so buttons pressed together land on the same frame and sticks don't lag. `install.py` applies it.

### Dock on a lobby poster

**AUTO DOCK** (top right of the launcher, and in the ≡ / HOME menu; on unless you turn it off, saved in `echo_tweaks.ini` `[dock] auto=`) puts the arcade on the big lobby poster nearest you by itself, and on every other poster of the same kind in that lobby (the game swaps the picture on the model they share). Walk up to another kind of poster and it goes on that one too: the earlier ones keep it. Posters docked this session get it back by themselves when you return to their lobby after a match. All of them take touches and shots. The tablet keeps showing it too, and turning **AUTO DOCK** off gives every poster its own picture back (a server poster comes back as well). AUTO DOCK only reaches 10 m: a poster farther away is left alone (the tablet says so), and the four tall boards along the entrance (the banners you meet coming out of the staging area) are never docked. The game's own art changes on a docked poster (the rotating displays swap their art on a timer) no longer take the arcade off: the two engine calls that apply art are skipped for docked posters. When a poster is undocked, every other poster of its model gets its own picture back too, each with its own art (the game had swapped the texture on the model they share). Dock works on the standing posters, the curved ring over the hub, the big combat-lobby ones, and the social lobby's big news board between the four small posters of the far wall: that board is a plain instance of the big poster model, not a dynamic poster, so the installer's manifest patch registers it as one in both lobby levels (build_arcade_tab.news_board_posters); while undocked it then shows the poster system's art instead of its own picture. The turned-picture path for the entrance boards stays in the code, unused while they are left alone. That display stores its picture turned (a portrait screen fed from a landscape texture), so it gets a texture of its own with the frame painted turned to stand upright on it, fitted to its width; touches and shots are mapped back the same way. The combat-lobby set piece is left out: its picture face is not mapped, the arcade drew blank on it and a shot at it crashed the GPU.

- **It's a touch screen.** Poke the poster with a fingertip: tap, hold and slide work as on the tablet (cards drag in Balatro, the on-screen RetroPad works).
- **Light gun.** Where your own bullets meet the docked poster, it's a tap at that exact spot: shoot the tiles, the menus, Balatro's cards, the player controls or the RetroPad's buttons. Only your shots count, not other players'. On a RetroArch game's picture a shot is a real **light-gun** shot, aimed where it hit (see below).
- Only you see it: the picture is streamed into your game locally, like the tablet.

### Light-gun games (RetroArch)

Duck Hunt style games work with the in-game guns: dock the arcade, start the game, and shoot the docked poster. Each bullet that lands on the game's picture aims the game's light gun at that spot and pulls the trigger (shots on the side panels still press the RetroPad buttons). Tested with *Metal Combat* (SNES Super Scope, Snes9x): aim calibration, crosshair and shots all land where the bullets hit.

- It goes through RetroArch's standard light gun, on any port, so it should work with every core that uses it: NES Zapper (Nestopia), SNES Super Scope / Justifier (Snes9x), Menacer / Light Phaser (Genesis Plus GX), GunCon / Justifier (PCSX ReARMed) and others. Only Snes9x has been tried so far.
- The game needs a light gun plugged in. If a game ignores the shots, open RetroArch's **MENU** > *Controls* > *Port 1 (or 2) Controls* > *Device Type*, pick the gun (Zapper, Super Scope, GunCon...) and *Save Game Remap File*. The Super Scope, for example, is Port 2 in Snes9x.
- Leave a core's own gun option on *Lightgun* (the usual default), not *Touchscreen*/*Pointer*/*Mouse*.
- **The gun's own buttons** (GunCon A/B, Justifier special, Super Scope pause...): once you have shot at the game, the tablet's **A**, **B**, **X**, **START** and **SELECT** also press the gun's A, B, C, Start and Select. The gun replaces the RetroPad on its port, so the RetroPad alone can't reach them. They are invisible overlay buttons in the bottom-left corner of RetroArch's window, pressed with a 0.3 s touch (Time Crisis misses shorter presses).
- Tested: *Metal Combat* (SNES Super Scope), *Duck Hunt* (NES Zapper, Nestopia: the Zapper reads light off the screen, and aimed shots hit ducks), *Time Crisis* PAL (PlayStation GunCon, PCSX ReARMed: language screen with tablet A, G-Con calibration, menus and stage 1 by shooting).

**How:** RetroArch aims its light gun at a touch on its window, focused or not, and an invisible overlay (`lightgun_overlay.cfg`, RetroArch's overlay light gun) makes the touch pull the trigger, for every core and port; the same overlay holds the gun-button hitboxes. So the host injects a 0.1 s touch at the spot. The window is marked *no-activate*, so the touch never takes focus from Echo VR. A touch goes to whichever window is on top at that point, so while you shoot, RetroArch's window is brought on screen and kept on top (without focus); 3 s after your last shot it goes back behind your windows (or off screen).

`light_gun=` in `arcade.ini`:

| Value | What a shot does |
| --- | --- |
| `touch` (default) | The touch above. Echo VR keeps focus. |
| `focus` | Fallback: gives RetroArch focus, puts the Windows cursor on the spot and clicks. Focus and the cursor go back 2 s after your last shot (or when the game closes). |
| `off` | Nothing on the game's picture (no overlay either). |

**Sound** from the apps goes to the VR headset (Link / Air Link "Oculus Virtual Audio Device"), not the Windows default: each app gets its own output device, the same per-app setting as Windows' volume mixer. Change it with `audio_device=` in `arcade.ini` (`default` = Windows default).

The six tab icons are centred on the bar; the Arcade and Settings icons are drawn at build time from your game's own tab art, so they match the stock ones. See **[docs/ROMS.md](docs/ROMS.md)** for ROM folders and BIOS files.

## How it works

```
echovr.exe
 ├─ EchoLoader (dbgcore.dll) loads plugins\EchoArcade.dll
 │    • hooks the tablet UI: new tab, page switching, touch cells (MinHook)
 │    • D3D12: finds the tab's screen texture (and the poster screen) when the
 │      engine creates it and copies each new frame into it on the game's own queue
 │    • DOCK: gives the nearest lobby poster that texture through the game's own
 │      poster system; fingertips and bullet rays hit-test its curved face
 └─ starts plugins\EchoArcade\ArcadeHost.exe (dies with Echo via a job object)
        • draws the launcher and touch controls
        • captures the game window (Windows.Graphics.Capture) → shared memory
        • touches → Balatro (Lua bridge over UDP 127.0.0.1)
                  → RetroArch (network RetroPad + command port, 127.0.0.1;
                               light gun: injected touch on its window)
                  → mpv (JSON IPC pipe), Plex (plex.tv link + your server)
```

- **Tablet data:** `tools/build_arcade_tab.py` reads your game's own manifest and adds the root, navigation and page canvases, the button table (the tab plus a 32×18 grid of invisible poke cells), the 1024×574 BGRA screen texture, the tab icons, and the 1249×612 poster screen texture (held by a hidden sprite so it stays loaded). Stock resources are verified byte-identical.
- **Posters:** `tools/build_poster_table.py` (run by the tab build, and by `build.cmd` if missing) reads every lobby level's dynamic posters: their placement and the picture face of each poster model, into `native/generated/posters.h`.
- **Balatro:** `tools/prepare_balatro.py` extracts the game from your `Balatro.exe` (a fused LÖVE executable) into `apps/balatro/`. It never writes to the Steam folder. The game then runs on LÖVE 11.5 with a small touch bridge (`bridge/arcade_bridge.lua`) appended to `main.lua`.
- **Supported build:** `echovr.exe` with timestamp `1683152886`. The runtime checks the executable and every hooked function's bytes at startup, and disables itself (logging the reason) on any mismatch.

## Install

Requirements:

- Windows 10/11 x64.
- An Echo VR install with **EchoLoader** (the `dbgcore.dll` + `echoloader.json` setup used by community servers).
- Visual Studio 2022 with *Desktop development with C++*.
- Python 3.10+.
- Balatro on Steam, if you want the Balatro tile. *Duck Hunt (World).nes* in `apps\roms\nes`, for the Duck Hunt tile.

```powershell
python -m venv .venv
.venv\Scripts\pip install zstandard==0.25.0 pillow texture2ddecoder
.venv\Scripts\python tools\setup_apps.py        # LÖVE, mpv + yt-dlp, RetroArch + 30 cores, DOOM shareware
.venv\Scripts\python tools\prepare_balatro.py   # finds Balatro.exe in your Steam libraries
.\build.cmd                                      # EchoArcade.dll, ArcadeHost.exe, runs the IPC test
# close Echo VR, then:
.venv\Scripts\python tools\install.py install --game "C:\path\to\ready-at-dawn-echo-arena"
```

Start Echo VR, open the hand tablet and press the **gamepad** tab (far left). The first open starts the host; after that the launcher appears.

To uninstall, close Echo VR and run `tools\install.py restore`. It puts back the original manifest (hash-checked) and `echoloader.json`, and removes the package and plugin files. Backups are kept in `backups\`.

After changing code, run `build.cmd` and then `tools\install.py update`. That refreshes the plugin, host and config without touching the tablet data. When the tablet data itself changes (as it did for DOCK), `update` says so: close Echo VR and run `tools\install.py reinstall` (restore, then install). Re-run `prepare_balatro.py` after Steam updates Balatro.

Put your ROMs in `apps\roms\<system>\` (see [docs/ROMS.md](docs/ROMS.md)); the playlists rebuild every time RetroArch opens. Video folders are set by `movies=` in `plugins\EchoArcade\arcade.ini`.

**Adding a ROM set:** `tools\import_roms.py "<set>.zip" <system>` unpacks a set (including zips nested inside it) into `apps\roms\<system>\`.

**FOV tweak:** `plugins\EchoArcade\echo_tweaks.ini` sets `[fov] x=` / `y=` multipliers (1.0 to 2.0; below 1.0 would cut off the edges of the view), edited only in that file, the same idea as EchoVR-Haptics' FovMultiplier, so you don't need that tool (it replaces EchoLoader; see Troubleshooting). Keep x and y equal (e.g. 1.4) for recordings; stretching one axis looks warped. The install seeds it from an old `haptics_config.txt` if present and never overwrites your edits.

**Plex privacy:** linking stores a Plex token in `%LOCALAPPDATA%\EchoArcade\plex.json`, never in the repo or the logs. *Unlink Plex* at the bottom of the Plex library list removes it. Streams go straight from your server to this PC.

## Test without a headset

`tools/fake_game.py` stands in for Echo VR: it keeps the heartbeat alive, pokes cells, answers DOCK like a lobby would, sends poster touches and shots, and saves the tablet frame as a PNG.

```powershell
dist\EchoArcade\ArcadeHost.exe --standalone
.venv\Scripts\python tools\fake_game.py tap 757 423      # DOOM tile
.venv\Scripts\python tools\fake_game.py tap 876 38       # DOCK TO POSTER
.venv\Scripts\python tools\fake_game.py poster 170 420   # a fingertip on the docked poster
.venv\Scripts\python tools\fake_game.py shot 990 300     # a bullet on the docked poster
.venv\Scripts\python tools\fake_game.py tap 602 38        # FREEZE
.venv\Scripts\python tools\fake_game.py pad A+RT 1 0 0 0 0.5   # Touch controllers: A + right trigger, stick right
.venv\Scripts\python tools\fake_game.py pad MENU 0 0 0 0 1.3   # hold the menu button: unfreeze
.venv\Scripts\python tools\fake_game.py level           # a new level loads (FREEZE ends)
.venv\Scripts\python tools\fake_game.py snap frame.png
```

`build.cmd` also runs `freeze_test` (the controller mapping: face buttons, D-pad / stick, the menu button's tap and hold, many buttons at once) and checks the controller hand-off in `ipc_test`.

`build.cmd` also runs `poster_math_test`: for every poster, touches and bullet rays at known points of its face must land on the right screen pixel.

## Troubleshooting

- The logs are `%LOCALAPPDATA%\EchoArcade\runtime.log` (in-game plugin) and `host.log` (launcher and apps).
- **Arcade tab shows but does nothing, and there's no `runtime.log`:** the plugin never started. Run `tools\install.py status`. If `echoloader dll: False`, another mod (e.g. the EchoVR-Haptics/FOV tool) replaced `bin\win10\dbgcore.dll`. Put EchoLoader back from `https://files.echovr.de/updates/dbgcore.dll` (the Echo VR Installer's source).
- **Arcade tab missing:** check `runtime.log` for `unsupported echovr.exe` or `signature mismatch`. Another tablet mod (e.g. the DOOM/MUSIC tab) uses the same slot; restore that one first.
- **Tab opens but shows the "STARTING…" splash forever:** look for `stream:` lines in `runtime.log`. `screen texture created` and `frames uploaded` should both appear.
- **Balatro only reacts to some taps:** on a profile that hasn't finished Balatro's tutorial, Jimbo's tutorial blocks everything except its own prompts. Tap **Skip >** (top right) or his speech bubble.
- **DOCK says "No lobby poster here"**: dock works in the social and combat lobbies, not in matches. **"Could not find your hands"**: touch the tablet once (that starts the fingertip tracking) and press DOCK again.
- **Docked, but the poster keeps its picture or touches/shots miss:** `runtime.log` has `posters:` and `light gun:` lines (where you were, which poster, where each shot went). Send those.
- **Light-gun shots don't register:** `host.log` has `light gun:` lines (where each shot went on the desktop, and whether another window was on top of RetroArch there). Check the game has a gun plugged in (above), or try `light_gun=focus`.
- **App windows** sit behind your other windows at the top-left of the desktop. Set `window_mode=offscreen` in `plugins\EchoArcade\arcade.ini` to move them off-screen.

## Status

On the desktop, every tile has been driven end to end through the real host, capture and input paths using `fake_game.py`: launching, touch input, pause menu and clean quit for all four tiles, and DOCK, poster touches and shots through the host. The RetroArch light gun (both modes, desktop and off-screen windows) was driven the same way with *Metal Combat* while another window kept focus. The in-game DOCK (poster texture override, fingertip and bullet hit-testing) is built from the game's code and data but hasn't been tried in a lobby yet. Report `runtime.log` with any issue.

## Credits

- **[heisthecat31/Doom-on-EchoVR](https://github.com/heisthecat31/Doom-on-EchoVR):** the reverse-engineered tablet canvas format, button records, hook addresses and package tools this builds on. `setup_apps.py` fetches it at a pinned commit; it is not redistributed here.
- **[MinHook](https://github.com/TsudaKageyu/minhook)** (BSD-2), included in `native/vendor/minhook`.
- **[LÖVE](https://love2d.org), [RetroArch/libretro](https://www.libretro.com), [prboom](https://github.com/libretro/libretro-prboom).**
- Balatro © LocalThunk / Playstack. You need your own copy; no game files are included.
