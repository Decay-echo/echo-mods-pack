"""Build the ARCADE tablet tab from the installed game's stock resources.

Offline: reads the game's manifest/packages, writes build/tab/{manifests,packages}
plus native/generated/arcade_tab.h. Never modifies the game folder (see install.py).

The page is a background panel, one streamed screen sprite (a BGRA texture the
runtime overwrites every frame on the GPU) and an offline label. Touch input is a
GRID_COLS x GRID_ROWS grid of invisible poke buttons laid over the screen.

DOCK adds a second streamed texture in the lobby posters' layout. A hidden sprite on
the page keeps it loaded for the whole session, so a poster only ever borrows it.
Also writes native/generated/posters.h (tools/build_poster_table.py).
"""
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PROBE = ROOT / 'vendor/Doom-on-EchoVR/EchoVr-Tablet-Probing'
os.environ.setdefault('ECHOVR_GAME', str(ROOT.parent / 'ready-at-dawn-echo-arena'))
sys.path.insert(0, str(PROBE))
sys.path.insert(0, str(HERE))

import echovr_pkg as P  # noqa: E402
from canvas_edit import Canvas, descriptor, u64  # noqa: E402
from echovr_patch import Patcher, ManifestFile, MID, GAME_MANIFEST, GAME_PACKAGES, verify  # noqa: E402
from build_tools_tab import empty_canvas, ROOT_CANVAS, NAV_CANVAS, LEVEL, ACTOR  # noqa: E402
from arcade_layout import (PAGE_W, PAGE_H, PAGE_X, PAGE_Y, SCREEN_RECT, TEX_W, TEX_H,  # noqa: E402
                           GRID_COLS, GRID_ROWS, TAB_SLOT_CENTERS, ARCADE_SLOT, STOCK_SLOTS,
                           SETTINGS_SLOT, POSTER_TEX_W, POSTER_TEX_H, POSTER_X0, POSTER_Y0,
                           tab_rect, splash_bgra)
import build_poster_table  # noqa: E402
from tab_icon import tab_icons  # noqa: E402

OUT = ROOT / 'build/tab'
GENERATED = ROOT / 'native/generated'
TITLE_DONOR = 0xfdea8aeb3a0f4862
NAV_SOURCE_BUTTON = 0x275876572b742791
PAGE = P.sym('echo_arcade_page_v1')
TEXTURE = P.sym('echo_arcade_screen_v1')
POSTER_TEXTURE = P.sym('echo_arcade_poster_screen_v1')
ICON_TEXTURE = P.sym('echo_arcade_tab_icon_texture_v1')
TAB_BUTTON = P.sym('echo_arcade_tab_button_v1')
SETTINGS_BUTTON = P.sym('echo_arcade_settings_button_v1')
STOCK_NAV_CANVAS = NAV_CANVAS
BGRA_TEMPLATE = 0x84b9a06506af66fe  # stock 128x128 B8G8R8A8_UNORM texture
NAV_ATLAS = 0x79563b60c7895d4e      # stock UI atlas (BC7_UNORM_SRGB) the tab icons come from
DXGI_B8G8R8A8_UNORM = 87
# The tablet's own UI textures are all sRGB (BC7_UNORM_SRGB). A linear UNORM screen
# made the game gamma-correct our already-sRGB pixels a second time: washed out.
DXGI_B8G8R8A8_UNORM_SRGB = 91


def cell_symbol(c, r):
    return P.sym(f'echo_arcade_cell_{c}_{r}_v1')


