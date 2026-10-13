"""Builds the Personal Disc add-on (maintainer's PC): the files go into a repo folder and into
dist/EchoPersonalDisc.zip, a separate download the pack's INSTALL.cmd picks up when it sits in
(or next to) the EchoModsPack folder.
    python tools/make_disc_addon.py [REPO_DIR]      (default: ..\\echo-personal-disc-addon)
Takes from the Personal Disc project folder (ECHO_DISC_SRC): the v3.5 patcher, its README and
source notes, echo_game.py (the game finder DiscButton's installer shares), and DiscButton
(source, installer, built DLL, MinHook with its licence). No Echo VR files.
"""
import hashlib
import os
import re
import shutil
import sys
import zipfile
from pathlib import Path

PACK = Path(__file__).resolve().parent.parent
DISC = Path(os.environ.get('ECHO_DISC_SRC', Path.home() / 'Downloads' / 'PersonalDiscEverywhere_v3.0.1 (1)' / 'PersonalDiscEverywhere_v3.0.1'))
FILES = [
    'patch_personal_disc.py', 'INSTALL.cmd', 'RESTORE.cmd', 'README.md', 'echo_game.py',
    'src/README.md', 'src/round_guard.c',
    'discbutton/discbutton.cpp', 'discbutton/discbutton_core.h', 'discbutton/discbutton_test.cpp',
    'discbutton/build_discbutton.cmd', 'discbutton/install_discbutton.py', 'discbutton/build/DiscButton.dll',
    'discbutton/minhook/AUTHORS.txt', 'discbutton/minhook/LICENSE.txt', 'discbutton/minhook/README.md',
]
TREES = ['discbutton/minhook/include', 'discbutton/minhook/src']
# The two original script DLLs of the final build, from the maintainer's own game (the patcher's
# backups there), checked against the patcher's own stock hashes: INSTALL puts them in place
# when a DLL was changed by another mod and no copy of the original lies beside it.
GAME = Path(os.environ.get('ECHOVR_GAME', r'C:\Program Files\Meta Horizon\Software\Software\ready-at-dawn-echo-arena'))
ORIGINALS_README = """The two original script DLLs of the final Echo VR build (echovr.exe 1683152886), as the game
ships them, for putting a DLL back that another mod changed. The pack's INSTALL.cmd uses them only
when the game's file is neither the original nor a known disc version and no copy of the original
lies beside it; the changed file is kept as <name>.before-pack. Hashes (SHA-256) are the ones the
patcher checks against.
"""
ADDON_README = """# Echo VR Personal Disc add-on (for the Echo Mods Pack)

The personal disc everywhere in Echo VR (lobby, timeouts, and from a goal until the launcher
countdown ends), summoned with a Touch button. Two parts:

- `patch_personal_disc.py` (INSTALL.cmd / RESTORE.cmd): the Personal Disc Everywhere patch, v3.5.
  It changes two script DLLs of your own game by a few bytes and keeps backups; RESTORE.cmd puts
  the originals back. Details and the exact rules: README.md.
- `discbutton\\`: DiscButton.dll, an EchoLoader plugin. Press X (left controller; A, B or Y in its
  settings) and the disc comes to your hand. The arcade tablet's SETTINGS gets a DISC page.

## Install

Put **EchoPersonalDisc.zip** in your **EchoModsPack** folder (or unzip it there, or next to the
pack folder) and run the pack's **INSTALL.cmd**: it installs the disc along with the HUD and the
arcade. **UNINSTALL.cmd** takes it out again (RESTORE.cmd here does the DLLs alone). The pack's
CHECK.cmd shows whether the disc patch and the button are in.

Without the pack: close Echo VR, run `INSTALL.cmd` here, then `py -3 discbutton\\install_discbutton.py`.

## Credits

The patch is [nmdurkee's Personal Disc Everywhere](https://github.com/nmdurkee/EchoVR-Personal-Disc-Everywhere)
(v3.0.1), continued here as v3.5 (the disc stays from a goal until the launcher countdown ends,
no arm popup). It is redistributed here without a licence from its author; it comes down the
day he asks. DiscButton by Decay-echo. [MinHook](https://github.com/TsudaKageyu/minhook) (BSD
2-Clause, `discbutton\\minhook\\LICENSE.txt`). The patch edits your own game's DLLs on your PC;
`originals\\` carries the two original script DLLs of the final build so the pack's INSTALL can put
one back that another mod changed (see originals\\README.txt).
"""


def main():
    repo = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else PACK.parent / 'echo-personal-disc-addon'
    for rel in FILES:
        if not (DISC / rel).is_file():
            sys.exit(f'missing {DISC / rel}')
    for item in list(repo.iterdir()) if repo.is_dir() else []:
        if item.name in ('.git', 'dist'):
            continue
        shutil.rmtree(item, ignore_errors=True) if item.is_dir() else item.unlink()
    repo.mkdir(parents=True, exist_ok=True)
    for rel in FILES:
        (repo / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(DISC / rel, repo / rel)
    for rel in TREES:
        shutil.copytree(DISC / rel, repo / rel, ignore=shutil.ignore_patterns('__pycache__', '*.obj'))
    text = (DISC / 'patch_personal_disc.py').read_text(encoding='utf-8')
    stocks = {}
    for name, stock in re.findall(r"'([0-9a-f]{16}\.dll)': \{'stock': '([0-9a-f]{64})'", text):
        stocks.setdefault(name, stock)
    (repo / 'originals').mkdir(exist_ok=True)
    for name, stock in stocks.items():
        src = GAME / 'bin' / 'win10' / 'scripts' / (name + '.personal-disc.original')
        if not src.is_file():
            sys.exit(f'missing {src}')
        if hashlib.sha256(src.read_bytes()).hexdigest() != stock:
            sys.exit(f'{src} is not the original (hash differs from the patcher\'s record)')
        shutil.copy2(src, repo / 'originals' / name)
    (repo / 'originals' / 'README.txt').write_text(ORIGINALS_README + ''.join(f'{n}  {s}\n' for n, s in stocks.items()), encoding='utf-8')
    (repo / 'PATCH_README.md').write_text((repo / 'README.md').read_text(encoding='utf-8'), encoding='utf-8')
    (repo / 'README.md').write_text(ADDON_README, encoding='utf-8')
    (repo / '.gitignore').write_text('/dist/\n__pycache__/\n', encoding='utf-8')
    out = PACK / 'dist' / 'EchoPersonalDisc.zip'
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for path in sorted(repo.rglob('*')):
            rel = path.relative_to(repo)
            if rel.parts[0] in ('.git', 'dist', '.gitignore') or '__pycache__' in rel.parts or path.is_dir():
                continue
            z.write(path, Path('EchoPersonalDisc') / rel)
    print(f'{out} ({out.stat().st_size / 1e6:.2f} MB) from {repo}')


if __name__ == '__main__':
    main()
