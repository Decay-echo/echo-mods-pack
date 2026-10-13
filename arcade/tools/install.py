"""Install / restore Echo Arcade in an Echo VR folder. Close Echo VR first.

    python tools/install.py setup       # the desktop INSTALL button: patches RetroArch, removes any
                                        # earlier Echo Arcade install (from any copy of this folder),
                                        # backs up, installs
    python tools/install.py uninstall   # the desktop UNINSTALL button: every original file back and
                                        # our packages and plugin files gone, whichever copy put them there
    python tools/install.py status
    python tools/install.py install     # builds the tab from your manifest, backs up, installs
    python tools/install.py update      # after build.cmd: refresh plugin, host and arcade.ini only
    python tools/install.py reinstall   # restore + install: needed when the tablet data changes
    python tools/install.py restore     # puts every original file back (this copy's own install)
    python tools/install.py configure   # (dev) write dist/EchoArcade/arcade.ini only

Options: --game PATH (default: this copy's install_state.json, then a sibling copy's, then the
Meta / Oculus install folder), --force (restore: ignore a manifest changed since install),
--dry-run (setup / uninstall: only say what would be done).

What install changes (all backed up under EchoArcade/backups/<time>/):
  _data/.../manifests/48037dc70b0ecab2      patched manifest (tablet resources + the DOCK poster screen)
  _data/.../packages/48037dc70b0ecab2_N     new package with those resources
  bin/win10/echoloader.json                 adds {"file": "EchoArcade.dll"}
  bin/win10/plugins/EchoArcade.dll          runtime plugin
  bin/win10/plugins/EchoArcade/             ArcadeHost.exe + arcade.ini
It also patches apps/retroarch/retroarch.exe (tools/patch_retroarch.py) so a button held on the
tablet stays held in RetroArch; the original is kept as retroarch.exe.orig.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GAME = ROOT.parent / 'ready-at-dawn-echo-arena'
KNOWN_GAMES = [Path(r'C:\Program Files\Meta Horizon\Software\Software\ready-at-dawn-echo-arena'),
               Path(r'C:\Program Files\Oculus\Software\Software\ready-at-dawn-echo-arena')]
STATE = ROOT / 'install_state.json'
ORIGINAL = ROOT / 'backups/original'  # a copy of the stock manifest, kept once seen
MID = '48037dc70b0ecab2'
EXE_TIMESTAMP, EXE_SIZE = 1683152886, 35852288
TAB_SCHEMA = 2  # bump when build_arcade_tab.py output changes (2: DOCK poster screen)
RETROARCH = ROOT / 'apps/retroarch/retroarch.exe'


def sha(path: Path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def sha_bytes(data: bytes):
    return hashlib.sha256(data).hexdigest()


def paths(game: Path):
    data = game / '_data/5932408047/rad15/win10'
    bin_ = game / 'bin/win10'
    return dict(manifest=data / 'manifests' / MID, packages=data / 'packages', bin=bin_,
                loader=bin_ / 'echoloader.json', plugins=bin_ / 'plugins')


def running(image: str):
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {image}'], capture_output=True, text=True).stdout
    return image.lower() in out.lower()


def check_game(game: Path):
    exe = game / 'bin/win10/echovr.exe'
    if not exe.exists():
        raise SystemExit(f'echovr.exe not found under {game}')
    d = exe.read_bytes()[:4096]
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    ts, size = struct.unpack_from('<I', d, pe + 8)[0], struct.unpack_from('<I', d, pe + 24 + 56)[0]
    if (ts, size) != (EXE_TIMESTAMP, EXE_SIZE):
        raise SystemExit(f'Unsupported echovr.exe build (timestamp {ts}); the runtime addresses are for {EXE_TIMESTAMP}.')
    if running('echovr.exe'):
        raise SystemExit('Echo VR is running. Close it first.')
    if running('ArcadeHost.exe'):
        raise SystemExit("Echo Arcade's host (ArcadeHost.exe) is still running. It closes a few seconds after Echo VR; try again.")


# ---- finding things: the game, the other copies of this folder, the stock manifest ----

def copies():
    """This folder and its sibling copies (e.g. Downloads\\echo-arcade-main*\\echo-arcade-main)."""
    seen, out = set(), []
    for c in [ROOT] + sorted(ROOT.parent.parent.glob('*/echo-arcade-main')) + sorted(ROOT.parent.glob('*')):
        if (c / 'tools/install.py').exists() and c.resolve() not in seen:
            seen.add(c.resolve())
            out.append(c)
    return out


def state_files():
    return [c / 'install_state.json' for c in copies() if (c / 'install_state.json').exists()]


def read_state(path: Path):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return {}


def find_game(arg):
    if arg:
        return arg
    for s in state_files():
        game = read_state(s).get('game')
        if game and (Path(game) / 'bin/win10/echovr.exe').exists():
            return Path(game)
    for g in KNOWN_GAMES + [DEFAULT_GAME]:
        if (g / 'bin/win10/echovr.exe').exists():
            return g
    raise SystemExit('Echo VR not found: pass --game "C:\\path\\to\\ready-at-dawn-echo-arena"')


def probe(game: Path):
    """The manifest tools from vendor/Doom-on-EchoVR (they read ECHOVR_GAME when imported)."""
    os.environ['ECHOVR_GAME'] = str(game)
    sys.path.insert(0, str(ROOT / 'vendor/Doom-on-EchoVR/EchoVr-Tablet-Probing'))
    import echovr_pkg as P
    from echovr_patch import ManifestFile
    return P, ManifestFile


def arcade_in(manifest: bytes, P, ManifestFile):
    """(does this manifest carry the ARCADE tab's resources, parsed manifest)"""
    mf = ManifestFile(manifest)
    return (P.typesym('CGTextureResource'), P.sym('echo_arcade_screen_v1')) in mf.index, mf


def original_manifest(P, ManifestFile):
    """The stock manifest from a backup: this copy's, ORIGINAL, or any sibling copy's.

    A candidate must parse and carry none of our resources; one whose hash an install recorded
    as the original wins over one that merely looks stock (a game update could change the stock
    manifest, so the hash is a preference, not a requirement)."""
    known = {read_state(s).get('original_manifest_sha256') for s in state_files()} - {None}
    candidates = [ORIGINAL / MID]
    for c in copies():
        candidates += sorted((c / 'backups').glob(f'*/{MID}'), reverse=True) if (c / 'backups').exists() else []
    best = None
    for f in candidates:
        if not f.exists():
            continue
        data = f.read_bytes()
        try:
            has, mf = arcade_in(data, P, ManifestFile)
        except Exception:
            continue
        if has:
            continue
        if sha_bytes(data) in known:
            return f, data, mf
        best = best or (f, data, mf)
    return best


def keep_original(manifest: Path, P, ManifestFile):
    """Remember the stock manifest in backups/original once we hold a pristine one."""
    data = manifest.read_bytes()
    try:
        has, _ = arcade_in(data, P, ManifestFile)
    except Exception:
        return
    if not has and not (ORIGINAL / MID).exists():
        ORIGINAL.mkdir(parents=True, exist_ok=True)
        (ORIGINAL / MID).write_bytes(data)


# ---- RetroArch: held buttons (tools/patch_retroarch.py) ----

def patch_retroarch():
    if not RETROARCH.exists():
        print('RetroArch not found (apps\\retroarch); run tools\\setup_apps.py for the RetroArch, DOOM and Duck Hunt tiles.')
        return
    sys.path.insert(0, str(ROOT / 'tools'))
    import patch_retroarch
    patch_retroarch.patch(RETROARCH)


# ---- the files we write ----

def write_ini(target: Path):
    apps = ROOT / 'apps'
    ini = f"""; Echo Arcade host configuration (written by tools/install.py)
[paths]
; this folder (the tablet's NORMAL TABLET runs tools\\remove_on_exit.py from it)
root={ROOT}
love={apps / 'love/love-11.5-win64/love.exe'}
balatro_steam={apps / 'balatro/steam'}
retroarch={apps / 'retroarch/retroarch.exe'}
doom_core={apps / 'retroarch/cores/prboom_libretro.dll'}
doom_wad={apps / 'roms/doom/doom1.wad'}
duck_hunt_core={apps / 'retroarch/cores/nestopia_libretro.dll'}
duck_hunt_rom={apps / 'roms/nes/Duck Hunt (World).nes'}
roms={apps / 'roms'}
mpv={apps / 'mpv/mpv.exe'}
; YOUTUBE tile: yt-dlp finds the videos, mpv plays them; your saved links, one "Name | link" per line
yt_dlp={apps / 'mpv/yt-dlp.exe'}
youtube_links={apps / 'youtube.txt'}
; MINECRAFT tile (Java Edition): your launchers are found by themselves (the Minecraft Launcher,
; CurseForge, Modrinth App, Prism Launcher); a path here overrides the search
minecraft=
curseforge=
modrinth=
prism=
; video folders for MOVIES, separated by ;  (environment variables allowed)
movies={apps / 'movies'};%USERPROFILE%\\Videos

[host]
; desktop: game windows sit at the top-left of your monitor, behind other windows
; offscreen: moved off the visible desktop (some apps throttle when off screen)
window_mode=desktop
; where the apps' sound goes: output device name fragments separated by |, first match
; wins (the Rift's headphones, else a Quest over Link / Air Link). "default" = the Windows default device.
audio_device=Rift Audio|Meta Quest|Oculus Virtual Audio
; each app's own volume on that device (Windows' per-app volume), percent
app_volume=65
; light gun for RetroArch games (Duck Hunt style), from your shots on the docked poster:
; touch: aims and fires without taking focus from Echo VR (default)
; focus: gives RetroArch focus and clicks the mouse at the spot; focus comes back after a pause
; off:   shots on RetroArch's picture do nothing
light_gun=touch
; FREEZE (your Touch controllers play RetroArch): A/B/X/Y on the RetroPad by position, like an
; Xbox pad (A bottom = B, B right = A, X left = Y, Y top = X), or "labels" (A = A, B = B ...)
freeze_buttons=position
; 1 = the MOVIES tile instead of MINECRAFT on the launcher
movies_tile=0
balatro_port=55410
retroarch_command_port=55355
retroarch_pad_port=55400

[minecraft]
; FREEZE in Minecraft: how fast the right stick turns you (mouse pixels per tick at full stick)
look_speed=3
"""
    target.mkdir(parents=True, exist_ok=True)
    (target / 'arcade.ini').write_text(ini, encoding='utf-8')
    write_youtube_links()


YOUTUBE_LINKS = """# Echo Arcade - YOUTUBE tile: your saved links, one per line:  Name | link
# A playlist or mix link (one with list= in it) opens as a list of its videos.
# Edit this file any time; the tile reads it each time you open it.
My music mix | https://www.youtube.com/watch?v=uvXbJfhd39E&list=RD6u9FsHX2b-8&index=11
"""


def write_youtube_links():
    """apps/youtube.txt is yours to edit: created once, never overwritten."""
    target = ROOT / 'apps/youtube.txt'
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(YOUTUBE_LINKS, encoding='utf-8')
        print(f'Created {target} (your YouTube links)')


def ensure_loader_entry(plugins):
    # EchoLoader only calls PluginInit for entries that have "args".
    entry = next((e for e in plugins if e.get('file', '').lower() == 'echoarcade.dll'), None)
    if entry is None:
        plugins.append({'file': 'EchoArcade.dll', 'args': {}})
    else:
        entry.setdefault('args', {})


def load_loader(p: Path):
    return json.loads(p.read_text(encoding='utf-8'))


def loader_has_entry(p: Path):
    try:
        return any(e.get('file', '').lower() == 'echoarcade.dll' for e in load_loader(p).get('plugins', []))
    except (OSError, ValueError):
        return False


def check_echoloader(p):
    """Other mod tools (e.g. EchoVR-Haptics) replace bin/win10/dbgcore.dll with
    their own proxy; then EchoLoader, and every plugin it loads, silently stops."""
    dll = p['bin'] / 'dbgcore.dll'
    if dll.exists() and b'EchoLoader' in dll.read_bytes():
        return True
    print('WARNING: bin/win10/dbgcore.dll is not EchoLoader (another mod replaced it?).\n'
          '         Echo Arcade will not load until it is restored: download dbgcore.dll from\n'
          '         https://files.echovr.de/updates/ (the Echo VR Installer\'s source) into bin/win10.')
    return False


def ini_get(text: str, section: str, key: str):
    """A key's value in ini text (None if missing); keys and sections any case, like Windows."""
    current = None
    for line in text.splitlines():
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            current = s[1:-1].strip().lower()
        elif current == section.lower() and '=' in s and not s.startswith(';'):
            k, _, v = s.partition('=')
            if k.strip().lower() == key.lower():
                return v.strip()
    return None


def ini_set(text: str, section: str, key: str, value: str):
    """The ini text with section/key set (added if missing); comments and everything else kept."""
    lines, current, done, last_in_section = text.splitlines(), None, False, None
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            current = s[1:-1].strip().lower()
            if current == section.lower():
                last_in_section = i
        elif current == section.lower():
            last_in_section = i if s else last_in_section
            k, eq, _ = s.partition('=')
            if eq and not s.startswith(';') and k.strip().lower() == key.lower():
                lines[i] = f'{key}={value}'
                done = True
    if not done:
        if last_in_section is None:
            lines += ['', f'[{section}]', f'{key}={value}']
        else:
            lines.insert(last_in_section + 1, f'{key}={value}')
    return '\n'.join(lines) + '\n'


def ini_drop_section(text: str, section: str):
    out, skip = [], False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith('[') and s.endswith(']'):
            skip = s[1:-1].strip().lower() == section.lower()
        if not skip:
            out.append(line)
    return '\n'.join(out).rstrip('\n') + '\n'


def undo_everything_off(p, target: Path):
    """The tablet's ALL OFF / NORMAL TABLET saved what was on in echo_tweaks.ini [before_off]:
    a new install puts it all back (the HUD lines are switched on by their own installer)."""
    text = target.read_text(encoding='utf-8', errors='replace')
    text = ini_set(text, 'normal', 'remove_on_exit', '0') if ini_get(text, 'normal', 'remove_on_exit') else text
    if ini_get(text, 'before_off', 'saved') != '1':
        target.write_text(text, encoding='utf-8')
        return
    scale, dock = ini_get(text, 'before_off', 'tablet_scale'), ini_get(text, 'before_off', 'auto_dock')
    if scale:
        text = ini_set(text, 'tablet', 'scale', scale)
    if dock:
        text = ini_set(text, 'dock', 'auto', dock)
    for name, section, key, value in (('EchoHudPlus', 'general', 'enabled', ini_get(text, 'before_off', 'hud')),
                                      ('DiscButton', 'summon', 'button', ini_get(text, 'before_off', 'disc_button'))):
        ini = p['plugins'] / name / 'settings.ini'
        if value and ini.exists():
            ini.write_text(ini_set(ini.read_text(encoding='utf-8', errors='replace'), section, key, value), encoding='utf-8')
            print(f'{name}: {section} {key}={value} again (as before ALL OFF on the tablet)')
    target.write_text(ini_drop_section(text, 'before_off'), encoding='utf-8')
    print('Put back what the tablet\'s ALL OFF had switched off (tablet size, auto dock).')


def write_tweaks(p):
    """echo_tweaks.ini is yours to edit: created once, never overwritten.

    Seeds from the copy an uninstall saved (backups/<time>/echo_tweaks.ini), else the FOV from
    EchoVR-Haptics' haptics_config.txt if that is present.
    """
    target = p['plugins'] / 'EchoArcade' / 'echo_tweaks.ini'
    if target.exists():  # an update: the tablet is still there to switch things back on
        return
    saved = sorted((ROOT / 'backups').glob('*/echo_tweaks.ini')) if (ROOT / 'backups').exists() else []
    if saved:
        shutil.copy2(saved[-1], target)
        print(f'Restored your settings file {target.name} from {saved[-1].parent.name}')
        undo_everything_off(p, target)
        return
    x = y = 1.0
    old = p['bin'] / 'haptics_config.txt'
    if old.exists():
        for line in old.read_text(errors='ignore').splitlines():
            key, _, value = line.partition('=')
            key = key.strip()
            try:
                if key in ('FovMultiplierX', 'FovMultiplier'):
                    x = float(value)
                if key in ('FovMultiplierY', 'FovMultiplier'):
                    y = float(value)
            except ValueError:
                pass
        # One-axis stretch (e.g. x1.0 y1.5) looks warped in a wide recording window.
        x = y = max(x, y)
    target.write_text(f"""; Echo VR tweaks (read when Echo starts)
[fov]
; Eye field-of-view multipliers, 1.0 to 2.0 (1.0 = normal). Widens the view the
; game renders: visible in the desktop mirror / recordings. Edited only here.
; Keep x and y equal for natural proportions. Higher values cost GPU (1.4 = ~2x the pixels).
x={x:.2f}
y={y:.2f}

[tablet]
; Tablet size, 0.5 to 4.0 (saved by the tablet's SETTINGS tab, applied live)
scale=1.00
""", encoding='utf-8')
    print(f'Created {target.name} (fov x={x:.2f} y={y:.2f})')


def copy_binaries(p):
    shutil.copy2(ROOT / 'dist/EchoArcade.dll', p['plugins'] / 'EchoArcade.dll')
    host_dir = p['plugins'] / 'EchoArcade'
    host_dir.mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'dist/EchoArcade/ArcadeHost.exe', host_dir / 'ArcadeHost.exe')
    write_ini(host_dir)
    write_tweaks(p)


