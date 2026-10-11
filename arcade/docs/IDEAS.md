# Echo Arcade: ideas backlog

These are suggestions only; none of them is built yet. Anything that runs as a Windows window can be captured and driven by touch, so most of these fit the existing host.

## Top three to do next

1. **On-screen keyboard:** makes search possible in Plex, YouTube and a browser, plus renaming saves.
2. **YouTube / Twitch through mpv:** mpv plays these through its built-in YouTube support (yt-dlp), so little new code is needed.
3. **ScummVM:** already a RetroArch core, and point-and-click suits tapping.

## Games

- **Other LÖVE games you own:** *Mari0*, *Move or Die* and similar reuse the Balatro Lua touch bridge almost unchanged.
- **Mouse-only card and puzzle games:** *Slay the Spire*, *Inscryption*, *Luck be a Landlord*, *Dicey Dungeons*, *Mini Metro*, *Baba Is You*. These need a generic touch-to-mouse bridge for regular Windows games.
- **ScummVM:** point-and-click adventures (Monkey Island, Day of the Tentacle, …) from your own copies.
- **Chess / Go vs the computer:** a board drawn by the host, using the Stockfish engine. Good between rounds in a lobby.
- **Emulator extras:** PICO-8 (owned) and the free TIC-80; Cave Story via the NXEngine core (the freeware original); Quake and Wolfenstein 3D engine ports with your own game files.

## Media

- **Jellyfin / Emby:** the same browse-and-play as Plex, for self-hosted servers.
- **YouTube:** browse and play through mpv + yt-dlp; needs the on-screen keyboard for search.
- **Twitch:** watch streams on the wrist in the lobby, through mpv.
- **Music player / Spotify remote:** Windows media controls. The DOOM mod author's MUSIC tab proves the approach.
- **Photo slideshow:** a folder viewer with next/previous.

## Utilities

- **Desktop mirror:** put any window or monitor on the tablet (Discord, browser, guides). Same capture code, with touch mapped to clicks.
- **Web browser:** an embedded WebView2 browser for guides and wikis. Needs the keyboard.
- **Echo stats page:** your match stats from the community server's API. Read-only.
- **Timer / scoreboard:** for private matches and scrims, drawn by the host with no capture needed.

## Multiplayer (idea only)

Echo Arcade is local-only today. Everything here adds networking between players who all have the mod installed, so it needs opt-in, and the server owner's permission for anything that touches the server.

### How players would connect

- **Arcade relay:** a tiny message relay for board games, invites and sync. It could run as an optional service next to the community server, or as a plugin on the server (EchoRelay / NEVR plugins are the natural hook).
- **Finding each other:** use the Echo lobby itself. Everyone in the same private lobby session sees an "Arcade room", so there are no codes to type. The runtime would read the lobby/session ID from the game.
- **Fallback:** a 4-letter room code shown on the tablet, like the Plex link screen.

### Games with friends

- **Chess, checkers, Connect 4, Battleships, tic-tac-toe, Uno-style cards:** turn-based and tiny to sync (one message per move). The host draws the boards natively, so there's no capture and it works well on the touch grid. **Best first multiplayer feature.**
- **Party games:** trivia and quick-draw guessing (draw on your tablet with the touch grid), with everyone in the lobby playing.
- **Spectating:** watch a friend's board or game on your own tablet.

### RetroArch multiplayer

- **RetroArch Netplay** is built in: one player hosts, others join, and it supports co-op and versus on SNES, N64, Genesis, arcade and more. Everyone needs the same core and ROM, and the relay or lobby handles finding the host.
- The tablet pad already sends input as "player 1". Netplay would map each person's tablet pad to their own port.
- Extras: a netplay invite button in the pause menu, and "join friend" in the launcher.

### Big screen in the lobby (shared)

A large screen in the lobby space that everyone nearby sees, for co-op games, movies and tournaments.

- **Rendering:** draw a big textured quad in the world instead of on the tablet. It needs reverse-engineering Echo's world rendering (or reusing an existing lobby screen or billboard mesh and swapping its texture, the same way the tablet texture is streamed now). **This is the hardest part.**
- **Movies (watch party):** don't stream video between players. Each client plays its own copy or its own Plex stream, and the relay keeps play/pause/seek in sync by timestamp. Cheap and high quality.
- **Co-op games:** the host player runs the game, and the others either use RetroArch Netplay (each client renders locally, low bandwidth) or receive a compressed video stream (e.g. H.264 over the relay). Netplay scales better.
- **Controls:** anyone's tablet becomes a controller for the big screen. Player slots are assigned when you join.
- **Etiquette:** a host or room owner decides what's on screen; volume falls off with distance; there's an off switch per player so nobody is forced to watch.

### Rough order

1. Relay plus lobby rooms, with **Chess** and **Battleships**.
2. **RetroArch Netplay** invites from the tablet.
3. **Watch-party sync** for Movies/Plex (still on the tablet).
4. **Big lobby screen**: the texture swap exists (DOCK, local only); next is showing it to everyone nearby, then co-op with shared controls.

## Tablet upgrades

- **On-screen keyboard** (see top three).
- **Box art in lists:** RetroArch thumbnails and Plex posters instead of text-only rows.
- **Favourites / recently played** row on the launcher.
- **Save states from the pause menu:** quick save/load through RetroArch's command port.
- ~~**Controller passthrough**~~: done as **FREEZE** (Touch controllers as the RetroPad, player held still).
- ~~**Big screen mode**~~: done as **DOCK** (the arcade on the nearest lobby poster, touch + light gun). Next: aimed light-gun shots inside RetroArch games (needs RetroArch focused and the Windows cursor, see README).
