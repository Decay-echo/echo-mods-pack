"""Unpack a ROM set archive into apps/roms/<system>/, including zips nested inside it.

    python tools/import_roms.py "C:\\path\\Nintendo - DS.zip" nds
    python tools/import_roms.py "C:\\path\\Atari - 2600.zip" atari2600

Games are written as plain ROM files (faster to load than zips, especially for
DS/GBA). Existing files with the same name and size are skipped, so re-running
is safe. Playlists refresh the next time RetroArch opens from the tablet.
"""
import argparse
import io
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROMS = ROOT / 'apps/roms'
SKIP = ('.txt', '.nfo', '.url', '.jpg', '.png', '.xml', '.dat')


def write(dest: Path, name: str, data_or_reader, size: int):
    target = dest / Path(name).name
    if target.exists() and target.stat().st_size == size:
        return False
    tmp = target.with_suffix(target.suffix + '.part')
    with open(tmp, 'wb') as out:
        if isinstance(data_or_reader, bytes):
            out.write(data_or_reader)
        else:
            while chunk := data_or_reader.read(1 << 20):
                out.write(chunk)
    tmp.replace(target)
    return True


def unpack(archive: zipfile.ZipFile, dest: Path, depth=0):
    added = skipped = 0
    for info in archive.infolist():
        if info.is_dir() or info.filename.lower().endswith(SKIP):
            continue
        if info.filename.lower().endswith('.zip') and depth < 2:
            with zipfile.ZipFile(io.BytesIO(archive.read(info))) as inner:
                a, s = unpack(inner, dest, depth + 1)
            added += a; skipped += s
        else:
            with archive.open(info) as reader:
                if write(dest, info.filename, reader, info.file_size):
                    added += 1
                    print('  +', Path(info.filename).name, flush=True)
                else:
                    skipped += 1
    return added, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('archive', type=Path)
    ap.add_argument('system', help='folder under apps/roms, e.g. nds, gba, atari2600 (see docs/ROMS.md)')
    a = ap.parse_args()
    dest = ROMS / a.system
    if not dest.is_dir():
        raise SystemExit(f'{dest} is not a known system folder (see docs/ROMS.md)')
    with zipfile.ZipFile(a.archive) as z:
        added, skipped = unpack(z, dest)
    print(f'{a.archive.name}: {added} added, {skipped} already present -> {dest}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
