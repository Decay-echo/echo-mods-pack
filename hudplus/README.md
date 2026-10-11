# EchoHudPlus

Extra readouts at the bottom of your view, where hud_lines moves Echo VR's speed readout (and hides Echo's own speed text, mute icon and connection bars, which this draws instead). Everything is drawn in your HUD colour: Echo's blue or orange for your team (from the network game's player record), white when you're not on a team.

- **Wrist thruster gauges.** One ring either side of the speed, one per hand. Each ring fills with how much of that hand's push goes into one direction together with the other hand. At the best push the whole circle fills in and the icon inside turns dark. One hand alone fills half (it caps at 1 m/s), and hands aimed apart fill less. A faint ring means that hand isn't thrusting.
- **Screen glow.** While both hands push lined up, the edges of your view glow in your HUD colour, like when you shield: one steady glow, the same all the way round, staying out at the edges so you can still see. How bright follows how well the hands line up: it starts at a thruster score of 0.9 (about 50 degrees apart) and is full from 0.985 (about 20 degrees) (`screen_glow_align_from` / `_full`). Speed scales it: faint below 3.5 m/s (`screen_glow_strength` 0.3), building to `screen_glow_peak` (0.55) at 4.8 m/s and staying there above it (`screen_glow_from` / `_to`). It's smoothed (in fast, out a little slower), so hand jitter doesn't flicker it, and a stun flash takes its place while it plays. `screen_glow=0` turns it off.
- **Mute icon and connection bars,** inside the left and right rings: a microphone with sound waves while others can hear you (open mic, push-to-talk held, push-to-mute released), slashed when they can't (muted or talk button up) and four bars lit from your ping (a cross when there's no ping). They replace Echo's own icons, which hud_lines hides because Echo always draws the connection bars white.
- **Speeds.** Between the rings, in Echo's own speed font, without "m/s":
  - your current speed on top, slightly bigger, with your top speed of the last 7 seconds (`top_speed_seconds`) in a filled pill beside it, e.g. `3.6`. It holds your highest for 7 s, then drops to the next highest still inside the window;
  - under it, the top speed of your last run at or above 10 m/s, held until your next run at 10+ replaces it.

  `make_digits.py` extracts the digits from your game files (`data/echo_digits.bin`, not in git); without them it uses built-in digits.
- **Speed rings.** Five small numbered rings side by side above the speeds, lighting up at 1, 2, 3, 4 and 5 m/s (`rings_at`). Each fills from the previous ring's speed to its own. Their labels are whole numbers.
- **Filled look.** A full thruster ring or speed ring fills in with three darker shades of the HUD colour: a deep outer band, a medium band and a lighter centre. It also has a faint diamond weave and a soft glow outside the ring. Icons and numbers on it switch to a light tint. The top-speed pill uses the same dark shades.
- **Stun flash.** When you get stunned, a full-view flash and pulse for the length of the stun: **green** if your shield was up on your side (or dropped within 200 ms), **red** if it wasn't. Echo's own team-coloured stun effect still plays underneath.
- **Boost gauges** (off by default; `[boost] enabled=1`), one per back-booster charge: red while it comes back, green when ready.

```
<echo-arcade>\.venv\Scripts\python.exe make_digits.py <echo-arcade>\vendor\Doom-on-EchoVR\EchoVr-Tablet-Probing
build_hudplus.cmd                      # builds build\EchoHudPlus.dll and the tests
py -3 install_hudplus.py               # copy into plugins\, add the echoloader.json entry
py -3 install_hudplus.py --uninstall
```

Settings: `plugins\EchoHudPlus\settings.ini` (positions, sizes, threshold, grace time, flash strength; restart Echo VR after changes). Positions are readout pixels from the centre of the speed: 512 px = 1 m at 0.319 m, y down. `install_hudplus.py --reset-settings` rewrites them with the defaults and keeps the old file as `settings.ini.old`. Log: `%LOCALAPPDATA%\EchoHudPlus\hudplus.log`.

It replaces EchoCrosshair (the centre dot); the installer removes that if it's still there.

## How Echo's boost works

From the arena balance file `mp_arena_movement` (`CJsonResource` `302043c66f60d3ec`):

```json
"boost":   {"accel": 10.0, "power_cost": 49.9, "time": 0.25, "recharge_time": 0.01, "max_vel": 5.0},
"battery": {"max_power": 100.0, "recharge_rate": 33.3, "recharge_delay": 0.25}
```

Each boost takes 49.9 from a 100 battery, so two fit. Every boost restarts a 0.25 s delay, then the battery refills at 33.3/s: about 1.75 s for one charge, 3.25 s for both. It's one battery, not two timers, so after a double boost the charge that comes back first is the one you used last. The left gauge always shows that first-to-return charge.

## How Echo's wrist thrusters work

From the same file, `"iron_man": {"single_hand_max_vel": 1.0, "two_hands_max_vel": 2.0, "accel": 1.5}`. The update is at `0x1409278B0`:
- Each thrusting hand aims at a target velocity of (its direction) × max_vel. That's 1 m/s alone, or 2 m/s each when both hands thrust.
- The two targets add up, so the most in one direction is 4 m/s, with both hands lined up.
- The player accelerates at 1.5 m/s² per hand towards the combined target. Thrusters don't push you past it; they only steer.

