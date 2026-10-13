"""Echo Mods Pack: one install for the bottom HUD (EchoHudPlus + HUD lines) and Echo Arcade.

    INSTALL.cmd     python install_pack.py
    UNINSTALL.cmd   python install_pack.py --uninstall
    CHECK.cmd       python install_pack.py --check      (what is installed and whether it runs: paste check.txt when asking for help)

Options: --game PATH   your ready-at-dawn-echo-arena folder (found by itself in the usual places)
         --no-arcade   only the HUD
         --no-hud      only Echo Arcade
         --reset-hud   switch to this version's HUD settings (an earlier install keeps its own otherwise)
         --keep-loader keep the game's own EchoLoader (bin/win10/dbgcore.dll). Without this, the pack's
                       copy goes in: a different build is replaced (kept as dbgcore.dll.before-pack), since
                       a loader that loads no plugins is the usual reason nothing of the pack shows up
         --no-disc     leave the personal disc out even when its add-on is here

The personal disc is a separate download (EchoPersonalDisc.zip): put it in this folder, or next
to it, and INSTALL puts it in too (UNINSTALL takes it out). Without it, nothing of the disc.

Close Echo VR first. Needs Python 3.10+, internet (Python packages, RetroArch and the other
arcade apps), and EchoLoader in the game (the community Echo VR installer sets it up).
Nothing of Echo VR is in this pack: the HUD's font and the HUD-lines patch are made from your
own game files, and the apps download from their official sources.
"""
import argparse
import importlib.util
import json
import os
import struct
import subprocess
import sys
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ARCADE = HERE / 'arcade'
VENV_PY = ARCADE / '.venv' / 'Scripts' / 'python.exe'
PACKAGES = ['zstandard==0.25.0', 'pillow', 'texture2ddecoder']
PKG_TOOLS = ARCADE / 'vendor' / 'Doom-on-EchoVR' / 'EchoVr-Tablet-Probing'  # Echo package reader (downloaded)
ECHO_BUILD = 1683152886  # echovr.exe timestamp of the final Echo VR build (the one community servers run)
LOADER = HERE / 'loader' / 'dbgcore.dll'  # EchoLoader as the community installer sets it up (see loader\README.txt)
LOADER_JSON = {'log_path': 'plugin_logs', 'plugins_dir': 'plugins', 'plugins': []}
DISC_NAME = 'EchoPersonalDisc'  # the add-on's zip / folder name
DISC_LINK = 'https://github.com/Decay-echo/echo-personal-disc-addon/releases/latest/download/EchoPersonalDisc.zip'


def disc_here(folder: Path):
    return (folder / 'patch_personal_disc.py').is_file() and (folder / 'discbutton' / 'install_discbutton.py').is_file()


def find_disc():
    """The Personal Disc add-on, when it is here: a `disc` or `EchoPersonalDisc` folder in this
    folder or an `EchoPersonalDisc` folder next to it (unzipped, with or without its top folder),
    else the newest EchoPersonalDisc*.zip here, next to this folder or in Downloads, unpacked
    into `disc`. None when it is nowhere."""
    for folder in (HERE / 'disc', HERE / DISC_NAME, HERE.parent / DISC_NAME):
        for candidate in (folder, folder / DISC_NAME):
            if disc_here(candidate):
                return candidate
    zips = []
    for where in (HERE, HERE.parent, Path.home() / 'Downloads'):
        try:
            zips += [z for z in where.glob(DISC_NAME + '*.zip') if z.is_file()]
        except OSError:
            pass
    for z in sorted(zips, key=lambda z: z.stat().st_mtime, reverse=True):
        try:
            with zipfile.ZipFile(z) as zf:
                names = [n for n in zf.namelist() if not n.endswith('/')]
                roots = {n.split('/')[0] for n in names if n.count('/') >= 1 and n.endswith('patch_personal_disc.py')}
                root = (roots.pop() + '/') if roots and 'patch_personal_disc.py' not in names else ''
                if not root and 'patch_personal_disc.py' not in names:
                    continue
                target = HERE / 'disc'
                if target.exists():
                    import shutil
                    shutil.rmtree(target)
                for n in names:
                    if not n.startswith(root) or '..' in n.split('/'):
                        continue
                    dest = target / n[len(root):]
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(zf.read(n))
            if disc_here(target):
                print(f'Personal disc add-on: {z.name} unpacked into {target}')
                return target
        except (OSError, zipfile.BadZipFile) as e:
            print(f'Personal disc add-on: {z} could not be read ({e})')
    return None


