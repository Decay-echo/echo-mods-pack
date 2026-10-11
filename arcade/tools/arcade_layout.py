"""Geometry shared by the tab builder and tests (canvas units: 1024 px = 0.3 m)."""
import struct

PAGE_X, PAGE_Y = 28, 112          # page child position inside the root canvas
PAGE_W, PAGE_H = 942, 528         # same page area the stock pages use
SCREEN_RECT = (0, 0, PAGE_W, PAGE_H)
TEX_W, TEX_H = 1024, 574          # page aspect; this size is unique among stock textures
GRID_COLS, GRID_ROWS = 32, 18     # invisible poke cells over the screen (~8.6 mm each)
# Navigation bar: ARCADE, the 4 stock tabs, SETTINGS as 6 evenly spaced icons centred
# on the bar (between its corner marks at x~61 and x~929). Stock spacing is 204.8,
# which only fits 4.
TAB_SIZE = 89                                   # stock icon size
TAB_SPACING = 139
TAB_SLOT_CENTERS = tuple(512 + (i - 2.5) * TAB_SPACING for i in range(6))
ARCADE_SLOT, STOCK_SLOTS, SETTINGS_SLOT = 0, (1, 2, 3, 4), 5
TAB_Y = (662, 751)
# Lobby poster screen (DOCK): every dynamic poster maps its face to texture u 0.016..0.876,
# v 0.032..0.97 (the shader doubles the mesh's v). The frame sits in that face at 16:9 on a
# ~1.87:1 face, so it keeps its shape on both the flat and the curved posters. Unique size.
POSTER_TEX_W, POSTER_TEX_H = 1249, 612
POSTER_X0, POSTER_Y0 = 45, 20              # where the 1024 x 574 frame goes in that texture


def tab_rect(slot):
    c = TAB_SLOT_CENTERS[slot]
    return (c - TAB_SIZE / 2, TAB_Y[0], c + TAB_SIZE / 2, TAB_Y[1])

_FONT = {  # 5x7 glyphs for the baked splash only; the host renders real text.
    'A': '01110 10001 10001 11111 10001 10001 10001', 'R': '11110 10001 10001 11110 10100 10010 10001',
    'C': '01111 10000 10000 10000 10000 10000 01111', 'D': '11110 10001 10001 10001 10001 10001 11110',
    'E': '11111 10000 10000 11110 10000 10000 11111', 'O': '01110 10001 10001 10001 10001 10001 01110',
    'F': '11111 10000 10000 11110 10000 10000 10000', 'L': '10000 10000 10000 10000 10000 10000 11111',
    'I': '11111 00100 00100 00100 00100 00100 11111', 'N': '10001 11001 10101 10011 10001 10001 10001',
    'S': '01111 10000 10000 01110 00001 00001 11110', 'T': '11111 00100 00100 00100 00100 00100 00100',
    'H': '10001 10001 10001 11111 10001 10001 10001', 'G': '01111 10000 10000 10011 10001 10001 01111',
    '.': '00000 00000 00000 00000 00000 01100 01100', ' ': '00000 00000 00000 00000 00000 00000 00000',
}


def splash_bgra(text=('ECHO ARCADE', 'STARTING...')):
    """Dark gradient with blocky text: what the tab shows before the host's first frame."""
    px = bytearray(TEX_W * TEX_H * 4)
    for y in range(TEX_H):
        shade = 18 + y * 20 // TEX_H
        row = bytes((shade + 22, shade, shade // 2 + 6, 255)) * TEX_W
        px[y * TEX_W * 4:(y + 1) * TEX_W * 4] = row

    def draw(line, scale, y0, color):
        width = len(line) * 6 * scale
        x0 = (TEX_W - width) // 2
        for i, ch in enumerate(line):
            rows = _FONT.get(ch, _FONT[' ']).split()
            for gy, bits in enumerate(rows):
                for gx, bit in enumerate(bits):
                    if bit != '1':
                        continue
                    for yy in range(scale):
                        y = y0 + gy * scale + yy
                        start = ((y * TEX_W) + x0 + (i * 6 + gx) * scale) * 4
                        px[start:start + scale * 4] = bytes(color) * scale

    draw(text[0], 10, 190, (80, 200, 255, 255))
    draw(text[1], 5, 320, (210, 210, 210, 255))
    return bytes(px)


if __name__ == '__main__':
    data = splash_bgra()
    print(len(data), struct.unpack_from('<4B', data, 0))