def texture_pair(stock, w, h, pixels, fmt=DXGI_B8G8R8A8_UNORM_SRGB):
    """Clone the stock BGRA texture header pair as a w x h texture of `fmt` holding `pixels`."""
    tt, tg = P.typesym('CGTextureResource'), P.typesym('CGTextureResource', True)
    cpu = bytearray(stock.get(tt, BGRA_TEMPLATE))
    gpu = stock.get(tg, BGRA_TEMPLATE)
    if len(cpu) != 256 or struct.unpack_from('<I', cpu, 0xd8)[0] != DXGI_B8G8R8A8_UNORM:
        raise ValueError('Unexpected BGRA template texture')
    pitch, size = w * 4, w * h * 4
    dds = bytearray(gpu[:148])
    if dds[:4] != b'DDS ' or dds[84:88] != b'DX10' or struct.unpack_from('<I', dds, 128)[0] != DXGI_B8G8R8A8_UNORM:
        raise ValueError('Unexpected BGRA template DDS header')
    struct.pack_into('<III', dds, 12, h, w, pitch)
    struct.pack_into('<I', dds, 128, fmt)
    struct.pack_into('<II', cpu, 0xc4, w, h)
    struct.pack_into('<I', cpu, 0xd8, fmt)
    struct.pack_into('<II', cpu, 0xe8, w, h)
    struct.pack_into('<II', cpu, 0xf4, len(dds) + size, pitch)
    assert len(pixels) == size
    return bytes(cpu), bytes(dds) + pixels


