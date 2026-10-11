"""Download everything Echo Arcade needs that is not in this repository.

    python tools/setup_apps.py

  vendor/Doom-on-EchoVR   tablet research + package tools (pinned commit), also the shareware DOOM1.WAD
  apps/love               LOVE 11.5 win64 (runs Balatro from your own Steam copy)
  apps/mpv                mpv 0.41 (movie player; local files, Plex) + yt-dlp (YouTube tile)
  apps/retroarch          RetroArch stable (portable) + a starter set of cores
  apps/roms/doom          doom1.wad (shareware) + prboom.wad

Nothing here downloads Balatro or any commercial ROM: bring your own.
"""
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APPS = ROOT / 'apps'
DL = APPS / 'downloads'
DOOM_REPO = 'https://github.com/heisthecat31/Doom-on-EchoVR.git'
DOOM_COMMIT = '8531ed84397c64d10e929f6d3836530bfac161d1'
LOVE = 'https://github.com/love2d/love/releases/download/11.5/love-11.5-win64.zip'
MPV = 'https://github.com/mpv-player/mpv/releases/download/v0.41.0/mpv-v0.41.0-x86_64-w64-mingw32.zip'
RETROARCH = 'https://buildbot.libretro.com/stable/1.22.2/windows/x86_64/RetroArch.7z'
# Latest on purpose: YouTube changes often and old yt-dlp versions stop working. The YOUTUBE
# tile has an "Update yt-dlp" entry for later updates (yt-dlp -U).
YTDLP = 'https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe'
# Must match SYSTEMS in host/playlists.cpp (folder -> core).
SYSTEMS = {
    'atari2600': 'stella', 'atari7800': 'prosystem', 'lynx': 'handy',
    'nes': 'nestopia', 'snes': 'snes9x', 'n64': 'mupen64plus_next', 'gb': 'gambatte', 'gba': 'mgba',
    'nds': 'melondsds', '3ds': 'citra', 'gamecube': 'dolphin', 'wii': 'dolphin', 'virtualboy': 'mednafen_vb',
    'mastersystem': 'genesis_plus_gx', 'gamegear': 'genesis_plus_gx', 'genesis': 'genesis_plus_gx',
    'segacd': 'genesis_plus_gx', '32x': 'picodrive', 'saturn': 'mednafen_saturn', 'dreamcast': 'flycast',
    'psx': 'pcsx_rearmed', 'ps2': 'pcsx2', 'psp': 'ppsspp',
    'pcengine': 'mednafen_pce_fast', 'pcenginecd': 'mednafen_pce_fast', 'ngp': 'mednafen_ngp',
    'wonderswan': 'mednafen_wswan', 'msx': 'bluemsx', 'arcade': 'fbneo', 'mame': 'mame2003_plus',
    'dos': 'dosbox_pure', 'doom': 'prboom',
}
CORES = sorted(set(SYSTEMS.values()))
# Engine support files (not BIOS) from libretro's Core System Files Downloader.
SYSTEM_PACKS = ['Dolphin', 'PPSSPP', 'LRPS2', 'blueMSX', 'PrBoom', 'FinalBurn%20Neo%20%28hiscore%29', 'MAME%202003-Plus']
SYSTEM_URL = 'https://buildbot.libretro.com/assets/system/{}.zip'
CORE_URL = 'https://buildbot.libretro.com/nightly/windows/x86_64/latest/{}_libretro.dll.zip'
PRBOOM_WAD = 'https://github.com/libretro/libretro-prboom/raw/master/prboom.wad'


def fetch(url, dest: Path):
    if dest.exists():
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    print('downloading', url)
    with urllib.request.urlopen(url) as r, open(dest.with_suffix(dest.suffix + '.part'), 'wb') as f:
        shutil.copyfileobj(r, f)
    dest.with_suffix(dest.suffix + '.part').rename(dest)
    return dest


def git(*args, cwd=None):
    subprocess.run(['git', *args], cwd=cwd, check=True)


