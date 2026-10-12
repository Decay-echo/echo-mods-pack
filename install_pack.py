"""Echo Mods Pack: one install for the bottom HUD (EchoHudPlus + HUD lines) and Echo Arcade.

    INSTALL.cmd     python install_pack.py
    UNINSTALL.cmd   python install_pack.py --uninstall

Options: --game PATH   your ready-at-dawn-echo-arena folder (found by itself in the usual places)
         --no-arcade   only the HUD
         --no-hud      only Echo Arcade
         --reset-hud   switch to this version's HUD settings (an earlier install keeps its own otherwise)

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
from pathlib import Path

HERE = Path(__file__).resolve().parent
ARCADE = HERE / 'arcade'
VENV_PY = ARCADE / '.venv' / 'Scripts' / 'python.exe'
PACKAGES = ['zstandard==0.25.0', 'pillow', 'texture2ddecoder']
PKG_TOOLS = ARCADE / 'vendor' / 'Doom-on-EchoVR' / 'EchoVr-Tablet-Probing'  # Echo package reader (downloaded)
ECHO_BUILD = 1683152886  # echovr.exe timestamp of the final Echo VR build (the one community servers run)


def game_helpers():
    spec = importlib.util.spec_from_file_location('echo_game', HERE / 'echo_game.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def stop(text):
    print('\nSTOPPED: ' + text)
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


def check_game(game: Path, want_hud_lines: bool):
    """Stops on what can't work; returns whether the HUD lines (NvrAssetPatches) can go in."""
    win10 = game / 'bin' / 'win10'
    exe = win10 / 'echovr.exe'
    if not exe.is_file():
        stop(f'{exe} not found.')
    if exe_timestamp(exe) != ECHO_BUILD:
        stop('This Echo VR build is not the one the pack was made for (the final build community servers use). '
             'Nothing was changed.')
    if not (win10 / 'echoloader.json').is_file() or not (win10 / 'dbgcore.dll').is_file():
        stop('EchoLoader is not installed in this game (bin\\win10\\dbgcore.dll and echoloader.json). It comes with the '
             'community Echo VR installer: set that up, start Echo VR once, close it, then run INSTALL.cmd again.')
    (win10 / 'plugins').mkdir(exist_ok=True)
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
    hud_lines = check_game(game, not args.no_hud)
    total = 6
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
    if args.dry_run:
        print('\nDRY RUN finished: the game was not changed.')
        return
    print('\nALL DONE. Start Echo VR.')
    if not args.no_hud:
        print(' - HUD: at the bottom of your view (settings: bin\\win10\\plugins\\EchoHudPlus\\settings.ini).')
    if not args.no_arcade:
        print(' - Arcade: open the hand tablet and press the gamepad tab (far left). FREEZE (top of the launcher) '
              'lets your Touch controllers play RetroArch; hold the menu button 1 s to move again.')
        print('   Your games go in ' + str(ARCADE / 'apps' / 'roms') + ' (one folder per system, see the README there).')


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
    print('\nDone: Echo VR is back to its own files (your arcade games and downloads stay in this folder).')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', help='your ready-at-dawn-echo-arena folder')
    ap.add_argument('--uninstall', action='store_true')
    ap.add_argument('--no-arcade', action='store_true', help='leave Echo Arcade out')
    ap.add_argument('--no-hud', action='store_true', help='leave the HUD out')
    ap.add_argument('--dry-run', action='store_true', help='download and prepare everything, but change nothing in the game')
    ap.add_argument('--reset-hud', action='store_true', help="use this pack's HUD settings (the old ones are kept as settings.ini.old)")
    args = ap.parse_args()
    if sys.version_info < (3, 10):
        stop('Python 3.10 or newer is needed (python.org).')
    if len(str(HERE)) > 100:  # the Python packages and RetroArch cores sit deep inside: Windows' 260-character limit
        stop(f'This folder\'s path is too long for Windows ({len(str(HERE))} characters):\n  {HERE}\n'
             'Move the EchoModsPack folder somewhere shorter, for example C:\\EchoModsPack or Documents\\EchoModsPack, '
             'and run INSTALL.cmd from there.')
    (uninstall if args.uninstall else install)(args)


if __name__ == '__main__':
    main()