# ---- actions ----

def install(game: Path):
    check_game(game)
    if STATE.exists():
        raise SystemExit('Already installed (install_state.json exists). Run restore first.')
    p = paths(game)
    for f in (ROOT / 'dist/EchoArcade.dll', ROOT / 'dist/EchoArcade/ArcadeHost.exe'):
        if not f.exists():
            raise SystemExit(f'{f} missing: run build.cmd first')
    P, ManifestFile = probe(game)
    keep_original(p['manifest'], P, ManifestFile)
    sys.path.insert(0, str(ROOT / 'tools'))
    import build_arcade_tab
    meta = build_arcade_tab.build()
    if meta['base_manifest_sha256'] != sha(p['manifest']):
        raise SystemExit('Manifest changed during build; try again.')
    package = ROOT / 'build/tab/packages' / meta['package']
    if (p['packages'] / meta['package']).exists():
        raise SystemExit(f'{meta["package"]} already exists in the game; refusing to overwrite it.')

    backup = ROOT / 'backups' / time.strftime('%Y%m%d-%H%M%S')
    backup.mkdir(parents=True)
    shutil.copy2(p['manifest'], backup / MID)
    shutil.copy2(p['loader'], backup / 'echoloader.json')

    shutil.copy2(package, p['packages'] / meta['package'])
    shutil.copy2(ROOT / 'build/tab/manifests' / MID, p['manifest'])
    p['plugins'].mkdir(exist_ok=True)
    copy_binaries(p)
    loader = load_loader(p['loader'])
    plugins = loader.setdefault('plugins', [])
    ensure_loader_entry(plugins)
    p['loader'].write_text(json.dumps(loader, indent=4) + '\n', encoding='utf-8')

    STATE.write_text(json.dumps(dict(game=str(game), backup=str(backup), package=meta['package'], tab_schema=TAB_SCHEMA,
                                     manifest_sha256=sha(p['manifest']), original_manifest_sha256=meta['base_manifest_sha256']), indent=2))
    check_echoloader(p)
    patch_retroarch()
    print(f'Installed. Backups in {backup}')
    print('Start Echo VR normally, open the hand tablet and press the gamepad tab (far left); the gear tab (far right) is SETTINGS.')