def fetch_doom_tools(vendor: Path):
    """The pinned Doom-on-EchoVR commit: with git when it's installed, else GitHub's zip of it."""
    if shutil.which('git'):
        if not vendor.exists():
            git('clone', DOOM_REPO, str(vendor))
        git('checkout', '--quiet', DOOM_COMMIT, cwd=vendor)
        return
    stamp = vendor / '.commit'
    if vendor.exists() and stamp.exists() and stamp.read_text().strip() == DOOM_COMMIT:
        return
    archive = fetch(f'https://codeload.github.com/heisthecat31/Doom-on-EchoVR/zip/{DOOM_COMMIT}', DL / f'Doom-on-EchoVR-{DOOM_COMMIT}.zip')
    tmp = vendor.with_name('_doom_unpack')
    shutil.rmtree(tmp, ignore_errors=True)
    zipfile.ZipFile(archive).extractall(tmp)
    inner = next(tmp.iterdir())  # Doom-on-EchoVR-<commit>/
    shutil.rmtree(vendor, ignore_errors=True)
    vendor.parent.mkdir(parents=True, exist_ok=True)
    inner.rename(vendor)
    shutil.rmtree(tmp, ignore_errors=True)
    stamp.write_text(DOOM_COMMIT + '\n')


def main():
    vendor = ROOT / 'vendor/Doom-on-EchoVR'
    fetch_doom_tools(vendor)

    love_zip = fetch(LOVE, DL / 'love-11.5-win64.zip')
    if not (APPS / 'love/love-11.5-win64/love.exe').exists():
        zipfile.ZipFile(love_zip).extractall(APPS / 'love')

    mpv_zip = fetch(MPV, DL / 'mpv-v0.41.0-x86_64-w64-mingw32.zip')
    if not (APPS / 'mpv/mpv.exe').exists():
        zipfile.ZipFile(mpv_zip).extractall(APPS / 'mpv')
        for inner in (APPS / 'mpv').glob('mpv-*.zip'):  # the release wraps the build in a second zip
            zipfile.ZipFile(inner).extractall(APPS / 'mpv')
            inner.unlink()
    (APPS / 'movies').mkdir(exist_ok=True)
    fetch(YTDLP, APPS / 'mpv/yt-dlp.exe')

    ra = fetch(RETROARCH, DL / 'RetroArch.7z')
    if not (APPS / 'retroarch/retroarch.exe').exists():
        tmp = APPS / 'retroarch_tmp'
        tmp.mkdir(exist_ok=True)
        # Windows' bsdtar reads 7z (including the BCJ2 filter py7zr lacks).
        subprocess.run([r'C:\Windows\System32\tar.exe', '-xf', str(ra), '-C', str(tmp)], check=True)
        shutil.move(str(tmp / 'RetroArch-Win64'), str(APPS / 'retroarch'))
        shutil.rmtree(tmp, ignore_errors=True)
    # A button held on the tablet must stay held in RetroArch (tools/patch_retroarch.py explains).
    sys.path.insert(0, str(ROOT / 'tools'))
    import patch_retroarch
    patch_retroarch.patch(APPS / 'retroarch/retroarch.exe')
    for core in CORES:
        z = fetch(CORE_URL.format(core), DL / 'cores' / f'{core}_libretro.dll.zip')
        if not (APPS / 'retroarch/cores' / f'{core}_libretro.dll').exists():
            zipfile.ZipFile(z).extractall(APPS / 'retroarch/cores')

    doom = APPS / 'roms/doom'
    doom.mkdir(parents=True, exist_ok=True)
    shutil.copy2(vendor / 'TabletDoom/data/doom1.wad', doom / 'doom1.wad')
    fetch(PRBOOM_WAD, doom / 'prboom.wad')
    (APPS / 'retroarch/system').mkdir(exist_ok=True)
    shutil.copy2(doom / 'prboom.wad', APPS / 'retroarch/system/prboom.wad')
    for pack in SYSTEM_PACKS:
        z = fetch(SYSTEM_URL.format(pack), DL / 'system' / f'{pack.replace("%20", " ").replace("%28", "(").replace("%29", ")")}.zip')
        marker = APPS / 'retroarch/system' / f'.{z.stem}.unpacked'
        if not marker.exists():
            zipfile.ZipFile(z).extractall(APPS / 'retroarch/system')
            marker.write_text('ok')
    for system in SYSTEMS:
        (APPS / 'roms' / system).mkdir(exist_ok=True)
    shutil.copy2(ROOT / 'docs/ROMS.md', APPS / 'roms/README.md')
    print('Done. Next: python tools/prepare_balatro.py, build.cmd, python tools/install.py install')
    return 0


if __name__ == '__main__':
    sys.exit(main())