def game_helpers():
    spec = importlib.util.spec_from_file_location('echo_game', HERE / 'echo_game.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def note_outcome(text):
    """What the last INSTALL / UNINSTALL ended with, for CHECK.cmd."""
    try:
        (HERE / 'last_install.txt').write_text(time.strftime('%Y-%m-%d %H:%M') + ' ' + ' '.join(sys.argv[1:]) + ': ' + text + '\n', encoding='utf-8')
    except OSError:
        pass


def stop(text):
    print('\nSTOPPED: ' + text)
    note_outcome('STOPPED: ' + text)
    sys.exit(1)


def run(args, what, check=True, env=None, cwd=None):
    """Runs a step with the arcade's Python environment, showing its output."""
    result = subprocess.run([str(a) for a in args], env=env, cwd=cwd)
    if check and result.returncode != 0:
        stop(f'{what} failed (see above). Nothing after this step was changed.')
    return result.returncode == 0


def find_game(given, g):
    if given:
        try:
            return g.game_root(g.resolve_target(given))
        except FileNotFoundError as e:
            stop(str(e))
    found = g.discover()
    if len(found) == 1:
        return g.game_root(found[0])
    print('Echo VR was not found in the usual places.' if not found else 'Several Echo VR installs were found.')
    while True:
        answer = input('Paste your Echo VR folder (ready-at-dawn-echo-arena, the one with bin\\win10\\echovr.exe), '
                       'or press Enter to quit: ').strip()
        if not answer:
            sys.exit(1)
        try:
            return g.game_root(g.resolve_target(answer))
        except FileNotFoundError as e:
            print(e)


def exe_timestamp(exe: Path):
    data = exe.read_bytes()[:4096]
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    return struct.unpack_from('<I', data, pe + 8)[0]


def loader_plugins(win10: Path):
    try:
        return [p.get('file', '').lower() for p in json.loads((win10 / 'echoloader.json').read_text(encoding='utf-8')).get('plugins', [])
                if isinstance(p, dict)]
    except (OSError, ValueError):
        return []


def sha256(path: Path):
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ensure_loader(game: Path, keep: bool):
    """EchoLoader in the game: the pack's copy goes in when there is none, and in place of a
    different build (kept as dbgcore.dll.before-pack) unless --keep-loader."""
    win10 = game / 'bin' / 'win10'
    dll, cfg = win10 / 'dbgcore.dll', win10 / 'echoloader.json'
    if not LOADER.is_file():
        if not dll.is_file() or not cfg.is_file():
            stop('EchoLoader is not installed in this game (bin\\win10\\dbgcore.dll and echoloader.json) and this copy of the '
                 'pack carries none. It comes with the community Echo VR installer: set that up, start Echo VR once, close it, '
                 'then run INSTALL.cmd again.')
        return
    if dll.is_file() and sha256(dll) == sha256(LOADER):
        pass
    elif not dll.is_file() or not keep:
        if dll.is_file():
            backup = dll.with_name('dbgcore.dll.before-pack')
            if not backup.exists():
                dll.replace(backup)
            else:
                dll.unlink()
            print(f'Replaced EchoLoader (bin\\win10\\dbgcore.dll) with the pack\'s; the old one is kept as {backup.name}.')
        else:
            print('EchoLoader was not in the game: put the pack\'s in (bin\\win10\\dbgcore.dll).')
        import shutil
        shutil.copy2(LOADER, dll)
    else:
        print('NOTE: the game\'s EchoLoader (bin\\win10\\dbgcore.dll) is a different build than the pack\'s and stays (--keep-loader). '
              'If the plugins do not load (no HUD, the arcade tab does nothing), run INSTALL.cmd without it.')
    if not cfg.is_file():
        cfg.write_text(json.dumps(LOADER_JSON, indent=4) + '\n', encoding='utf-8')
        print('Created bin\\win10\\echoloader.json.')
    (win10 / 'plugins').mkdir(exist_ok=True)


def check_game(game: Path, want_hud_lines: bool, keep_loader: bool = False):
    """Stops on what can't work; returns whether the HUD lines (NvrAssetPatches) can go in."""
    win10 = game / 'bin' / 'win10'
    exe = win10 / 'echovr.exe'
    if not exe.is_file():
        stop(f'{exe} not found.')
    if exe_timestamp(exe) != ECHO_BUILD:
        stop('This Echo VR build is not the one the pack was made for (the final build community servers use). '
             'Nothing was changed.')
    ensure_loader(game, keep_loader)
    if not want_hud_lines:
        return False
    has_plugin = (win10 / 'plugins' / 'NvrAssetPatches.dll').is_file() and 'nvrassetpatches.dll' in loader_plugins(win10)
    if not has_plugin:
        print('NOTE: NvrAssetPatches is not installed, so the HUD lines part is skipped: Echo\'s own speed, mute icon and '
              'connection bars will show on top of the new HUD, and the visor lines stay. Install NvrAssetPatches '
              '(community Echo VR tools) and run INSTALL.cmd again to add it.')
        return False
    manifest = win10 / 'asset_patches' / 'manifest.json'
    if not manifest.is_file():
        manifest.parent.mkdir(exist_ok=True)
        manifest.write_text(json.dumps({'version': '1.0', 'patches': []}, indent=4) + '\n', encoding='utf-8')
        print('Created an empty asset_patches\\manifest.json for NvrAssetPatches.')
    return True


def python_env():
    if not VENV_PY.is_file():
        print('Creating the Python environment (arcade\\.venv)...')
        run([sys.executable, '-m', 'venv', ARCADE / '.venv'], 'Creating the Python environment')
    print('Python packages: ' + ', '.join(PACKAGES))
    run([VENV_PY, '-m', 'pip', 'install', '--disable-pip-version-check', '-q', *PACKAGES], 'Installing the Python packages')


def install(args):
    g = game_helpers()
    print('ECHO MODS PACK - INSTALL')
    print('from:', HERE)
    game = find_game(args.game, g)
    print('game:', game)
    if g.game_running():
        stop('Echo VR is running. Close it completely, then run this again.')
    hud_lines = check_game(game, not args.no_hud, args.keep_loader)
    disc = None if args.no_disc else find_disc()
    total = 7
    print(f'\n[1/{total}] Python environment')
    python_env()
    print(f'\n[2/{total}] Downloads (Echo package tools{", RetroArch + cores, LOVE, mpv, DOOM shareware" if not args.no_arcade else ""})')
    if args.no_arcade:  # the HUD still needs the Echo package reader
        run([VENV_PY, '-c', 'import sys; sys.path.insert(0, r"%s"); import setup_apps as s; '
             's.fetch_doom_tools(s.ROOT / "vendor/Doom-on-EchoVR")' % (ARCADE / 'tools')], 'Downloading the Echo package tools')
    else:
        run([VENV_PY, ARCADE / 'tools' / 'setup_apps.py'], 'Downloading the arcade apps', cwd=ARCADE)
        print('\nBalatro (optional, from your own Steam copy):')
        if not run([VENV_PY, ARCADE / 'tools' / 'prepare_balatro.py'], 'Balatro', check=False, cwd=ARCADE):
            print('Balatro not set up (no Steam copy found?): its tile will say what is missing. Everything else works.')
    env = dict(os.environ, ECHOVR_GAME=str(game))
    print(f'\n[3/{total}] Reading your game files (HUD font{", HUD-lines patch" if hud_lines else ""})')
    if not args.no_hud:
        if not run([VENV_PY, HERE / 'hudplus' / 'make_digits.py', PKG_TOOLS], 'Reading Echo\'s speed font', check=False, env=env):
            print('Could not read Echo\'s speed font: the HUD uses its built-in digits instead.')
        if hud_lines:
            run([VENV_PY, HERE / 'hud_lines' / 'make_hud_lines_patch.py', PKG_TOOLS], 'Making the HUD-lines patch', env=env)
    print(f'\n[4/{total}] Echo Arcade')
    if args.no_arcade:
        print('skipped (--no-arcade)')
    else:
        run([VENV_PY, ARCADE / 'tools' / 'install.py', 'setup', '--game', game, *(['--dry-run'] if args.dry_run else [])],
            'Installing Echo Arcade', cwd=ARCADE)
    print(f'\n[5/{total}] HUD')
    if args.no_hud:
        print('skipped (--no-hud)')
    elif args.dry_run:
        print('would install', HERE / 'hudplus' / 'build' / 'EchoHudPlus.dll', 'and', HERE / 'hudplus' / 'data' / 'echo_digits.bin')
    else:
        run([VENV_PY, HERE / 'hudplus' / 'install_hudplus.py', '--target', game, *(['--reset-settings'] if args.reset_hud else [])],
            'Installing the HUD')
    print(f'\n[6/{total}] HUD lines')
    if not hud_lines:
        print('skipped')
    elif args.dry_run:
        print('would install', ', '.join(p.name for p in sorted((HERE / 'hud_lines' / 'patch').iterdir())))
    else:
        run([VENV_PY, HERE / 'hud_lines' / 'install_hud_lines.py', '--target', game], 'Installing the HUD lines')
    print(f'\n[7/{total}] Personal disc (a separate download)')
    if args.no_disc:
        print('skipped (--no-disc)')
    elif not disc:
        print('not here: nothing of the disc is installed. For it, download ' + DISC_LINK)
        print('into this folder and run INSTALL.cmd again.')
    elif args.dry_run:
        print('from', disc)
        run([VENV_PY, disc / 'patch_personal_disc.py', '--target', game, '--check'], 'Checking the personal disc patch', check=False)
    else:
        print('from', disc)
        run([VENV_PY, disc / 'patch_personal_disc.py', '--target', game], 'Installing the personal disc patch')
        run([VENV_PY, disc / 'discbutton' / 'install_discbutton.py', '--target', game], 'Installing the disc button')
    if args.dry_run:
        print('\nDRY RUN finished: the game was not changed.')
        return
    print('\nALL DONE. Start Echo VR.')
    if not args.no_hud:
        print(' - HUD: at the bottom of your view, the pass counter at the top (it needs Echo\'s Settings > Game > '
              'API access on).')
    if disc:
        print(' - Personal disc: everywhere but live play; press X on the left controller and it comes to your hand '
              '(the DISC page of the tablet\'s settings changes the button).')
    if not args.no_arcade:
        print(' - Settings: the arcade\'s gear tab on the hand tablet (HUD, glow, visor lines, and NORMAL to switch it '
              'all off, or take it all out when Echo closes).')
        print(' - Arcade: open the hand tablet and press the gamepad tab (far left). FREEZE (top of the launcher) '
              'lets your Touch controllers play RetroArch or Minecraft; both hands above your head for 5 s to move again.')
        print('   Your games go in ' + str(ARCADE / 'apps' / 'roms') + ' (one folder per system, see the README there).')
    note_outcome('ALL DONE')


def tail(path: Path, n=12):
    try:
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
    except OSError:
        return None
    return lines[-n:]


def check(args):
    """What is in the game and whether it ran: for pasting when something does not show up."""
    g = game_helpers()
    out = []

    def say(text=''):
        print(text)
        out.append(text)

    say('ECHO MODS PACK - CHECK  (' + time.strftime('%Y-%m-%d %H:%M') + ')')
    say('pack: ' + str(HERE))
    game = find_game(args.game, g)
    win10 = game / 'bin' / 'win10'
    say('game: ' + str(game))
    say('Echo VR running now: ' + ('yes (start it and open the tablet, then close it and run this again)' if g.game_running() else 'no'))
    say('')
    last = tail(HERE / 'last_install.txt', 3)
    say('[last INSTALL / UNINSTALL from this folder]')
    say('  ' + (last[-1] if last else 'no record (an older version of the pack, or never run from here)'))
    say('')
    say('[EchoLoader]')
    loader = win10 / 'echoloader.json'
    dll = win10 / 'dbgcore.dll'
    if dll.is_file():
        same = LOADER.is_file() and sha256(dll) == sha256(LOADER)
        say(f'  dbgcore.dll: {dll.stat().st_size} bytes, sha256 {sha256(dll)[:16]}... ' + ('(the pack\'s build)' if same else '(a DIFFERENT build than the pack\'s: INSTALL.cmd puts the pack\'s in)'))
    else:
        say('  dbgcore.dll: MISSING (EchoLoader is not installed)')
    log_dir = 'plugin_logs'
    try:
        raw = loader.read_text(encoding='utf-8')
        cfg = json.loads(raw)
        log_dir = str(cfg.get('log_path', 'plugin_logs'))
        say('  echoloader.json plugins: ' + ', '.join(p.get('file', '?') if isinstance(p, dict) else str(p) for p in cfg.get('plugins', [])))
        say('  echoloader.json as it is:')
        for l in raw.splitlines()[:40]:
            say('    ' + l.rstrip()[:160])
    except (OSError, ValueError) as e:
        say(f'  echoloader.json: cannot read ({e})')
    say('  everything in plugins\\:')
    try:
        for f in sorted((win10 / 'plugins').iterdir()):
            say(f'    {f.name}' + (f'  ({f.stat().st_size} bytes)' if f.is_file() else '  (folder)'))
    except OSError as e:
        say(f'    cannot list: {e}')
    for name in ('EchoArcade.dll', 'EchoHudPlus.dll'):
        if not (win10 / 'plugins' / name).is_file():
            say(f'  plugins\\{name}: MISSING (quarantined by an antivirus, or never installed)')
    # The loader's log: where echoloader.json says, else any loader log under bin\win10.
    logs = [win10 / log_dir / 'loader.log', win10 / 'plugin_logs' / 'loader.log']
    try:
        logs += sorted((f for f in win10.rglob('*loader*.log') if f.is_file()), key=lambda f: f.stat().st_mtime, reverse=True)
    except OSError:
        pass
    seen, log = set(), None
    for candidate in logs:
        if candidate in seen or not candidate.is_file():
            continue
        seen.add(candidate)
        log = log or candidate
    other = [str(f.relative_to(win10)) for f in seen if f != log]
    if other:
        say('  other loader logs: ' + ', '.join(other))
    lines = tail(log, 400) if log else None
    if lines is None:
        say(f'  loader log: none under {win10} (Echo VR has not started with EchoLoader since the install, or EchoLoader writes no log)')
    else:
        say(f'  loader log: {log} (changed {time.strftime("%Y-%m-%d %H:%M", time.localtime(log.stat().st_mtime))})')
        starts = [i for i, l in enumerate(lines) if 'Loading plugin:' in l]
        last = lines[max(0, starts[0] - 5):] if starts else lines[-20:]
        # the last start of Echo: from its first "Loading plugin" to its summary
        session = []
        for l in reversed(lines):
            session.append(l)
            if '===' in l and 'EchoLoader' in l and 'Summary' not in l:
                break
        session = list(reversed(session))[-40:]
        summary = [l for l in lines if 'Summary' in l]
        say('  loader.log, last start: ' + (summary[-1].strip() if summary else '(no summary line)'))
        for l in session:
            if any(k in l for k in ('FAIL', 'fail', 'error', 'Error', 'skipped', 'Loading plugin', 'Summary', 'echovr')):
                say('    ' + l.strip()[:160])
    say('')
    say('[the mods\' own logs]')
    local = Path(os.environ.get('LOCALAPPDATA', ''))
    for label, path in (('HUD', local / 'EchoHudPlus' / 'hudplus.log'), ('arcade plugin', local / 'EchoArcade' / 'runtime.log'),
                        ('arcade host', local / 'EchoArcade' / 'host.log')):
        lines = tail(path, 6)
        if lines is None:
            say(f'  {label}: no log at {path} (it never ran)')
        else:
            say(f'  {label}: {path}')
            for l in lines:
                say('    ' + l.strip()[:160])
    # The dock, from the arcade plugin's log of the last Echo start: the posters it recognised
    # in each lobby, where it docked, and whether a turned picture (the central display) was fed.
    runtime = tail(local / 'EchoArcade' / 'runtime.log', 3000) or []
    starts = [i for i, l in enumerate(runtime) if 'started ArcadeHost' in l or 'arcade runtime' in l.lower()]
    session = runtime[starts[-1]:] if starts else runtime[-400:]
    keep = [l for l in session if ('posters:' in l and not any(k in l for k in ('No lobby poster here', 'Docked on ', 'Undocked', 'near the poster', 'touched the poster', 'fingertips from', 'hands from')))
            or (('stream: turned' in l or 'stream: poster' in l) and 'frames uploaded' not in l)]
    if keep:
        say('  dock, last Echo start (%d lines):' % len(keep))
        for l in keep[-40:]:
            say('    ' + l.strip()[:200])
    say('')
    say('[personal disc]')
    disc = find_disc()
    say('  add-on: ' + (str(disc) if disc else 'not here (a separate download: ' + DISC_LINK + ')'))
    try:
        scripts = win10 / 'scripts'
        known = {}
        if disc:  # the patcher's own records: stock and patched hashes of both DLLs
            text = (disc / 'patch_personal_disc.py').read_text(encoding='utf-8', errors='replace')
            import re
            for name, stock, patched in re.findall(r"'([0-9a-f]{16}\.dll)': \{'stock': '([0-9a-f]{64})', 'patched': '([0-9a-f]{64})'", text):
                known.setdefault(name, (stock, []))[1].append(patched)  # this version's record first, older ones after
        for name in ('351c49438bd38225.dll', '38965d90a823f03f.dll'):
            f = scripts / name
            if not f.is_file():
                say(f'  {name}: MISSING')
                continue
            h = sha256(f)
            stock, patched = known.get(name, ('', []))
            state = ('stock (no disc patch)' if h == stock else 'PATCHED (this add-on\'s version)' if patched and h == patched[0]
                     else 'patched, an OLDER version (INSTALL updates it)' if h in patched else 'unknown version' if known else '')
            say(f'  {name}: sha256 {h[:16]}... {state}')
        button = win10 / 'plugins' / 'DiscButton.dll'
        say('  DiscButton.dll: ' + (f'{button.stat().st_size} bytes' if button.is_file() else 'not installed'))
        ini = win10 / 'plugins' / 'DiscButton' / 'settings.ini'
        if ini.is_file():
            say('  settings.ini: ' + ' | '.join(l.strip() for l in ini.read_text(encoding='utf-8', errors='replace').splitlines() if l.strip() and not l.startswith(';'))[:200])
        lines = tail(local / 'DiscButton' / 'discbutton.log', 6)
        if lines:
            say('  discbutton.log (last lines):')
            for l in lines:
                say('    ' + l.strip()[:160])
    except OSError as e:
        say(f'  could not look: {e}')
    say('')
    say('[tablet data]')
    try:
        sys.path.insert(0, str(ARCADE / 'tools'))
        import install as arcade_install
        p = arcade_install.paths(game)
        pr, mf_cls = arcade_install.probe(game)
        data = p['manifest'].read_bytes()
        has, mf = arcade_install.arcade_in(data, pr, mf_cls)
        say(f'  manifest: {mf.npkg} packages (stock has 3), Echo Arcade tab: {"yes" if has else "NO"}')
        say('  packages: ' + ', '.join(sorted(f.name for f in p['packages'].glob('48037dc70b0ecab2*'))))
    except Exception as e:  # noqa: BLE001
        say('  not read: the Echo package tools are not in this copy (INSTALL downloads them)' if 'echovr_pkg' in str(e)
            else f'  could not read the tablet data: {e}')
    say('')
    say('[antivirus]')
    try:
        script = ('$names = @{}; Get-MpThreat -ErrorAction SilentlyContinue | ForEach-Object { $names[[string]$_.ThreatID] = $_.ThreatName }; '
                  'Get-MpThreatDetection -ErrorAction SilentlyContinue | Sort-Object InitialDetectionTime -Descending | Select-Object -First 12 | '
                  'ForEach-Object { $files = @($_.Resources | ForEach-Object { ($_ -split "\\|")[0] } | Where-Object { $_ -like "file:*" } | '
                  'ForEach-Object { $_ -replace "^file:_", "" }); $_.InitialDetectionTime.ToString("s") + "  " + $names[[string]$_.ThreatID] + '
                  '"  status " + $_.CurrentThreatExecutionStatusID + "/" + $_.ThreatStatusID + "  " + ($files -join ", ") }')
        r = subprocess.run(['powershell', '-NoProfile', '-Command', script], capture_output=True, text=True, timeout=90)
        hits = [l.strip() for l in r.stdout.splitlines() if l.strip()]
        ours = [l for l in hits if any(k in l for k in ('EchoArcade', 'EchoHudPlus', 'EchoModsPack', 'ArcadeHost', 'retroarch', 'ready-at-dawn'))]
        if ours:
            say('  Defender detections touching the pack (threat name, status, files):')
            for l in ours:
                say('    ' + l[:300])
            say('  -> Windows Security > Protection history: Allow / Restore these, and add the EchoModsPack folder and')
            say('     the game\'s bin\\win10\\plugins folder under Virus & threat protection settings > Exclusions. Then INSTALL again.')
        else:
            say('  Defender detections touching the pack: ' + ('none seen' if not r.returncode else 'could not ask'))
    except (OSError, subprocess.TimeoutExpired):
        say('  Defender: could not ask')
    (HERE / 'check.txt').write_text('\n'.join(out) + '\n', encoding='utf-8')
    say('')
    say('Written to ' + str(HERE / 'check.txt') + ': paste that file when asking for help.')


def uninstall(args):
    g = game_helpers()
    print('ECHO MODS PACK - UNINSTALL')
    game = find_game(args.game, g)
    print('game:', game)
    if g.game_running():
        stop('Echo VR is running. Close it completely, then run this again.')
    if not VENV_PY.is_file():
        stop('arcade\\.venv is missing: this copy of the pack never installed. Use the copy you installed from.')
    win10 = game / 'bin' / 'win10'
    if not args.no_hud:
        if (win10 / 'asset_patches' / 'hud_lines').exists():
            run([VENV_PY, HERE / 'hud_lines' / 'install_hud_lines.py', '--target', game, '--uninstall'], 'Removing the HUD lines')
        run([VENV_PY, HERE / 'hudplus' / 'install_hudplus.py', '--target', game, '--uninstall'], 'Removing the HUD')
    if not args.no_arcade:
        run([VENV_PY, ARCADE / 'tools' / 'install.py', 'uninstall', '--game', game], 'Removing Echo Arcade', cwd=ARCADE)
    disc = None if args.no_disc else find_disc()
    if disc:
        if (win10 / 'plugins' / 'DiscButton.dll').is_file():
            run([VENV_PY, disc / 'discbutton' / 'install_discbutton.py', '--target', game, '--uninstall'], 'Removing the disc button', check=False)
        if not run([VENV_PY, disc / 'patch_personal_disc.py', '--target', game, '--restore'], 'Restoring the original DLLs', check=False):
            print('(the disc patch was not in, or not this version: nothing to restore)')
    elif (win10 / 'plugins' / 'DiscButton.dll').is_file():
        print('The personal disc stays in: its add-on (EchoPersonalDisc) is not here. Its own RESTORE.cmd puts the original DLLs back.')
    print('\nDone: Echo VR is back to its own files (your arcade games and downloads stay in this folder).')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', help='your ready-at-dawn-echo-arena folder')
    ap.add_argument('--uninstall', action='store_true')
    ap.add_argument('--check', action='store_true', help='what is installed and whether it ran (writes check.txt)')
    ap.add_argument('--keep-loader', action='store_true', help="keep the game's own EchoLoader instead of the pack's")
    ap.add_argument('--loader', action='store_true', help=argparse.SUPPRESS)  # earlier versions: now the default
    ap.add_argument('--no-arcade', action='store_true', help='leave Echo Arcade out')
    ap.add_argument('--no-hud', action='store_true', help='leave the HUD out')
    ap.add_argument('--no-disc', action='store_true', help='leave the personal disc out even when its add-on is here')
    ap.add_argument('--dry-run', action='store_true', help='download and prepare everything, but change nothing in the game')
    ap.add_argument('--reset-hud', action='store_true', help="use this pack's HUD settings (the old ones are kept as settings.ini.old)")
    args = ap.parse_args()
    if sys.version_info < (3, 10):
        stop('Python 3.10 or newer is needed (python.org).')
    if len(str(HERE)) > 100:  # the Python packages and RetroArch cores sit deep inside: Windows' 260-character limit
        stop(f'This folder\'s path is too long for Windows ({len(str(HERE))} characters):\n  {HERE}\n'
             'Move the EchoModsPack folder somewhere shorter, for example C:\\EchoModsPack or Documents\\EchoModsPack, '
             'and run INSTALL.cmd from there.')
    if args.check:
        check(args)
        return
    (uninstall if args.uninstall else install)(args)


if __name__ == '__main__':
    main()