def update(game: Path):
    check_game(game)
    if not STATE.exists():
        raise SystemExit('Not installed yet: run install first.')
    state = json.loads(STATE.read_text())
    if state.get('tab_schema', 1) < TAB_SCHEMA:
        raise SystemExit('This version changes the tablet data (DOCK adds a poster screen), so update is not enough.\n'
                         'Close Echo VR and run:  .venv\\Scripts\\python tools\\install.py reinstall')
    p = paths(Path(state['game']))
    copy_binaries(p)
    loader = load_loader(p['loader'])
    ensure_loader_entry(loader.setdefault('plugins', []))
    p['loader'].write_text(json.dumps(loader, indent=4) + '\n', encoding='utf-8')
    print('Updated EchoArcade.dll, ArcadeHost.exe and arcade.ini (tablet data unchanged).')
    check_echoloader(p)
    patch_retroarch()


def restore(game: Path, force: bool):
    check_game(game)
    if not STATE.exists():
        raise SystemExit('Nothing to restore (no install_state.json). To remove an install made from another copy, run uninstall.')
    state = json.loads(STATE.read_text())
    p = paths(Path(state['game']))
    backup = Path(state['backup'])
    if not (backup / MID).exists():
        raise SystemExit(f'The backup {backup / MID} is gone. Run uninstall, which finds the original in another copy.')
    current = sha(p['manifest'])
    if current != state['manifest_sha256'] and not force:
        raise SystemExit('The manifest was changed after Echo Arcade installed (another mod or a game update?). '
                         'Re-run with --force to restore our backup anyway, or run uninstall.')
    shutil.copy2(backup / MID, p['manifest'])
    pkg = p['packages'] / state['package']
    if pkg.exists():
        pkg.unlink()
    loader = load_loader(p['loader'])
    loader['plugins'] = [e for e in loader.get('plugins', []) if e.get('file', '').lower() != 'echoarcade.dll']
    p['loader'].write_text(json.dumps(loader, indent=4) + '\n', encoding='utf-8')
    for f in (p['plugins'] / 'EchoArcade.dll',):
        if f.exists():
            f.unlink()
    host_dir = p['plugins'] / 'EchoArcade'
    if host_dir.exists():  # keep the user's echo_tweaks.ini (FOV, tablet size) for a reinstall
        for f in host_dir.iterdir():
            if f.name != 'echo_tweaks.ini':
                shutil.rmtree(f, ignore_errors=True) if f.is_dir() else f.unlink()
        if not any(host_dir.iterdir()):
            host_dir.rmdir()
        else:
            print(f'Kept your settings file {host_dir / "echo_tweaks.ini"} (delete it by hand if you are uninstalling for good).')
    ok = sha(p['manifest']) == state['original_manifest_sha256']
    STATE.unlink()
    print('Restored original manifest' + (' (hash verified).' if ok else ' (WARNING: hash differs from the pre-install manifest).'))


