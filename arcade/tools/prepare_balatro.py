"""Prepare Balatro from the player's own Steam copy.

    python tools/prepare_balatro.py [--exe PATH\\Balatro.exe]

Balatro.exe is a fused LOVE executable: the game is a zip appended to it. We
extract it (read-only; the Steam install is never touched) into:

  apps/balatro/steam       original game + Echo Arcade touch bridge

Re-run after Steam updates Balatro.
"""
import argparse
import hashlib
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPS = ROOT / 'apps'
BRIDGE = ROOT / 'bridge/arcade_bridge.lua'
BALATRO_APPID = '2379780'


def steam_libraries():
    roots = [Path(r'C:\Program Files (x86)\Steam'), Path(r'C:\Program Files\Steam')]
    libs = []
    for root in roots:
        vdf = root / 'steamapps/libraryfolders.vdf'
        if vdf.exists():
            libs += [Path(p.replace('\\\\', '\\')) for p in re.findall(r'"path"\s+"([^"]+)"', vdf.read_text(errors='ignore'))]
        libs.append(root)
    return list(dict.fromkeys(libs))


def find_exe():
    for lib in steam_libraries():
        exe = lib / 'steamapps/common/Balatro/Balatro.exe'
        if exe.exists():
            return exe
    return None


def extract(exe: Path, dest: Path):
    dest.mkdir(parents=True, exist_ok=True)
    for child in dest.iterdir():  # clear contents (the folder itself may be someone's cwd)
        shutil.rmtree(child) if child.is_dir() else child.unlink()
    with zipfile.ZipFile(exe) as z:  # zip readers find the archive appended to the exe
        z.extractall(dest)
    if not (dest / 'main.lua').exists():
        raise SystemExit(f'{exe} does not contain a LOVE game (no main.lua)')


def add_bridge(dest: Path):
    shutil.copy2(BRIDGE, dest / 'arcade_bridge.lua')
    main = dest / 'main.lua'
    text = main.read_text(encoding='utf-8')
    if 'arcade_bridge' not in text:
        # Must run after main.lua has defined love.update and the Game class.
        main.write_text(text.rstrip('\n') + '\n\n-- Echo Arcade (tablet touch bridge; inert outside Echo VR)\nrequire("arcade_bridge")\n', encoding='utf-8')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', type=Path)
    args = ap.parse_args()
    exe = args.exe or find_exe()
    if not exe or not exe.exists():
        print('Balatro.exe not found in any Steam library. Install Balatro from Steam, or pass --exe.')
        return 1
    digest = hashlib.sha256(exe.read_bytes()).hexdigest()
    print('Using', exe, digest[:16])
    dest = APPS / 'balatro/steam'
    extract(exe, dest)
    add_bridge(dest)
    # Native helpers from the player's install: https and the Steam API.
    for name in ('https.dll', 'luasteam.dll', 'steam_api64.dll', 'steam_appid.txt'):
        if (exe.parent / name).exists():
            shutil.copy2(exe.parent / name, dest / name)
    (dest / 'echo_arcade_source.txt').write_text(f'{exe}\n{digest}\n')
    print('Prepared', dest)
    return 0


if __name__ == '__main__':
    sys.exit(main())
