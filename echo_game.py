"""Where Echo VR is installed, and whether it is running: shared by the HUD and HUD-lines
installers. Python 3.8+, standard library only.

A game folder is recognised by bin\\win10\\scripts\\351c49438bd38225.dll, a script DLL every
Echo VR install has. Paths can be given as the game folder, bin\\win10, the scripts folder,
echovr.exe or that DLL.
"""
import os
import subprocess
from pathlib import Path

MARKER = '351c49438bd38225.dll'
STORES = ('Meta Horizon', 'Oculus')  # Program Files\<store>\Software\Software\ready-at-dawn-echo-arena


def game_running():
    """True while echovr.exe is running."""
    listing = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq echovr.exe', '/FO', 'CSV', '/NH'],
                             capture_output=True, text=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return '"echovr.exe"' in listing.stdout.lower()


def resolve_target(value):
    """The marker DLL (bin\\win10\\scripts\\351c49438bd38225.dll) for any of the accepted paths."""
    path = Path(os.path.expandvars(str(value).strip().strip('"'))).expanduser()
    if path.is_file():
        options = [path.parent / 'scripts' / MARKER] if path.name.lower() == 'echovr.exe' else [path]
    else:
        options = [path / 'bin' / 'win10' / 'scripts' / MARKER, path / 'scripts' / MARKER, path / MARKER]
    for option in options:
        if option.name.lower() == MARKER and option.is_file():
            return option.resolve()
    raise FileNotFoundError(f'No Echo VR install found at {path} (looked for bin\\win10\\scripts\\{MARKER})')


def discover():
    """Every Echo VR install in the usual places (and around this folder), as marker DLL paths."""
    places = [Path(__file__).resolve().parent, Path.cwd()]
    for variable in ('ProgramW6432', 'ProgramFiles', 'ProgramFiles(x86)'):
        base = os.environ.get(variable)
        if base:
            places += [Path(base) / store / 'Software' / 'Software' / 'ready-at-dawn-echo-arena' for store in STORES]
    found = []
    for place in places:
        try:
            marker = resolve_target(place)
        except FileNotFoundError:
            continue
        if marker not in found:
            found.append(marker)
    return found


def game_root(marker):
    """The game folder (ready-at-dawn-echo-arena) for a marker DLL path."""
    return Path(marker).parent.parent.parent.parent
