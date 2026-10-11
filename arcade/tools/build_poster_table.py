"""Lobby posters for DOCK, touch and light gun: writes native/generated/posters.h.

Read-only. For every level with dynamic posters (R15NetDynamicPoster) it records each
poster actor's transform, and for each poster mesh the triangles of its picture face
(the part whose UVs cover the poster art), in the mesh's own space. The runtime moves
fingers and bullet rays into a poster's space and hits those triangles.

    python tools/build_poster_table.py
"""
import os
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
os.environ.setdefault('ECHOVR_GAME', str(ROOT.parent / 'ready-at-dawn-echo-arena'))
sys.path.insert(0, str(ROOT / 'vendor/Doom-on-EchoVR/EchoVr-Tablet-Probing'))
sys.path.insert(0, str(HERE))

import echovr_pkg as P  # noqa: E402
from arcade_layout import POSTER_TEX_W, POSTER_TEX_H, POSTER_X0, POSTER_Y0, TEX_W, TEX_H  # noqa: E402

OUT = ROOT / 'native/generated/posters.h'
MID = '48037dc70b0ecab2'
POSTER_MODEL = 0x80011e8d2e3d410e      # the instance-model component every poster overrides
FACE_U = (0.0155, 0.8775)              # mesh UVs of the picture face (all poster meshes)
FACE_V = (0.0155, 0.4865)
TRANSFORM_STRIDE = 0xb0
NO_MESH = 0xffffffff                   # PosterInfo.mesh of a poster without a face map


def records(blob, stride=None):
    n = struct.unpack_from('<Q', blob, 0x28)[0]
    stride = stride or (len(blob) - 0x38) // n
    assert 0x38 + n * stride == len(blob), 'unexpected component record layout'
    return [blob[0x38 + i * stride:0x38 + (i + 1) * stride] for i in range(n)]


def mesh_of(manifest, instances, actor):
    """CInstanceModelCR per-instance entry (0x48 bytes: component, actor, ..., +0x20 model
    resource). The same (component, actor) pair also appears in the record headers."""
    key = struct.pack('<QQ', POSTER_MODEL, actor)
    model = P.typesym('CGInstancedModelResource')
    o = instances.find(key)
    while o >= 0:
        resource = struct.unpack_from('<Q', instances, o + 0x20)[0] if o + 0x28 <= len(instances) else 0
        if manifest.get(model, resource) is not None:
            return resource
        o = instances.find(key, o + 1)
    return None


def face_triangles(manifest, resource):
    """Picture-face triangles [(p0, p1, p2, uv0, uv1, uv2)] of the first mesh of a model.

    Vertex streams: 20 bytes (8 unknown, float2 UV, unorm2 UV) then 28 bytes (float3
    position, packed normals); then u16 indices. Other layouts (a big combat-lobby set
    piece that carries one poster) return [] and are left out."""
    cpu = manifest.get(P.typesym('CGInstancedModelResource'), resource)
    gpu = manifest.get(P.typesym('CGInstancedModelResource', True), resource)
    marker = cpu.find(b'\x0b' + bytes(15), 0x4e0)
    b_offset, vertices = struct.unpack_from('<I', cpu, marker + 0x10)[0], struct.unpack_from('<I', cpu, marker + 0x1c)[0]
    if marker < 0 or b_offset != 20 * vertices:
        return []
    k = cpu.find(struct.pack('<I', 48 * vertices), marker)
    if k < 0 or 48 * vertices + 2 * struct.unpack_from('<I', cpu, k + 4)[0] > len(gpu):
        return []
    count = struct.unpack_from('<I', cpu, k + 4)[0]
    index = struct.unpack_from(f'<{count}H', gpu, 48 * vertices)
    pos = [struct.unpack_from('<3f', gpu, b_offset + i * 28) for i in range(vertices)]
    uv = [struct.unpack_from('<2f', gpu, i * 20 + 8) for i in range(vertices)]
    tris = []
    for t in range(0, count, 3):
        ids = index[t:t + 3]
        if all(FACE_U[0] <= uv[i][0] <= FACE_U[1] and FACE_V[0] <= uv[i][1] <= FACE_V[1] for i in ids):
            tris.append(tuple(pos[i] for i in ids) + tuple(uv[i] for i in ids))
    return tris


def upright(tris):
    """True when the art's u runs along the mesh's x and v down its y (the poster is
    upright). Double-sided posters mirror u on the back, so only |dP/du| is checked.
    (The lobby news board maps its art sideways; it is left out.)"""
    along_x = along_y = down = 0.0
    for p0, p1, p2, a, b, c in tris:
        du1, dv1, du2, dv2 = b[0] - a[0], b[1] - a[1], c[0] - a[0], c[1] - a[1]
        det = du1 * dv2 - du2 * dv1
        if abs(det) < 1e-9:
            continue
        e1 = [p1[i] - p0[i] for i in range(3)]
        e2 = [p2[i] - p0[i] for i in range(3)]
        dpdu = [(dv2 * e1[i] - dv1 * e2[i]) / det for i in range(3)]
        dpdv = [(du1 * e2[i] - du2 * e1[i]) / det for i in range(3)]
        w = abs(det)
        along_x += abs(dpdu[0]) * w
        along_y += abs(dpdu[1]) * w
        down += dpdv[1] * w
    return along_x > 4 * along_y and down < 0


