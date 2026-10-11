"""Builds the transparent visor-line texture headers for install_hud_lines.py.

Reads the game's packages, so it needs zstandard and echovr_pkg.py. Run it with
EchoArcade's environment:
    <echo-arcade>\\.venv\\Scripts\\python.exe make_hud_lines_patch.py <echo-arcade>\\vendor\\Doom-on-EchoVR\\EchoVr-Tablet-Probing

The helmet's visor line art is two streamed 2048x1024 textures used by helmet
material 858d5ac84cc5eadf: c98ec7f37bb9c291 (BC1) and c98ec7f37bbfc38a (BC3, the
alpha). Each CGTextureResource "Win10" buffer holds a 256-byte header plus an
embedded DDS with mips 128x64..1x1. Mips 256x128..2048x1024 are separate
streamed resources named sym("_mip8".."_mip11", seed=texture) that NvrAssetPatches
never sees. This writes the two header buffers with transparent embedded mips and
the full size set to the embedded size, so nothing larger gets streamed in.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
import echovr_pkg as P  # noqa: E402

TEXTURES = {0xC98EC7F37BB9C291: 8, 0xC98EC7F37BBFC38A: 16}  # name -> bytes per 4x4 block
CLEAR_BLOCK = {8: bytes(4) + b'\xff' * 4, 16: bytes(16)}      # transparent black (BC1 / BC3)
OUT = Path(__file__).resolve().parent / 'patch'

# Readout layout (mute icon | speed | connection), canvases d66fcf1b64026ebd and
# 2d06e14794a11e31 on helmet transform af0f60ef729284b5 in r14_glb_global_mp.
GLOBAL_LEVEL = 'r14_glb_global_mp'
READOUT_TRANSFORM = 0xAF0F60EF729284B5  # stock local position (0, -0.198, 0.319)
READOUT_HEIGHT = -0.10                  # metres above eye level: bottom of the view (stock -0.198)
# EchoHudPlus draws your speed (with the last-run speed under it), the mute icon and
# the connection bars itself, in the HUD colour, so Echo's own speed text, mute sprite
# and (always white) connection bars are moved off the canvas. Set HIDE_ICONS = False
# to keep them, the icons spread either side of the speed.
# (The speed text's size can't be changed here: element +0x94 is its character
# capacity, 33, not a font size, and Echo has no bigger version of that font.)
HIDE_ICONS = True
OFF_CANVAS = 2000                       # canvas px: far outside the 512 px canvas
SPREAD = 80                             # canvas px from the speed's centre to each icon's centre
ICON_DROP = 20                          # canvas px the mute and connection icons sit below the speed
SPEED_CANVAS, MUTE_SPRITE = 0xD66FCF1B64026EBD, 0xDE495C45BDC1B51B          # stock centre -24
SPEED_TEXT = 0x41D2CA380E26150D
NETWORK_CANVAS, NETWORK_PANELS = 0x2D06E14794A11E31, (0xAF05B299E02227C0, 0x444647855627D557)
NETWORK_ICON_CENTRE = 44                # bars sprite centre in stock layout (panel centre 45, sprite -1)


def shift_elements(canvas, roots, dx, dy):
    """Move elements (dx, dy) canvas px, y down. Element records (0xe0 from +0x1b0)
    keep their position in three forms, all relative to the parent: rect
    +0x14..+0x20 (x0 y0 x1 y1), offsets +0x34..+0x40, position +0x4c/+0x50.
    Children move with their parent unchanged."""
    count = struct.unpack_from('<Q', canvas, 0x80)[0]
    names = [struct.unpack_from('<Q', canvas, 0x1b0 + i * 0xe0)[0] for i in range(count)]
    if any(r not in names for r in roots):
        raise SystemExit('canvas layout changed (game update?)')
    for root in roots:
        at = 0x1b0 + names.index(root) * 0xe0
        for offs, d in (((0x14, 0x1c, 0x34, 0x3c, 0x4c), dx), ((0x18, 0x20, 0x38, 0x40, 0x50), dy)):
            for off in offs:
                struct.pack_into('<f', canvas, at + off, struct.unpack_from('<f', canvas, at + off)[0] + d)
    return canvas


def layout(manifests, out):
    def get(cls, name):
        t = P.typesym(cls)
        return bytearray(next(b for m in manifests if (b := m.get(t, name)) is not None))

    transforms = get('CTransformCR', P.sym(GLOBAL_LEVEL))
    count, stride = struct.unpack_from('<Q', transforms, 0x28)[0], 0xb0
    hits = 0
    for i in range(count):
        at = 0x38 + i * stride
        if struct.unpack_from('<Q', transforms, at)[0] == READOUT_TRANSFORM:
            x, y, z = struct.unpack_from('<3f', transforms, at + 0x90)
            if abs(y + 0.198) > 1e-3 or abs(z - 0.319) > 1e-3:
                raise SystemExit('readout transform changed (game update?): %s' % ((x, y, z),))
            struct.pack_into('<f', transforms, at + 0x94, READOUT_HEIGHT)
            hits += 1
    if hits != 2:
        raise SystemExit('expected 2 readout transforms, found %d' % hits)
    (out / 'r14_glb_global_mp.transforms').write_bytes(transforms)

    speed = get('CUICanvasResource', SPEED_CANVAS)
    network = get('CUICanvasResource', NETWORK_CANVAS)
    if HIDE_ICONS:
        shift_elements(speed, [MUTE_SPRITE, SPEED_TEXT], OFF_CANVAS, 0)
        shift_elements(network, list(NETWORK_PANELS), OFF_CANVAS, 0)
    else:
        shift_elements(speed, [MUTE_SPRITE], -SPREAD + 24, ICON_DROP)
        shift_elements(network, list(NETWORK_PANELS), SPREAD - NETWORK_ICON_CENTRE, ICON_DROP)
    (out / ('%016x.canvas' % SPEED_CANVAS)).write_bytes(speed)
    (out / ('%016x.canvas' % NETWORK_CANVAS)).write_bytes(network)
    print('layout: readout %.3f m from eye level; speed text and mute/connection icons %s'
          % (READOUT_HEIGHT, 'hidden (EchoHudPlus draws them)' if HIDE_ICONS else 'shown, icons at -%d / +%d px' % (SPREAD, SPREAD)))


def main():
    tt = P.typesym('CGTextureResource')
    manifests = P.open_all()
    OUT.mkdir(exist_ok=True)
    layout(manifests, OUT)
    for name, block in TEXTURES.items():
        cpu = next(b for m in manifests if (b := m.get(tt, name)) is not None)
        if cpu[256:260] != b'DDS ':
            raise SystemExit('%016x: no embedded DDS (game update?)' % name)
        payload = len(cpu) - 256 - 148
        if payload % block:
            raise SystemExit('%016x: unexpected embedded mip size %d' % (name, payload))
        patched = bytearray(cpu[:256 + 148] + CLEAR_BLOCK[block] * (payload // block))
        # The streamed mips bypass NvrAssetPatches' hook, so shrink the texture to its
        # embedded mips (+0xc4 width, +0xc8 height, +0xcc mip count): nothing left to stream.
        height, width = struct.unpack_from('<II', cpu, 256 + 12)
        mips = struct.unpack_from('<I', cpu, 256 + 28)[0]
        struct.pack_into('<III', patched, 0xc4, width, height, mips)
        patched = bytes(patched)
        (OUT / ('%016x.desc' % name)).write_bytes(patched)
        print('%016x.desc  %d bytes (%dx%d, %d mips -> %dx%d, %d mips)' % (
            (name, len(patched)) + struct.unpack_from('<III', cpu, 0xc4) + (width, height, mips)))


if __name__ == '__main__':
    main()
