"""NORMAL TABLET (the tablet's SETTINGS > NORMAL): take Echo Arcade off once Echo VR closes.

Started by ArcadeHost (with this folder's .venv pythonw, outside the job that closes the host
with the game). Waits until echovr.exe and ArcadeHost.exe are gone, then uninstalls: the whole
Echo Mods Pack (HUD, HUD lines and the arcade) when this folder is the pack's arcade\\, else
Echo Arcade (tools/install.py uninstall). Tapping CANCEL on the tablet (echo_tweaks.ini
[normal] remove_on_exit=0) stops it before then. INSTALL.cmd brings everything back.

    pythonw tools/remove_on_exit.py --game GAME --tweaks ...\\EchoArcade\\echo_tweaks.ini [--dry-run]

Log: remove_on_exit.log in this folder. Only one runs at a time.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOG = ROOT / 'remove_on_exit.log'
PACK = ROOT.parent / 'install_pack.py'  # this folder is the Echo Mods Pack's arcade\ when it exists
kernel = ctypes.WinDLL('kernel32', use_last_error=True)
kernel.CreateToolhelp32Snapshot.restype = wt.HANDLE
CREATE_NO_WINDOW = 0x08000000


def log(text):
    with LOG.open('a', encoding='utf-8') as f:
        f.write(time.strftime('%Y-%m-%d %H:%M:%S ') + text + '\n')


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD), ('th32DefaultHeapID', ctypes.c_size_t),
                ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD), ('th32ParentProcessID', wt.DWORD),
                ('pcPriClassBase', ctypes.c_long), ('dwFlags', wt.DWORD), ('szExeFile', wt.WCHAR * 260)]


def running(*names):
    """Which of these program names are running (no console window, unlike tasklist)."""
    snap = kernel.CreateToolhelp32Snapshot(2, 0)  # TH32CS_SNAPPROCESS
    found, entry = set(), PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    wanted = {n.lower() for n in names}
    ok = kernel.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() in wanted:
            found.add(entry.szExeFile.lower())
        ok = kernel.Process32NextW(snap, ctypes.byref(entry))
    kernel.CloseHandle(snap)
    return found


def still_wanted(tweaks: Path):
    if not tweaks.exists():
        return False
    return kernel.GetPrivateProfileIntW('normal', 'remove_on_exit', 0, str(tweaks)) != 0


def tell(text, error=False):
    """A message box on the desktop: Echo is closed by now, so the player sees it."""
    ctypes.windll.user32.MessageBoxW(None, text, 'Echo Mods - normal tablet', 0x00040000 | 0x00010000 | (0x10 if error else 0x40))


def uninstall_command(game: Path):
    py = ROOT / '.venv' / 'Scripts' / 'python.exe'
    if PACK.is_file():
        return [str(py), str(PACK), '--uninstall', '--game', str(game)]
    return [str(py), str(ROOT / 'tools' / 'install.py'), 'uninstall', '--game', str(game)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', type=Path, required=True)
    ap.add_argument('--tweaks', type=Path, required=True)
    ap.add_argument('--dry-run', action='store_true', help='log what it would run instead of running it')
    ap.add_argument('--poll', type=float, default=2.0)
    a = ap.parse_args()
    mutex = kernel.CreateMutexW(None, False, 'Local\\EchoArcade.RemoveOnExit')
    if ctypes.get_last_error() == 183:  # ERROR_ALREADY_EXISTS: one is waiting already
        return
    log(f'waiting for Echo VR to close (game {a.game}){" [dry run]" if a.dry_run else ""}')
    clear_since = None
    while True:
        if not still_wanted(a.tweaks):
            log('cancelled on the tablet')
            return
        if running('echovr.exe', 'ArcadeHost.exe'):
            clear_since = None
        elif clear_since is None:
            clear_since = time.monotonic()
        elif time.monotonic() - clear_since >= 5:  # closed for 5 s, not just between restarts
            break
        time.sleep(a.poll)
    command = uninstall_command(a.game)
    # Off in the saved copy too: the next install brings Echo Arcade back without scheduling this again.
    kernel.WritePrivateProfileStringW('normal', 'remove_on_exit', '0', str(a.tweaks))
    log('Echo VR closed; running: ' + ' '.join(command))
    if a.dry_run:
        log('dry run: nothing changed')
        return
    result = subprocess.run(command, capture_output=True, text=True, creationflags=CREATE_NO_WINDOW, input='', cwd=str(ROOT))
    log((result.stdout + result.stderr).strip())
    log(f'finished with exit code {result.returncode}')
    if result.returncode == 0:
        tell('Echo VR is back to normal: Echo Arcade is off the tablet' + (', and the HUD and HUD lines are removed' if PACK.is_file() else '')
             + '.\n\nTo bring it all back, run INSTALL.cmd again.')
    else:
        tell(f'Taking Echo Arcade off did not finish (see {LOG}).\n\nClose Echo VR and run UNINSTALL.cmd.', error=True)
    kernel.CloseHandle(mutex)


if __name__ == '__main__':
    try:
        main()
    except Exception as e:  # pythonw has no console: the log is the only place to say it
        log(f'error: {e!r}')
        sys.exit(1)
