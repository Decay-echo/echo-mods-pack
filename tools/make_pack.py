"""Refreshes this pack from the source projects (maintainer's PC) and writes dist/EchoModsPack.zip.

    python tools/make_pack.py

Takes from the Personal Disc project folder: echo_game.py, the HUD (installer, font
extractor, source, built EchoHudPlus.dll) and the HUD lines (installer, patch maker). From
Echo Arcade: its committed files (git archive HEAD) plus the built EchoArcade.dll and
ArcadeHost.exe. Refuses if Echo Arcade has uncommitted changes, so the source in the pack is
what the binaries were built from.

Never copied: anything made from Echo VR's files (the HUD font, the HUD-lines patch, the
arcade's poster table and tablet data), the Personal Disc patch (its upstream has no
license), apps, ROMs and Python environments.
"""
import io
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

PACK = Path(__file__).resolve().parent.parent
# The source projects (override with ECHO_DISC_SRC / ECHO_ARCADE_SRC).
DISC = Path(os.environ.get('ECHO_DISC_SRC', Path.home() / 'Downloads' / 'PersonalDiscEverywhere_v3.0.1 (1)' / 'PersonalDiscEverywhere_v3.0.1'))
ARCADE = Path(os.environ.get('ECHO_ARCADE_SRC', Path.home() / 'Downloads' / 'echo-arcade-main10' / 'echo-arcade-main'))

FROM_DISC = {
    'echo_game.py': 'echo_game.py',
    'hudplus/install_hudplus.py': 'hudplus/install_hudplus.py',
    'hudplus/make_digits.py': 'hudplus/make_digits.py',
    'hudplus/README.md': 'hudplus/README.md',
    'hudplus/hudplus.cpp': 'hudplus/src/hudplus.cpp',
    'hudplus/hudplus_core.h': 'hudplus/src/hudplus_core.h',
    'hudplus/game_memory.h': 'hudplus/src/game_memory.h',
    'hudplus/build/EchoHudPlus.dll': 'hudplus/build/EchoHudPlus.dll',
    'hud_lines/install_hud_lines.py': 'hud_lines/install_hud_lines.py',
    'hud_lines/make_hud_lines_patch.py': 'hud_lines/make_hud_lines_patch.py',
    'hud_lines/README.md': 'hud_lines/README.md',
}
ARCADE_BUILT = ['dist/EchoArcade.dll', 'dist/EchoArcade/ArcadeHost.exe']
ZIP_SKIP = {'.git', 'tools', 'dist', '.gitignore', '.gitattributes'}


def main():
    dirty = subprocess.run(['git', 'status', '--porcelain'], cwd=ARCADE, capture_output=True, text=True).stdout.strip()
    if dirty:
        sys.exit('Echo Arcade has uncommitted changes; commit them (and rebuild) first:\n' + dirty)
    for src, dst in FROM_DISC.items():
        target = PACK / dst
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(DISC / src, target)
    arcade = PACK / 'arcade'
    for keep in ('.venv', 'apps', 'vendor', 'backups', 'install_state.json'):  # a test install's own files
        if (arcade / keep).exists():
            sys.exit(f'{arcade / keep} exists (a test install?): clear it before rebuilding the pack')
    shutil.rmtree(arcade, ignore_errors=True)
    archive = subprocess.run(['git', 'archive', '--format=tar', 'HEAD'], cwd=ARCADE, capture_output=True, check=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(arcade, filter='data')
    for rel in ARCADE_BUILT:
        (arcade / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ARCADE / rel, arcade / rel)
    head = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ARCADE, capture_output=True, text=True).stdout.strip()
    (arcade / 'PACK_SOURCE.txt').write_text(f'Echo Arcade at commit {head}, with dist/ built from it.\n', encoding='utf-8')

    out = PACK / 'dist' / 'EchoModsPack.zip'
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for path in sorted(PACK.rglob('*')):
            rel = path.relative_to(PACK)
            if rel.parts[0] in ZIP_SKIP or '__pycache__' in rel.parts or path.is_dir():
                continue
            z.write(path, Path('EchoModsPack') / rel)
    print(f'{out} ({out.stat().st_size / 1e6:.1f} MB, arcade @ {head})')


if __name__ == '__main__':
    main()
