"""Installs or removes EchoHudPlus.dll: boost and wrist thruster gauges, a top-speed
tracker and a green/red stun flash, around Echo VR's helmet readout.

Needs EchoLoader (echoloader.json). Build first with build_hudplus.cmd.
  py -3 install_hudplus.py [--target GAME_FOLDER]
  py -3 install_hudplus.py --uninstall
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
ENTRY = 'EchoHudPlus.dll'
OLD_ENTRY = 'EchoCrosshair.dll'  # the centre dot this replaced
SETTINGS = """; EchoHudPlus: your speed and last-run speed with your mute icon and connection bars at
; the bottom of your view, the screen glow, the stun flash, and the pass counter at the top.
; Changes apply while Echo runs (within a second); most are also on the arcade tablet's
; SETTINGS tab. Positions are in readout pixels from the centre of
; Echo's speed (512 px = 1 m at the readout's distance): x out to each side, y down
; (negative = up).
[general]
enabled=1
; the HUD paints at most this many pictures a second, on a thread of its own (Echo's
; render thread spends microseconds on it either way)
max_fps=60

[layout]
; height of the whole cluster, metres from eye level (Echo's own readout, hidden by hud_lines, is at -0.13)
hud_distance_m=0.319
hud_height_m=-0.10
px_per_metre=512

[colors]
; HUD colour per team (Echo's own team colours); neutral = not on a team
blue=34,160,197
orange=255,132,1
neutral=235,235,235

[thrusters]
; your mute icon (left) and connection bars (right), either side of the speeds
enabled=1
; 1 = a gauge ring around each icon, one per wrist: it fills with how much of that hand's
; push goes into one direction together with the other hand (full = best speed)
rings=0
size_px=38
x_px=60
y_px=43
; 1 = if the left ring reacts to your right hand
swap_hands=0
; the mute icon and connection bars themselves
icons=1
; ping (ms) below which 4 / 3 / 2 bars light (otherwise 1)
ping_bars_ms=50,100,150
; while your hands push lined up, the edges of your view glow in your HUD colour, like
; shielding: steady, nothing up to screen_glow_from (m/s), then building from
; screen_glow_strength (0 = fades in from nothing) to screen_glow_peak at screen_glow_to
screen_glow=1
screen_glow_strength=0
screen_glow_peak=0.55
screen_glow_from=3.5
screen_glow_to=4.8
; how lined up (the thruster score: 0.9 is about 50 degrees apart, 0.985 about 20): the glow
; starts at screen_glow_align_from and is full from screen_glow_align_full
screen_glow_align_from=0.9
screen_glow_align_full=0.985

[speed]
; your current speed on top, the top speed of your last run at or above `threshold`
; under it (held until the next run), both in Echo's speed font; Echo's own speed
; text is hidden by hud_lines
enabled=1
threshold=10
; box between the rings: height (twice as wide), and the two digit sizes
size_px=34
current_font_px=20
font_px=13.5
; 1 = beside the current speed, your top speed over the last top_speed_seconds
; (off: the current speed sits in the middle on its own)
top_speed=0
top_speed_seconds=7
; how far below the readout's centre (negative = above it)
down_px=43
; five numbered rings above the speeds, side by side: each fills from the previous
; ring's speed to its own (m/s) and lights up when you reach it
ring=0
rings_at=1,2,3,4,5
ring_size_px=14
ring_gap_px=3
ring_y_px=13
; how long under the threshold before a run is over
run_gap_ms=300

[boost]
; back-booster charges: red while one comes back, green when ready (off by default)
enabled=0
size_px=18
x_px=52
y_px=40
swap_sides=0

[stun]
; a green/red flash when you're stunned (green: your shield was up)
enabled=1
; a shield dropped this long before the stun still counts as up (green)
shield_grace_ms=200
; flash opacity 0.05 .. 1
strength=0.85

[passes]
; at the top of your view: "+1" each time the disc goes to a teammate, the count itself from
; count_from passes on, gone when the other team gets it; a goal shows the chain 50% bigger,
; flashing. Reads Echo's own API: Echo's Settings > Game > API access must be on.
enabled=1
; both = either team's passes (each in its colour), mine = only your team's
teams=both
count_from=6
; a pass must reach the teammate within max_seconds of leaving the hand, with at most max_bounces
; bounces on the way (1 = a bounce pass counts); a slower catch, or more bounces, is no pass and
; the chain starts over at it
max_seconds=5
max_bounces=1
; box height (readout px) and how far above eye level (metres)
size_px=26
y_m=0.075
"""

spec = importlib.util.spec_from_file_location('patcher', HERE.parent / 'echo_game.py')  # find the game, is it running
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)


def game_bin(target):
    if target:
        return patcher.resolve_target(target).parent.parent
    found = patcher.discover()
    if len(found) != 1:
        raise RuntimeError('Pass --target with your Echo VR game folder.')
    return found[0].parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', help='Echo game folder, echovr.exe, scripts folder, or controller DLL')
    parser.add_argument('--uninstall', action='store_true')
    parser.add_argument('--reset-settings', action='store_true', help='rewrite settings.ini with the defaults')
    args = parser.parse_args()
    bin_ = game_bin(args.target)
    loader, plugins = bin_ / 'echoloader.json', bin_ / 'plugins'
    if not loader.is_file() or not plugins.is_dir():
        raise RuntimeError('EchoLoader not found (missing echoloader.json or plugins folder) in ' + str(bin_))
    if patcher.game_running():
        raise RuntimeError('Close Echo VR completely first.')
    config = json.loads(loader.read_text(encoding='utf-8'))
    backup = loader.with_name('echoloader.json.hudplus-backup')
    if not backup.exists():
        shutil.copy2(loader, backup)

    def entries(name):
        return [p for p in config.get('plugins', []) if isinstance(p, dict) and p.get('file', '').lower() == name.lower()]

    ours = entries(ENTRY)
    if args.uninstall:
        config['plugins'] = [p for p in config.get('plugins', []) if p not in ours]
        loader.write_text(json.dumps(config, indent=4) + '\n', encoding='utf-8')
        (plugins / ENTRY).unlink(missing_ok=True)
        print('EchoHudPlus removed. Settings kept in', plugins / 'EchoHudPlus')
        return
    built = HERE / 'build' / ENTRY
    if not built.is_file():
        raise RuntimeError('Build it first: run build_hudplus.cmd')
    shutil.copy2(built, plugins / ENTRY)
    settings = plugins / 'EchoHudPlus' / 'settings.ini'
    settings.parent.mkdir(exist_ok=True)
    if args.reset_settings and settings.exists():
        shutil.copy2(settings, settings.with_suffix('.ini.old'))
    if args.reset_settings or not settings.exists():
        settings.write_text(SETTINGS, encoding='utf-8')
    else:
        text = settings.read_text(encoding='utf-8', errors='replace')
        lines = text.splitlines()
        section, changed = '', False
        for i, line in enumerate(lines):  # installing means you want it: switched back on
            s = line.strip()
            if s.startswith('['):
                section = s.lower()
            elif section == '[general]' and s.replace(' ', '').lower() == 'enabled=0':
                lines[i], changed = 'enabled=1', True
        if changed:
            print('The HUD was switched off in settings.ini (or by the tablet\'s ALL OFF): switched back on')
        if '[passes]' not in text.lower():  # new in this version: its section, with the explanation
            lines += [''] + SETTINGS[SETTINGS.index('[passes]'):].rstrip('\n').splitlines()
            changed = True
            print('Added the pass counter settings ([passes]) to settings.ini')
        elif 'max_bounces' not in text.lower():  # this version's pass rules, under count_from
            at = next((i for i, l in enumerate(lines) if l.strip().lower().startswith('count_from=')), None)
            rules = SETTINGS[SETTINGS.index('; a pass must reach'):SETTINGS.index('max_bounces=1') + len('max_bounces=1')].splitlines()
            if at is not None:
                lines[at + 1:at + 1] = rules
                changed = True
                print('Added the pass rules (max_seconds, max_bounces) to settings.ini')
        if changed:
            settings.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    digits = HERE / 'data' / 'echo_digits.bin'
    if digits.is_file():
        shutil.copy2(digits, settings.parent / 'digits.bin')
        print("Echo's speed digits installed (last speed in the same font as the speed)")
    else:
        print('No data/echo_digits.bin: run make_digits.py for Echo\'s font (built-in digits until then)')
    old = entries(OLD_ENTRY)
    if old:
        config['plugins'] = [p for p in config.get('plugins', []) if p not in old]
        (plugins / OLD_ENTRY).unlink(missing_ok=True)
        print('Removed the old EchoCrosshair dot.')
    if not ours:
        config.setdefault('plugins', []).append({'file': ENTRY})
    loader.write_text(json.dumps(config, indent=4) + '\n', encoding='utf-8')
    print('EchoHudPlus installed:', plugins / ENTRY)
    print('Settings:', settings)
    print('Log after playing: %LOCALAPPDATA%\\EchoHudPlus\\hudplus.log')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print('ERROR: ' + str(error), file=sys.stderr)
        sys.exit(1)