def uninstall(game: Path, dry: bool):
    """Every original file back, whichever copy of this folder installed Echo Arcade.

    Finds the stock manifest in any copy's backups, removes every package index past the stock
    count (ours, and leftovers of earlier installs), the echoloader.json entry, the plugin and the
    host folder (echo_tweaks.ini is saved under backups/ for the next install)."""
    check_game(game)
    p = paths(game)
    P, ManifestFile = probe(game)
    current = p['manifest'].read_bytes()
    has_arcade, mf = arcade_in(current, P, ManifestFile)
    known = {read_state(s).get('original_manifest_sha256') for s in state_files()} - {None}
    plan, source = [], None
    if has_arcade:
        found = original_manifest(P, ManifestFile)
        if not found:
            raise SystemExit('The tablet data carries Echo Arcade, but no backup of the original manifest was found in\n'
                             f'{ROOT / "backups"} or any sibling copy. Reinstalling Echo VR restores it.')
        source, original, mf = found
        plan.append(f'manifest: put back the original from {source} (sha256 {sha_bytes(original)[:12]}'
                    + (', the one recorded at install)' if sha_bytes(original) in known else ')'))
    else:
        plan.append('manifest: already the original (no Echo Arcade resources in it)')
    stock = mf.npkg
    extra = sorted((f for f in p['packages'].glob(f'{MID}_*') if f.name[len(MID) + 1:].isdigit() and int(f.name[len(MID) + 1:]) >= stock),
                   key=lambda f: int(f.name[len(MID) + 1:]))
    # Only packages provably ours: named by an install_state.json, or byte-identical to one that is
    # (or to the one build_arcade_tab.py wrote). Anything else past the stock count is left alone.
    named = {read_state(s).get('package') for s in state_files()} - {None}
    our_hashes = {sha(p['packages'] / n) for n in named if (p['packages'] / n).exists()}
    our_hashes |= {sha(f) for f in (ROOT / 'build/tab/packages').glob(f'{MID}_*')} if (ROOT / 'build/tab/packages').exists() else set()
    ours = [f for f in extra if f.name in named or sha(f) in our_hashes]
    others = [f for f in extra if f not in ours]
    if ours:
        plan.append(f'packages: delete {len(ours)} Echo Arcade package(s) past the stock {stock} ({ours[0].name} .. {ours[-1].name})')
    if others:
        plan.append(f'packages: leave {len(others)} that are not Echo Arcade\'s alone ({", ".join(f.name for f in others)})')
    if loader_has_entry(p['loader']):
        plan.append('echoloader.json: remove the EchoArcade.dll entry')
    dll, host_dir = p['plugins'] / 'EchoArcade.dll', p['plugins'] / 'EchoArcade'
    if dll.exists():
        plan.append(f'delete {dll}')
    if host_dir.exists():
        plan.append(f'delete {host_dir}' + (' (echo_tweaks.ini saved under backups\\)' if (host_dir / 'echo_tweaks.ini').exists() else ''))
    if STATE.exists():
        plan.append('forget this copy\'s install_state.json')
    if len(plan) == 1 and not has_arcade and not ours and not STATE.exists():
        print('Echo Arcade is not installed in', game)
        return
    print(('Would do:' if dry else 'Uninstalling:'))
    for line in plan:
        print('  -', line)
    if dry:
        return
    backup = ROOT / 'backups' / time.strftime('%Y%m%d-%H%M%S')
    backup.mkdir(parents=True, exist_ok=True)
    shutil.copy2(p['manifest'], backup / f'{MID}.before-uninstall')
    shutil.copy2(p['loader'], backup / 'echoloader.json.before-uninstall')
    if has_arcade:
        p['manifest'].write_bytes(original)
        if not (ORIGINAL / MID).exists():
            ORIGINAL.mkdir(parents=True, exist_ok=True)
            (ORIGINAL / MID).write_bytes(original)
    for f in ours:
        f.unlink()
    if loader_has_entry(p['loader']):
        loader = load_loader(p['loader'])
        loader['plugins'] = [e for e in loader.get('plugins', []) if e.get('file', '').lower() != 'echoarcade.dll']
        p['loader'].write_text(json.dumps(loader, indent=4) + '\n', encoding='utf-8')
    if dll.exists():
        dll.unlink()
    if host_dir.exists():
        if (host_dir / 'echo_tweaks.ini').exists():
            shutil.copy2(host_dir / 'echo_tweaks.ini', backup / 'echo_tweaks.ini')
        shutil.rmtree(host_dir)
    if STATE.exists():
        STATE.unlink()
    now = sha(p['manifest'])
    verdict = 'matches the manifest recorded before the first install' if now in known else 'is a stock manifest (no record of its hash to compare with)'
    print(f'Uninstalled. The manifest {verdict}; what was replaced is under {backup}.')


