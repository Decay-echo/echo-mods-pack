"""The ARCADE and SETTINGS tab icons, built from the game's own tab art at build time.

The stock tabs are framed squares (grey = idle, teal = selected) with a white glyph.
We crop both List-tab frames from the user's atlas, clear its glyph and draw our own
(gamepad for ARCADE, gear for SETTINGS) in the same white style. Nothing from the
game ships in this repo.
"""
from PIL import Image, ImageDraw, ImageFilter

TEX_W, TEX_H = 256, 128  # per icon row: idle frame left, selected frame right


def _crop(atlas, uv):
    u0, v0, u1, v1 = uv
    return atlas.crop((round(u0 * atlas.width), round(v0 * atlas.height),
                       round(u1 * atlas.width), round(v1 * atlas.height))).convert('RGBA')


def _clear_glyph(img):
    """Remove the List icon's glyph (horizontal bars) from inside the frame.

    The frame interior is one colour with alpha in bands, so each glyph pixel is
    refilled from the nearest clean pixel in its column: the gaps between the bars.
    """
    w, h = img.size
    px = img.load()
    mx, my = int(w * .14), int(h * .14)  # stay clear of the white/grey frame
    mask = Image.new('L', img.size, 0)
    m = mask.load()
    sums = sorted(sum(px[x, y][:3]) for y in range(my, h - my) for x in range(mx, w - mx))
    typical = sums[len(sums) // 3]  # the interior colour (glyph pixels are the bright minority)
    for y in range(my, h - my):
        for x in range(mx, w - mx):
            r, g, b, _ = px[x, y]
            if min(r, g, b) > 120 or r + g + b < typical * .85:  # glyph, or its drop shadow
                m[x, y] = 255
    m = mask.filter(ImageFilter.MaxFilter(5)).load()  # plus the anti-aliased edge
    for x in range(mx, w - mx):
        clean = [y for y in range(my, h - my) if not m[x, y]]
        if not clean:
            continue
        for y in range(my, h - my):
            if m[x, y]:
                px[x, y] = px[x, min(clean, key=lambda c: abs(c - y))]
    return img


def _gamepad(size, stroke):
    """White gamepad glyph (outline body, filled d-pad and buttons), 4x supersampled."""
    s = 4
    w, h = size
    layer = Image.new('RGBA', (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    white = (255, 255, 255, 255)
    X = lambda f: f * w * s
    Y = lambda f: f * h * s
    t = stroke * s
    # body with two grips
    d.rounded_rectangle((X(.22), Y(.36), X(.78), Y(.62)), radius=Y(.12), outline=white, width=t)
    d.ellipse((X(.20), Y(.44), X(.40), Y(.72)), outline=white, width=t)
    d.ellipse((X(.60), Y(.44), X(.80), Y(.72)), outline=white, width=t)
    d.rectangle((X(.26), Y(.39), X(.74), Y(.58)), fill=(0, 0, 0, 0))  # hollow the overlaps
    d.rounded_rectangle((X(.22), Y(.36), X(.78), Y(.62)), radius=Y(.12), outline=white, width=t)
    # d-pad
    cx, cy, arm, th = X(.35), Y(.49), X(.075), X(.03)
    d.rectangle((cx - arm, cy - th, cx + arm, cy + th), fill=white)
    d.rectangle((cx - th, cy - arm, cx + th, cy + arm), fill=white)
    # face buttons
    for fx, fy in ((.62, .45), (.69, .53)):
        r = X(.04)
        d.ellipse((X(fx) - r, Y(fy) - r, X(fx) + r, Y(fy) + r), fill=white)
    return layer.resize(size, Image.LANCZOS)


def _gear(size, stroke):
    """White gear glyph (8 teeth, hollow hub), 4x supersampled."""
    import math
    s = 4
    w, h = size
    layer = Image.new('RGBA', (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    white = (255, 255, 255, 255)
    cx, cy = w * s / 2, h * s / 2
    r_out, r_body, r_hole = w * s * .27, w * s * .20, w * s * .085
    for k in range(8):  # teeth
        a = k * math.pi / 4
        tw = .22
        pts = [(cx + r * math.cos(a + da), cy + r * math.sin(a + da))
               for r, da in ((r_body * .9, -tw * 1.3), (r_out, -tw * .75), (r_out, tw * .75), (r_body * .9, tw * 1.3))]
        d.polygon(pts, fill=white)
    d.ellipse((cx - r_body, cy - r_body, cx + r_body, cy + r_body), fill=white)
    d.ellipse((cx - r_hole, cy - r_hole, cx + r_hole, cy + r_hole), fill=(0, 0, 0, 0))
    return layer.resize(size, Image.LANCZOS)


GLYPHS = {'arcade': _gamepad, 'settings': _gear}


def tab_icons(atlas, idle_uv, selected_uv, names=('arcade', 'settings')):
    """-> (w, h, BGRA bytes, {name: (idle uv, selected uv)}) for one icon texture.

    Frames sit in 128x128 cells: one row per icon, idle left and selected right."""
    tex = Image.new('RGBA', (TEX_W, TEX_H * len(names)), (0, 0, 0, 0))
    W, H = tex.size
    uvs = {}
    for row, name in enumerate(names):
        pair = []
        for col, uv in enumerate((idle_uv, selected_uv)):
            frame = _clear_glyph(_crop(atlas, uv))
            w, h = frame.size
            assert w <= TEX_W // 2 and h <= TEX_H, frame.size
            frame.alpha_composite(GLYPHS[name]((w, h), max(2, round(w * .07))))
            x0, y0 = col * TEX_W // 2, row * TEX_H
            tex.paste(frame, (x0, y0))
            pair.append((x0 / W, y0 / H, (x0 + w) / W, (y0 + h) / H))
        uvs[name] = tuple(pair)
    r, g, b, a = tex.split()
    return W, H, Image.merge('RGBA', (b, g, r, a)).tobytes(), uvs


if __name__ == '__main__':  # preview: python tools/tab_icon.py out.png (needs the game installed)
    import os, struct, sys
    sys.argv.append('tab_icon_preview.png')
    from build_arcade_tab import atlas_image, P, STOCK_NAV_CANVAS
    from echovr_patch import Patcher
    from canvas_edit import Canvas
    stock = Patcher().stock
    nav = Canvas(stock.get(P.typesym('CUICanvasResource'), STOCK_NAV_CANVAS))
    w, h, px, _ = tab_icons(atlas_image(stock), struct.unpack_from('<4f', nav.elements[3], 0x90),
                            struct.unpack_from('<4f', nav.elements[4], 0x90))
    b, g, r, a = Image.frombytes('RGBA', (w, h), px).split()
    img = Image.new('RGBA', (w, h), (38, 40, 58, 255))
    img.alpha_composite(Image.merge('RGBA', (r, g, b, a)))
    img.resize((w * 3, h * 3), Image.NEAREST).save(sys.argv[1])
    print('wrote', os.path.abspath(sys.argv[1]))
