"""Extracts the digits of Echo VR's helmet speed font for EchoHudPlus.

The speed readout uses CTTFontResource dbcd462f8d9f0512: a 512x512 multi-channel
signed distance field atlas (texture dfaaf3ca83578e14) plus a glyph table. This copies
the glyphs for 0-9 and '.' from your own game files into data/echo_digits.bin, which
install_hudplus.py installs next to the plugin so the last-speed number is drawn in
the same font as Echo's speed. Without the file the plugin falls back to its own digits.

Reads the game's packages, so it needs zstandard and echovr_pkg.py. Run it with
EchoArcade's environment:
    <echo-arcade>\\.venv\\Scripts\\python.exe make_digits.py <echo-arcade>\\vendor\\Doom-on-EchoVR\\EchoVr-Tablet-Probing
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
import echovr_pkg as P  # noqa: E402

FONT = 0xDBCD462F8D9F0512
ATLAS = 0xDFAAF3CA83578E14
CHARS = '0123456789.'
PX_RANGE = 4.0   # distance range of the field, atlas px
EM = 32.0        # font size the atlas was made at
OUT = Path(__file__).resolve().parent / 'data' / 'echo_digits.bin'


def atlas_pixels(manifests):
    """BGRA8 pixels of the 512x512 atlas (embedded DDS, format 87)."""
    tt = P.typesym('CGTextureResource')
    for gpu in (False, True):
        t = P.typesym('CGTextureResource', gpu)
        for m in manifests:
            b = m.get(t, ATLAS)
            if b is None:
                continue
            i = b.find(b'DDS ')
            if i < 0:
                continue
            dds = b[i:]
            h, w = struct.unpack_from('<II', dds, 12)
            fmt = struct.unpack_from('<I', dds, 128)[0]
            if (w, h) != (512, 512) or fmt != 87:
                raise SystemExit('unexpected atlas %dx%d format %d (game update?)' % (w, h, fmt))
            return dds[148:148 + w * h * 4]
    raise SystemExit('font atlas %016x not found' % ATLAS)


def glyph_table(manifests):
    t = P.typesym('CTTFontResource')
    b = next((x for m in manifests if (x := m.get(t, FONT)) is not None), None)
    if b is None:
        raise SystemExit('font %016x not found' % FONT)
    glyphs, at = {}, 0x220
    while at + 32 <= len(b):
        c = struct.unpack_from('<I', b, at)[0]
        if c < 32 or c > 0xFFFF:
            break
        x, y, w, h = struct.unpack_from('<4H', b, at + 4)
        xo, yo = struct.unpack_from('<2h', b, at + 16)
        adv = struct.unpack_from('<I', b, at + 20)[0]
        glyphs[chr(c)] = (x, y, w, h, xo, yo, adv)
        at += 32
    return glyphs


def main():
    manifests = P.open_all()
    pixels = atlas_pixels(manifests)
    glyphs = glyph_table(manifests)
    out = bytearray(b'EDG1' + struct.pack('<Iff', len(CHARS), PX_RANGE, EM))
    for ch in CHARS:
        if ch not in glyphs:
            raise SystemExit('glyph %r missing (game update?)' % ch)
        x, y, w, h, xo, yo, adv = glyphs[ch]
        out += struct.pack('<IHHhhf', ord(ch), w, h, xo, yo, float(adv))
        for row in range(y, y + h):
            for col in range(x, x + w):
                b, g, r, _ = pixels[(row * 512 + col) * 4:(row * 512 + col) * 4 + 4]
                out += bytes((r, g, b))
    OUT.parent.mkdir(exist_ok=True)
    OUT.write_bytes(bytes(out))
    print('%s: %d glyphs, %d bytes' % (OUT, len(CHARS), len(out)))


if __name__ == '__main__':
    main()
