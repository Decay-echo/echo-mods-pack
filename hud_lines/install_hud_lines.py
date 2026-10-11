"""Hides Echo VR's visor HUD lines, keeps the mute icon, speed and connection bars,
and moves that readout to the top of the view with the icons spread out.

Uses NvrAssetPatches (EchoLoader plugin): adds "hud_lines_*" entries to
bin\\win10\\asset_patches\\manifest.json that replace the visor line textures'
headers: transparent embedded mips, and the full size shrunk to those mips so the
2048x1024 lines are never streamed in. The other HUD elements are canvases with
their own textures and are not touched.
    py -3 install_hud_lines.py [--target GAME_FOLDER]
    py -3 install_hud_lines.py --uninstall
"""
import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import sys

HERE = Path(__file__).resolve().parent
PREFIX = 'hud_lines_'
TEXTURE_TYPE = '0xB3EBE9E0BF0EB81F'  # sym("CGTextureResource")
TEXTURES = ('c98ec7f37bb9c291', 'c98ec7f37bbfc38a')  # visor line art: BC1 colour, BC3 colour+alpha
# Readout layout (make_hud_lines_patch.py): moved to the top, mute and connection spread out.
LAYOUT = (  # label, type_hash, name_hash, file
    ('layout_transforms', '0x9C72178D05ADB686', '0x3F9915D3001DC28E', 'r14_glb_global_mp.transforms'),  # CTransformCR
    ('layout_speed', '0x46222C5D2AFEB3FF', '0xD66FCF1B64026EBD', 'd66fcf1b64026ebd.canvas'),            # CUICanvasResource
    ('layout_network', '0x46222C5D2AFEB3FF', '0x2D06E14794A11E31', '2d06e14794a11e31.canvas'),
)

spec = importlib.util.spec_from_file_location('patcher', HERE.parent / 'echo_game.py')  # find the game, is it running
patcher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patcher)


def asset_patches(target):
    if target:
        scripts = patcher.resolve_target(target).parent
    else:
        found = patcher.discover()
        if len(found) != 1:
            raise RuntimeError('Pass --target with your Echo VR game folder.')
        scripts = found[0].parent
    folder = scripts.parent / 'asset_patches'
    if not (folder / 'manifest.json').is_file():
        raise RuntimeError('NvrAssetPatches manifest not found: ' + str(folder / 'manifest.json'))
    return folder


def entries():
    rows = [dict(label=PREFIX + texture, type_hash=TEXTURE_TYPE, name_hash='0x' + texture.upper(),
                 file='hud_lines/%s.desc' % texture, buffer_index=0, enabled=True) for texture in TEXTURES]
    rows += [dict(label=PREFIX + label, type_hash=kind, name_hash=name, file='hud_lines/' + file,
                  buffer_index=0, enabled=True) for label, kind, name, file in LAYOUT]
    return rows


def files():
    return ['%s.desc' % t for t in TEXTURES] + [row[3] for row in LAYOUT]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', help='Echo game folder, echovr.exe, scripts folder, or controller DLL')
    parser.add_argument('--uninstall', action='store_true')
    args = parser.parse_args()
    folder = asset_patches(args.target)
    if patcher.game_running():  # Echo holds the patch files open; a half-done swap would break them
        raise RuntimeError('Close Echo VR completely first.')
    manifest_path = folder / 'manifest.json'
    manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
    backup = folder / 'manifest.json.hudlines-backup'
    if not backup.exists():
        shutil.copy2(manifest_path, backup)
    kept = [p for p in manifest.get('patches', []) if not str(p.get('label', '')).startswith(PREFIX)]
    if args.uninstall:
        manifest['patches'] = kept
        manifest_path.write_text(json.dumps(manifest, indent=4) + '\n', encoding='utf-8')
        shutil.rmtree(folder / 'hud_lines', ignore_errors=True)
        print('Visor lines restored (restart Echo VR).')
        return
    out = folder / 'hud_lines'
    shutil.rmtree(out, ignore_errors=True)  # also drops files from older versions
    out.mkdir()
    for name in files():
        shutil.copy2(HERE / 'patch' / name, out / name)
    manifest['patches'] = kept + entries()
    manifest_path.write_text(json.dumps(manifest, indent=4) + '\n', encoding='utf-8')
    print('Visor lines hidden: %d patches in %s' % (len(entries()), out))
    print('Restart Echo VR. Check: %s' % (folder.parent / 'plugin_logs' / 'NvrAssetPatches' / 'NvrAssetPatches.log'))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print('ERROR: ' + str(error), file=sys.stderr)
        sys.exit(1)