A hand's gauge is its part of the combined push divided by its two-hand best:
- both lined up: 1.0 each;
- 90° apart: 0.71 each;
- one hand alone: 0.5.

## Team, mute and ping

These are read from the network game, reached from any component system: `+0x80` → scene, `+8` → game, `+0x8518` → network game. The steps match Echo's own `R15NetLocalPlayerPingExpression` and the session API:
- **Player record:** the local peer is in bits 0–4 of the u16 at `+0x178`. The peer → slot table at `+0x138` gives your player slot. The record is `slot × 0x250`, and its u16 at `+0x178` holds your **team** in bits 10–12 (0 blue, 1 orange, 2 spectators).
- **Ping:** `[+0x40] + 0x18` → peer table (`+0x360`, count `+0x368`, stride `0xD8`), u16 at `+0x8A`.
- **Voice chat muted:** bit 38 of the flags at `[+0x2DA0]`, the bit Echo sets when it fires `delegate_onvoipmuted`.
- **Voice mode:** `[+8]` (back to the game) `+0x8C14`: 0 off, 1 open mic, 2 push to talk, 3 push to mute.
- **Talk button:** from a fourth hook, the voice broadcaster update at RVA `0xD7BD90`, which fires `delegate_onvoippressed`/`released`. Your entry (0x78 bytes, peer handle at `+8`) has flag `0x400` at `+6` while the button is held.

The connection script (`cf2cf7fb12da1b51.dll`) only switches the bars' alpha; it never sets a colour. That's why Echo's bars stay white while the speed and mute icon follow your team. The thresholds here (`ping_bars_ms`) are mine, not Echo's.

## Where the data comes from

All offsets are for echovr.exe build `0x6452dff6` (sha256 `b6d08277…`) and live in `game_memory.h`. Each hook is installed only if the code at its address matches byte for byte, and every read is guarded, so a bad pointer gives "no data", not a crash.

| hook | RVA | what it gives |
| --- | --- | --- |
| local boost input `void(playerNavSystem*, playerNavEntry*, float dt)` | `0x74F0B0` | runs every frame for each `CR15PlayerNavCS` entry; only the local player prefabs (`dfb4e911bdaddc39`, `f1e70acd0dde0312`) have one, so that is you. PlayerNav entry (0x890 bytes) `+0x28/+0x30` → movement entry (0x280 bytes): `+0xD8` body (velocity `+0x70`), `+0x11C` boost `power_cost`, hand thrust directions `+0x1B4`/`+0x1C0`, thruster state `+0x128` (on flags `+0x2C/+0x30`, per-hand factors `+0x34..+0x40`, max_vel single/two `+0x4C/+0x50`), `+0x250/+0x258` battery handle → `CR15BatteryPackCS` entry (energy `+4`, pending `+8`, delay left `+0xC`; max/rate/delay in its resource). Also your actor reference. |
| punch/shield update `void(punchSystem*)` | `0xD679E0` | the punch entry (0x628 bytes) whose controller handle (`+0x10/+0x18`) is yours: flags `+0x608`, bit 0 = shield up. This bit fires `OnBlockPressed`, which drives the shield HUD. |
| punchable stun `void(system*, id, …, float seconds)` | `0xD534F0` | who a stun lands on (actor reference at `+0x100`), whether they were already stunned (`+4` bits 0–1), and the stun length. |

Handles are `u32 {index, generation}`. A component system's id map at `+0xC8` gives `{slot, generation}`, entries are at `+0xF8`, and 16-byte actor references at `+0x100`. These are the same checks as the game's own `IsValid`.

## Drawing

Everything is in one 512×256 texture (sRGB, premultiplied alpha) on a 3-buffer Oculus swap chain, made on the game's D3D12 queue. It's redrawn on the CPU and uploaded only when something changes. Each element is its own head-locked `ovrLayerQuad` with a viewport into that texture:
- the gauges and tracker sit at the readout's depth (0.319 m);
- the flash is a 3.4 m quad at 1 m.

The layer header (8 or 136 bytes, depending on the SDK) is detected from the game's own first layer. If the runtime rejects a frame with the overlay, it retries once with the other header, then resubmits without the overlay and turns it off for the session.

## Tests

- `build\hudplus_test.exe` covers:
  - charge maths, the thruster scores, the speed-tracker runs, the shield-grace rule and the flash curve;
  - the drawings and the layer bytes;
  - every game-memory read against fake memory in the game's layouts, including stale handles and unmapped pointers;
  - the ring uploader on the GPU, with read-back.
- `build\hudplus_hook_test.exe <build\fake> <mode>` runs a test build of the plugin against a stand-in `LibOVRRT64_1.dll` and fake game memory, through the plugin's own game-side code. It covers boost and thruster gauges, the tracker, green and red stuns, another player's stun, already stunned, the shield grace, the ready pulse and stale data. The modes are: runtime accepts (0), rejects (1), and only the 136-byte header works (2).