def build():
    manifest = P.Manifest(MID)
    poster_type = P.typesym('CR15NetDynamicPosterCR')
    meshes, mesh_index, posters, skipped, display_only = [], {}, {}, set(), set()
    for level, _, _ in manifest.by_type(poster_type):
        rows = records(manifest.get(poster_type, level))
        transforms = records(manifest.get(P.typesym('CTransformCR'), level), TRANSFORM_STRIDE)
        instances = manifest.get(P.typesym('CInstanceModelCR'), level)
        for row in rows:
            actor = struct.unpack_from('<Q', row, 8)[0]
            if actor in posters:
                continue
            resource = mesh_of(manifest, instances, actor)
            t = next((t for t in transforms if struct.unpack_from('<Q', t, 8)[0] == actor), None)
            if resource is None or t is None:
                continue
            if struct.unpack_from('<q', t, 0x58)[0] != -1:
                raise SystemExit(f'Poster {actor:016x} has a parent transform; not supported')
            if resource not in mesh_index:
                tris = face_triangles(manifest, resource)
                if not tris:  # face not found (other vertex layouts): dock only, no touch or shots
                    display_only.add(resource)
                    mesh_index[resource] = NO_MESH
                elif not upright(tris):
                    skipped.add(resource)
                    mesh_index[resource] = None
                else:
                    mesh_index[resource] = len(meshes)
                    meshes.append((resource, tris))
            m = mesh_index[resource]
            if m is None:
                continue
            q = struct.unpack_from('<4f', t, 0x20)
            p = struct.unpack_from('<3f', t, 0x30)
            s = struct.unpack_from('<3f', t, 0x3c)
            posters[actor] = (m, p, q, s)

    f = lambda v: f'{v:.6f}f'
    lines = ['// Generated by tools/build_poster_table.py from the game data -- do not edit.',
             '#pragma once', '#include <cstdint>', '',
             f'constexpr unsigned POSTER_TEX_W = {POSTER_TEX_W}, POSTER_TEX_H = {POSTER_TEX_H};',
             f'constexpr unsigned POSTER_X0 = {POSTER_X0}, POSTER_Y0 = {POSTER_Y0}, POSTER_FRAME_W = {TEX_W}, POSTER_FRAME_H = {TEX_H};',
             '', 'struct PosterTri { float p[3][3]; float uv[3][2]; };',
             'struct PosterMesh { const PosterTri* tris; unsigned count; };',
             '// mesh = POSTER_NO_MESH: the face was not found; the poster can show the arcade but has no touch or shots.',
             f'constexpr unsigned POSTER_NO_MESH = 0x{NO_MESH:x};',
             'struct PosterInfo { uint64_t actor; unsigned mesh; float pos[3], rot[4], scale[3]; };', '']
    for i, (resource, tris) in enumerate(meshes):
        lines.append(f'// model {resource:016x}: {len(tris)} face triangles')
        lines.append(f'constexpr PosterTri POSTER_MESH_{i}[] = {{')
        for p0, p1, p2, a, b, c in tris:
            pts = ','.join('{' + ','.join(f(v) for v in pt) + '}' for pt in (p0, p1, p2))
            uvs = ','.join('{' + ','.join(f(v) for v in u) + '}' for u in (a, b, c))
            lines.append(f'    {{{{{pts}}},{{{uvs}}}}},')
        lines.append('};')
    lines.append('constexpr PosterMesh POSTER_MESHES[] = {' +
                 ', '.join(f'{{POSTER_MESH_{i}, {len(t)}}}' for i, (_, t) in enumerate(meshes)) + '};')
    lines.append('constexpr PosterInfo POSTERS[] = {')
    for actor, (m, p, q, s) in sorted(posters.items()):
        lines.append(f'    {{0x{actor:016x}, {m}, {{{",".join(f(v) for v in p)}}}, {{{",".join(f(v) for v in q)}}}, {{{",".join(f(v) for v in s)}}}}},')
    lines.append('};')
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text('\n'.join(lines) + '\n')
    print(f'Poster table: {len(posters)} posters, {len(meshes)} meshes'
          + (f' (dock only, no face found: {", ".join(f"{r:016x}" for r in display_only)})' if display_only else '')
          + (f' (left out meshes {", ".join(f"{r:016x}" for r in skipped)})' if skipped else '') + f' -> {OUT}')


if __name__ == '__main__':
    build()