def setup(game: Path, dry: bool):
    """The INSTALL button: RetroArch patch, a clean slate, then install."""
    check_game(game)
    print('RetroArch:', end=' ')
    patch_retroarch()
    p = paths(game)
    P, ManifestFile = probe(game)
    installed = STATE.exists() or arcade_in(p['manifest'].read_bytes(), P, ManifestFile)[0] or loader_has_entry(p['loader']) \
        or (p['plugins'] / 'EchoArcade.dll').exists()
    if installed:
        print('An earlier Echo Arcade install is in the game; removing it first.')
        uninstall(game, dry)
    if dry:
        print('Would install Echo Arcade from', ROOT)
        return
    install(game)


def status(game: Path):
    p = paths(game)
    print('game:', game)
    print('installed (this copy):', STATE.exists())
    try:
        P, ManifestFile = probe(game)
        print('arcade tab in the tablet data:', arcade_in(p['manifest'].read_bytes(), P, ManifestFile)[0])
    except Exception as e:
        print('arcade tab in the tablet data: unknown (%s)' % e)
    print('plugin present:', (p['plugins'] / 'EchoArcade.dll').exists())
    print('echoloader dll:', check_echoloader(p))
    print('loader entry:', loader_has_entry(p['loader']))
    print('manifest sha256:', sha(p['manifest']))
    if RETROARCH.exists():
        sys.path.insert(0, str(ROOT / 'tools'))
        import patch_retroarch
        print('retroarch:', patch_retroarch.status(RETROARCH)[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('action', choices=['setup', 'uninstall', 'install', 'update', 'reinstall', 'restore', 'status', 'configure'])
    ap.add_argument('--game', type=Path)
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    if a.action == 'configure':
        write_ini(ROOT / 'dist/EchoArcade')
        print('wrote', ROOT / 'dist/EchoArcade/arcade.ini')
        return
    game = find_game(a.game)
    if a.action == 'setup':
        setup(game, a.dry_run)
    elif a.action == 'uninstall':
        uninstall(game, a.dry_run)
    elif a.action == 'install':
        install(game)
    elif a.action == 'update':
        update(game)
    elif a.action == 'reinstall':
        game = Path(json.loads(STATE.read_text())['game']) if STATE.exists() else game
        if STATE.exists():
            restore(game, a.force)
        install(game)
    elif a.action == 'restore':
        restore(game, a.force)
    elif a.action == 'status':
        status(game)


if __name__ == '__main__':
    main()