def atlas_image(stock):
    """Decode the stock UI atlas (top mip) to a PIL RGBA image."""
    import texture2ddecoder
    from PIL import Image
    tt, tg = P.typesym('CGTextureResource'), P.typesym('CGTextureResource', True)
    cpu, gpu = stock.get(tt, NAV_ATLAS), stock.get(tg, NAV_ATLAS)
    w, h = struct.unpack_from('<II', cpu, 0xc4)
    if struct.unpack_from('<I', gpu, 128)[0] != 99:
        raise SystemExit('Unexpected tablet atlas format (game update?)')
    bgra = texture2ddecoder.decode_bc7(gpu[148:148 + (w // 4) * (h // 4) * 16], w, h)
    return Image.frombytes('RGBA', (w, h), bgra, 'raw', 'BGRA')


def poster_bgra():
    """Poster texture: dark, with the splash where the frame streams (arcade_layout.py)."""
    px = bytearray(bytes((30, 22, 20, 255)) * (POSTER_TEX_W * POSTER_TEX_H))
    splash = splash_bgra(('ECHO ARCADE', 'DOCKED'))
    for y in range(TEX_H):
        start = ((POSTER_Y0 + y) * POSTER_TEX_W + POSTER_X0) * 4
        px[start:start + TEX_W * 4] = splash[y * TEX_W * 4:(y + 1) * TEX_W * 4]
    return bytes(px)


def rect_of(row):
    return list(struct.unpack_from('<4f', row, 0x34))


def respace_stock_tabs(nav):
    """Move the 4 stock tab icons (idle + selected sprite pairs) into their new slots.

    Returns [(old_center, new_center, idle_index, selected_index)] left to right."""
    pairs = [(1, 2), (3, 4), (5, 6), (7, 8)]
    moves = []
    for k, (idle, sel) in enumerate(pairs):
        a, b = rect_of(nav.elements[idle]), rect_of(nav.elements[sel])
        old = (a[0] + a[2]) / 2
        uv_idle, uv_sel = struct.unpack_from('<4f', nav.elements[idle], 0x90), struct.unpack_from('<4f', nav.elements[sel], 0x90)
        assert abs(old - 204.8 * (k + 1)) < 2 and abs((b[0] + b[2]) / 2 - old) < 2, ('Unexpected stock tab layout', k, a, b)
        assert uv_sel[1] < uv_idle[1], 'Unexpected stock tab sprite order'
        new = TAB_SLOT_CENTERS[STOCK_SLOTS[k]]
        for i in (idle, sel):
            r = rect_of(nav.elements[i])
            struct.pack_into('<4f', nav.elements[i], 0x34, r[0] + new - old, r[1], r[2] + new - old, r[3])
        moves.append((old, new, idle, sel))
    return moves


def fix_texture_count(canvas):
    """Header +0x28 = elements that reference a texture (sprite 3, mask 4, color mask 8).

    Canvas load pre-sizes its "waiting for texture" list from this count; a sprite
    on a canvas that says 0 makes the engine push into an empty list and crash
    (echovr.exe+0x719ccb). Every stock canvas (277/277) follows this rule.
    """
    count = sum(struct.unpack_from('<I', e, 8)[0] in (3, 4, 8) for e in canvas.elements)
    struct.pack_into('<I', canvas.header, 0x28, count)
    return count


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    patcher = Patcher()
    stock = patcher.stock
    base_manifest = GAME_MANIFEST.read_bytes()
    before = ManifestFile(base_manifest)
    cv = P.typesym('CUICanvasResource')
    tt = P.typesym('CGTextureResource')
    if stock.get(cv, PAGE) is not None or (tt, TEXTURE) in before.index:
        raise SystemExit('ARCADE is already installed in this manifest; run install.py restore first.')
    for n, _, _ in stock.by_type(tt):
        d = stock.get(tt, n)
        for w, h in ((TEX_W, TEX_H), (POSTER_TEX_W, POSTER_TEX_H)):
            if len(d) >= 0xd0 and struct.unpack_from('<II', d, 0xc4) == (w, h):
                raise SystemExit(f'Screen size {w}x{h} is not unique ({n:016x}); the D3D12 streamer relies on it')

    root = Canvas(stock.get(cv, ROOT_CANVAS))
    nav = Canvas(stock.get(cv, NAV_CANVAS))
    donor = Canvas(stock.get(cv, TITLE_DONOR))
    if len(root.elements) != 7 or len(nav.elements) != 9:
        raise SystemExit('Unexpected stock tablet layout (another tablet mod installed?). Restore it first.')
    old_root = [bytes(r) for r in root.elements]
    old_nav = [bytes(r) for r in nav.elements]
    sprite_template = nav.elements[0]
    assert struct.unpack_from('<I', sprite_template, 8)[0] == 3

    # Navigation: 6 centred icon tabs. Our icon pairs (idle + selected) are drawn on the
    # stock List icon's frames, so they match the other tabs exactly.
    moves = respace_stock_tabs(nav)
    list_idle, list_sel = nav.elements[3], nav.elements[4]
    icon_w, icon_h, icon_pixels, icon_uvs = tab_icons(
        atlas_image(stock), struct.unpack_from('<4f', list_idle, 0x90), struct.unpack_from('<4f', list_sel, 0x90))

    def tab_sprite(template, name, slot, uv, hidden):
        index = nav.append(template, name, tab_rect(slot), parent=struct.unpack_from('<i', template, 0x5c)[0], hidden=hidden)
        row = nav.elements[index]
        row[0x24:0x34] = template[0x24:0x34]  # keep the stock sprite's layout fields
        struct.pack_into('<4f', row, 0x78, 1, 1, 1, 1)
        struct.pack_into('<Q', row, 0x88, ICON_TEXTURE)
        struct.pack_into('<4f', row, 0x90, *uv)
        return index

    nav_icon = tab_sprite(list_idle, 'echo_arcade_tab_icon_v1', ARCADE_SLOT, icon_uvs['arcade'][0], False)
    nav_selected = tab_sprite(list_sel, 'echo_arcade_tab_selected_v1', ARCADE_SLOT, icon_uvs['arcade'][1], True)
    tab_sprite(list_idle, 'echo_arcade_settings_icon_v1', SETTINGS_SLOT, icon_uvs['settings'][0], False)
    nav_settings_selected = tab_sprite(list_sel, 'echo_arcade_settings_selected_v1', SETTINGS_SLOT, icon_uvs['settings'][1], True)

    # Page canvas.
    page = empty_canvas(stock.get(cv, TITLE_DONOR), PAGE_W, PAGE_H)
    struct.pack_into('<Q', page.header, 0, PAGE)
    bg = page.append(donor.elements[0], 'echo_arcade_background_v1', (0, 0, PAGE_W, PAGE_H))
    struct.pack_into('<4f', page.elements[bg], 0x78, .02, .02, .03, 1)
    screen = page.append(sprite_template, 'echo_arcade_screen_sprite_v1', SCREEN_RECT)
    row = page.elements[screen]
    struct.pack_into('<4f', row, 0x78, 1, 1, 1, 1)
    struct.pack_into('<Q', row, 0x88, TEXTURE)
    struct.pack_into('<4f', row, 0x90, 0, 0, 1, 1)
    status = page.label(donor.elements[1], 'echo_arcade_status_v1', 'STARTING ARCADE...',
                        (0, PAGE_H - 34, PAGE_W, PAGE_H), 18, hidden=True, capacity=96)
    # Never drawn: holds the poster texture for as long as the tablet exists (DOCK).
    keepalive = page.append(sprite_template, 'echo_arcade_poster_keepalive_v1', (0, 0, 1, 1), hidden=True)
    row = page.elements[keepalive]
    struct.pack_into('<4f', row, 0x78, 1, 1, 1, 1)
    struct.pack_into('<Q', row, 0x88, POSTER_TEXTURE)
    struct.pack_into('<4f', row, 0x90, 0, 0, 1, 1)
    # Local vertex/index budgets for RenderMT (same sizing rule as stock pages).
    struct.pack_into('<2I', page.header, 0x2c, 256, 384)

    # Root: page child + header strip + title, all hidden until ARCADE is selected.
    child = root.append(root.elements[1], 'echo_arcade_page_child_v1', (PAGE_X, PAGE_Y, PAGE_X + PAGE_W, PAGE_Y + PAGE_H), hidden=True)
    struct.pack_into('<Q', root.elements[child], 0x78, PAGE)
    header = root.append(donor.elements[0], 'echo_arcade_header_background_v1', (54, 22, 870, 112), hidden=True)
    struct.pack_into('<4f', root.elements[header], 0x78, .02, .02, .03, 1)
    title = root.label(donor.elements[1], 'echo_arcade_title_v1', 'ARCADE', (130, 22, 822, 112), 40, hidden=True, capacity=64)
    struct.pack_into('<2I', root.header, 0x2c, 264, 396)
    struct.pack_into('<2I', nav.header, 0x2c, 548, 822)
    for c in (root, nav, page):
        fix_texture_count(c)
        c.validate()
    assert struct.unpack_from('<I', page.header, 0x28)[0] == 2 and struct.unpack_from('<I', nav.header, 0x28)[0] == 13
    assert [bytes(r) for r in root.elements[:7]] == old_root
    for i, (old, new) in enumerate(zip(old_nav, nav.elements[:9])):  # stock tabs: only their x position moved
        assert old[:0x34] == new[:0x34] and old[0x44:] == new[0x44:], f'nav element {i} changed beyond its rect'

    # Buttons: tab + touch grid, cloned from a stock navigation button.
    bt = P.typesym('CR15ButtonInteractCR')
    original = stock.get(bt, LEVEL)
    stride, rows = P.parse_cr(original)
    if stride != 296:
        raise SystemExit('Unexpected button record size')
    source = next(r for r in rows if u64(r, 8) == ACTOR and u64(r, 0) == NAV_SOURCE_BUTTON)
    s = .3 / 1024

    def add_button(name_sym, rect):
        row = bytearray(source)
        struct.pack_into('<Q', row, 0, name_sym)
        x0, y0, x1, y1 = rect
        struct.pack_into('<2f', row, 0x80, (x0 + x1) / 2 * s, -(y0 + y1) / 2 * s)
        struct.pack_into('<2f', row, 0x9c, (x1 - x0) / 2 * s, (y1 - y0) / 2 * s)
        rows.append(bytes(row))

    # Stock tab hitboxes follow their icons into the new slots.
    moved = set()
    for i, r in enumerate(rows):
        if u64(r, 8) != ACTOR or not 0x275876572b742791 <= u64(r, 0) <= 0x275876572b742794:
            continue
        x, y = struct.unpack_from('<2f', r, 0x80)
        old, new, _, _ = min(moves, key=lambda m: abs(m[0] - x / s))
        assert abs(old - x / s) < 3, ('Stock tab hitbox not under a stock icon', x / s)
        row = bytearray(r)
        struct.pack_into('<f', row, 0x80, new * s)
        rows[i] = bytes(row)
        moved.add(new)
    assert len(moved) == 4, 'Expected 4 stock tab hitboxes'
    changed_rows = {i for i, r in enumerate(rows) if 0x275876572b742791 <= u64(r, 0) <= 0x275876572b742794 and u64(r, 8) == ACTOR}
    add_button(TAB_BUTTON, tab_rect(ARCADE_SLOT))
    add_button(SETTINGS_BUTTON, tab_rect(SETTINGS_SLOT))
    sx0, sy0, sx1, sy1 = SCREEN_RECT
    cw, ch = (sx1 - sx0) / GRID_COLS, (sy1 - sy0) / GRID_ROWS
    for r in range(GRID_ROWS):
        for c in range(GRID_COLS):
            add_button(cell_symbol(c, r), (PAGE_X + sx0 + c * cw, PAGE_Y + sy0 + r * ch,
                                           PAGE_X + sx0 + (c + 1) * cw, PAGE_Y + sy0 + (r + 1) * ch))
    hdr = bytearray(original[:56])
    descriptor(hdr, 0, len(rows), stride)
    buttons = bytes(hdr) + b''.join(rows)
    _, original_rows = P.parse_cr(original)
    for i, r in enumerate(original_rows):
        assert i in changed_rows or rows[i] == r, 'Unrelated button record changed'

    tex_cpu, tex_gpu = texture_pair(stock, TEX_W, TEX_H, splash_bgra())
    icon_cpu, icon_gpu = texture_pair(stock, icon_w, icon_h, icon_pixels)
    poster_cpu, poster_gpu = texture_pair(stock, POSTER_TEX_W, POSTER_TEX_H, poster_bgra())
    patcher.add_item('CUICanvasResource', False, f'0x{ROOT_CANVAS:016x}', root.serialize())
    patcher.add_item('CUICanvasResource', False, f'0x{NAV_CANVAS:016x}', nav.serialize())
    patcher.add_item('CUICanvasResource', False, f'0x{PAGE:016x}', page.serialize())
    patcher.add_item('CR15ButtonInteractCR', False, f'0x{LEVEL:016x}', buttons)
    patcher.add_item('CGTextureResource', False, f'0x{TEXTURE:016x}', tex_cpu)
    patcher.add_item('CGTextureResource', True, f'0x{TEXTURE:016x}', tex_gpu)
    patcher.add_item('CGTextureResource', False, f'0x{ICON_TEXTURE:016x}', icon_cpu)
    patcher.add_item('CGTextureResource', True, f'0x{ICON_TEXTURE:016x}', icon_gpu)
    patcher.add_item('CGTextureResource', False, f'0x{POSTER_TEXTURE:016x}', poster_cpu)
    patcher.add_item('CGTextureResource', True, f'0x{POSTER_TEXTURE:016x}', poster_gpu)
    while (GAME_PACKAGES / f'{MID}_{patcher.mf.npkg}').exists():
        i = patcher.mf.npkg
        patcher.mf.C.append([i, (GAME_PACKAGES / f'{MID}_{i}').stat().st_size, 0, 0])
        patcher.mf.npkg += 1
    package_index = patcher.mf.npkg
    patcher.build(OUT)

    after = ManifestFile((OUT / 'manifests' / MID).read_bytes())
    overrides = {(t, n) for t, n, _ in patcher.items}
    after_rows = {(a[0], a[1]): (a, b) for a, b in zip(after.A, after.B)}
    for a, b in zip(before.A, before.B):
        if (a[0], a[1]) not in overrides:
            assert after_rows[(a[0], a[1])] == (a, b), 'Unrelated resource changed'
    assert after.C[:len(before.C)] == before.C, 'Existing frame references changed'
    if not verify(OUT):
        raise SystemExit('Package verification failed')

    sha = lambda b: hashlib.sha256(b).hexdigest()
    package_name = f'{MID}_{package_index}'
    meta = dict(schema=1, base_manifest_sha256=sha(base_manifest),
                manifest_sha256=sha((OUT / 'manifests' / MID).read_bytes()),
                package=package_name, package_sha256=sha((OUT / 'packages' / package_name).read_bytes()),
                page=f'{PAGE:016x}', texture=f'{TEXTURE:016x}', tab_button=f'{TAB_BUTTON:016x}', settings_button=f'{SETTINGS_BUTTON:016x}',
                grid=[GRID_COLS, GRID_ROWS], texture_size=[TEX_W, TEX_H],
                poster_texture=f'{POSTER_TEXTURE:016x}', poster_texture_size=[POSTER_TEX_W, POSTER_TEX_H])
    (OUT / 'arcade_tab.json').write_text(json.dumps(meta, indent=2))

    GENERATED.mkdir(parents=True, exist_ok=True)
    cells = ','.join(f'0x{cell_symbol(c, r):016x}' for r in range(GRID_ROWS) for c in range(GRID_COLS))
    (GENERATED / 'arcade_tab.h').write_text(
        '// Generated by tools/build_arcade_tab.py -- do not edit.\n#pragma once\n#include <cstdint>\n'
        f'constexpr uint64_t ARCADE_TAB=0x{TAB_BUTTON:016x}, SETTINGS_TAB=0x{SETTINGS_BUTTON:016x}, ARCADE_PAGE_MARKER=0x{P.sym("echo_arcade_background_v1"):016x};\n'
        f'constexpr uint64_t ARCADE_ROOT_MARKER=0x{P.sym("echo_arcade_page_child_v1"):016x}, ARCADE_NAV_MARKER=0x{P.sym("echo_arcade_tab_icon_v1"):016x};\n'
        f'constexpr unsigned ARCADE_PAGE_ELEMENTS={len(page.elements)}, ARCADE_ROOT_ELEMENTS={len(root.elements)}, ARCADE_NAV_ELEMENTS={len(nav.elements)};\n'
        f'constexpr unsigned ARCADE_ROOT_CHILD={child}, ARCADE_ROOT_HEADER={header}, ARCADE_ROOT_TITLE={title};\n'
        f'constexpr unsigned ARCADE_NAV_ICON={nav_icon}, ARCADE_NAV_SELECTED={nav_selected}, SETTINGS_NAV_SELECTED={nav_settings_selected};\n'
        f'constexpr unsigned ARCADE_NAV_STOCK_SELECTED[]={{{",".join(str(m[3]) for m in moves)}}};\n'
        f'constexpr unsigned ARCADE_PAGE_SCREEN={screen}, ARCADE_PAGE_STATUS={status};\n'
        f'constexpr unsigned ARCADE_GRID_COLS={GRID_COLS}, ARCADE_GRID_ROWS={GRID_ROWS};\n'
        f'constexpr unsigned ARCADE_TEX_W={TEX_W}, ARCADE_TEX_H={TEX_H};\n'
        f'constexpr uint64_t ARCADE_POSTER_TEXTURE=0x{POSTER_TEXTURE:016x};\n'
        f'constexpr uint64_t ARCADE_CELLS[]={{{cells}}};\n')
    print(f'Built ARCADE tab: {len(patcher.items)} resources in {package_name}, '
          f'{GRID_COLS}x{GRID_ROWS} touch grid, {TEX_W}x{TEX_H} BGRA screen, {POSTER_TEX_W}x{POSTER_TEX_H} poster screen.')
    build_poster_table.build()
    return meta


if __name__ == '__main__':
    build()
