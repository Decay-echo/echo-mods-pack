"""Patch RetroArch's Network RetroPad: held buttons stay held, and every packet is read each frame.

    python tools/patch_retroarch.py status            # is apps/retroarch/retroarch.exe patched?
    python tools/patch_retroarch.py patch             # keeps the original as retroarch.exe.orig
    python tools/patch_retroarch.py unpatch           # puts retroarch.exe.orig back
    python tools/patch_retroarch.py ... --exe PATH    # another RetroArch

Why: Echo Arcade's on-screen RetroPad reaches RetroArch over its Network RetroPad (UDP), sending
a packet only when a button changes. RetroArch 1.22.2 and earlier read one packet per frame and,
on a frame with nothing queued, decide between "nothing new" and "connection lost" by looking at
errno; Winsock never sets errno, so the decision rests on whatever the last C runtime call left
there. On a fresh install that happened to be ENOENT and holds worked; once RetroArch had saved
its config, playlists and history it was something else, and every frame without a packet
released all the controls: a held D-pad lasted one frame. Fixed upstream on 2026-10-04
(libretro/RetroArch cfb1204314, "input: Network RetroPad takes all that was sent each poll"),
which is not in any release yet.

The patch skips the release: when a frame has no packet, what was held stays held, which is also
what the upstream fix does.

Read-all: the same poll reads ONE packet per user per frame. That is fine for a finger on the
tablet, but FREEZE plays the Touch controllers through it: buttons pressed together would land a
frame apart, and moving sticks would queue up into seconds of lag. After a packet is parsed the
code moves on to the next user; the patch points those exits back at the read, so it keeps
reading until nothing is waiting (recvfrom fails, which still goes on to the next user). That is
the rest of the upstream fix.

The code is located by its byte patterns (the recvfrom call with the packet size, the size check
and the parse that follows) and every byte is verified before it is written, so an unknown build
is left alone (or gets only the part that matches).
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / 'apps/retroarch/retroarch.exe'

# input_driver_poll, the Network RetroPad part, as GCC lays it out for RetroArch 1.22.x win64:
#   mov r8d, 0x14 ; mov [rsp+28h], 0 ; mov [rsp+20h], 0     recvfrom(fd, &msg, sizeof msg = 20, 0, NULL, NULL)
#   call rdi ; movsxd rdx, eax ; cmp eax, 0x14 ; jne <check>  not a whole message -> errno check / release
RECV = bytes.fromhex('41b814000000' '48c744242800000000' '48c744242000000000' 'ffd7' '4863d0' '83f814' '0f85')
# <check>: cmp rdx, -1 ; je <errno test>   (anything else: release every control of this user)
CHECK = bytes.fromhex('4883faff' '0f84')
# in the parse that follows the size check: cmp eax, 5 (RETRO_DEVICE_ANALOG) ; jne <next user>
NEXT = bytes.fromhex('83f805' '75')
PATCH_LEN = 10  # cmp rdx,-1 (4 bytes) + je rel32 (6 bytes)

# Read-all. Templates are hex with '..' for any byte (the jump offsets); each jump is
# (offset in the template, instruction length) and must point at <next user> (or, once
# patched, at <read>).
# <read>: mov rax, [fds] ; mov r14d, ebx ; mov rcx, [rax+r14*8] ; cmp rcx, -1 ; je   (0x23 before RECV)
READ = '488b05........' '4189de' '4a8b0cf0' '4883f9ff' '0f84'
READ_BEFORE_RECV = 0x23
# The parse, right after the size check: device JOYPAD -> <joypad>, ANALOG -> store the axis.
PARSE = ('8b842454010000' '83f801' '0f84........' '83f805' '75..' '8b84245c010000' '83f801' '0f8f........'
         '8b942458010000' '83fa01' '0f8f........' '8d0450' '0fb7942460010000' '4898' '48c1e004' '498d440640'
         '664189944760010000' 'e9........')
PARSE_JUMPS = [(19, 2), (31, 6), (47, 6), (84, 5)]
PARSE_JOYPAD = 12  # rel32 of "je <joypad>"
# <joypad>: set or clear the button bit of this user.
JOYPAD = ('8b8c245c010000' '83f90f' '0f8f........' 'd3e0' '89c2' 'f7d2' '4863d2' '482354dd00' '6683bc246001000000'
          '488954dd00' '0f84........' '4898' '4809d0' '488944dd00' 'e9........')
JOYPAD_JUMPS = [(10, 6), (44, 6), (60, 5)]

KNOWN = {  # sha256 of the original retroarch.exe -> name (for the message only; the pattern decides)
    '81c11b6f24932bf7918f05eee8928035bff3887335fd2a081507c75e9d94d06a': 'RetroArch 1.22.2 win64 (stable)',
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def matches(data, at, template):
    if at < 0 or at + len(template) // 2 > len(data):
        return False
    return all(template[i:i + 2] == '..' or data[at + i // 2] == int(template[i:i + 2], 16) for i in range(0, len(template), 2))


def jump_target(data, at, length):
    if length == 2:
        return at + 2 + struct.unpack_from('<b', data, at + 1)[0]
    return at + length + struct.unpack_from('<i', data, at + length - 4)[0]


def jump_to(at, length, target):
    """The new offset bytes (and where they go) for the jump at `at` to land on `target`."""
    rel = target - (at + length)
    if length == 2:
        if not -128 <= rel <= 127:
            raise LookupError('a short jump cannot reach the read')
        return at + 1, struct.pack('<b', rel)
    return at + length - 4, struct.pack('<i', rel)


def locate(data):
    """Returns (held, read_all) or raises LookupError. Each part is (done, edits); edits is a list of
    (file offset, new bytes). read_all is (None, []) when this build's parse doesn't match."""
    hits = [i for i in range(len(data) - len(RECV)) if data.startswith(RECV, i)]
    if len(hits) != 1:
        raise LookupError(f'{len(hits)} matches of the Network RetroPad poll (expected 1)')
    at = hits[0] + len(RECV)                       # the jne's rel32
    jne_end = at + 4
    check = jne_end + struct.unpack_from('<i', data, at)[0]
    if not 0 < check < len(data) - PATCH_LEN:
        raise LookupError('the size-mismatch branch points outside the file')
    parse = data[jne_end: jne_end + 0x40]
    n = parse.find(NEXT)
    if n < 0:
        raise LookupError('the "next user" jump was not found after the parse')
    rel8 = struct.unpack_from('<b', parse, n + len(NEXT))[0]
    next_user = jne_end + n + len(NEXT) + 1 + rel8
    read = hits[0] - READ_BEFORE_RECV
    if next_user == read and data[check] == 0xE9:  # read-all already points that jump at <read>
        next_user = check + 5 + struct.unpack_from('<i', data, check + 1)[0]  # the held patch's jmp knows <next user>
    replacement = b'\xe9' + struct.pack('<i', next_user - (check + 5)) + b'\x90' * (PATCH_LEN - 5)  # jmp next_user ; nops
    current = data[check: check + PATCH_LEN]
    if current == replacement:
        held = (True, [])
    elif current.startswith(CHECK):
        held = (False, [(check, replacement)])
    else:
        raise LookupError('unexpected bytes at the release check')

    joypad = jne_end + PARSE_JOYPAD + 4 + struct.unpack_from('<i', data, jne_end + PARSE_JOYPAD)[0] if matches(data, jne_end, PARSE) else -1
    if not (matches(data, read, READ) and matches(data, jne_end, PARSE) and matches(data, joypad, JOYPAD)):
        return held, (None, [])
    jumps = [(jne_end + o, n) for o, n in PARSE_JUMPS] + [(joypad + o, n) for o, n in JOYPAD_JUMPS]
    edits, done = [], 0
    for j, length in jumps:
        target = jump_target(data, j, length)
        if target == read:
            done += 1
        elif target == next_user:
            edits.append(jump_to(j, length, read))
        else:
            return held, (None, [])  # not the layout we know: leave it
    return held, (done == len(jumps), edits)


def status(exe: Path):
    """Returns (state, message, edits): state is patched / unpatched / unknown."""
    data = exe.read_bytes()
    orig = exe.with_suffix('.exe.orig')
    try:
        held, read_all = locate(data)
    except LookupError as e:
        return 'unknown', f'{exe.name}: not a RetroArch build this tool knows ({e}); left alone', None
    todo = held[1] + read_all[1]
    if not todo:
        extra = '; every RetroPad packet is read each frame' if read_all[0] else '; read-all not available for this build'
        return 'patched', f'{exe.name}: patched (held buttons stay held{extra})' + ('' if orig.exists() else '; no .orig backup'), None
    missing = []
    if not held[0]:
        missing.append('a held button drops after one frame')
    if read_all[0] is False:
        missing.append('one RetroPad packet per frame (FREEZE controllers would lag)')
    return 'unpatched', f'{exe.name}: NOT fully patched: ' + '; '.join(missing), todo


def patch(exe: Path):
    state, text, todo = status(exe)
    print(text)
    if state != 'unpatched':
        return state == 'patched'
    data = bytearray(exe.read_bytes())
    orig = exe.with_suffix('.exe.orig')
    if not orig.exists():
        orig.write_bytes(data)
    for off, new in todo:
        data[off: off + len(new)] = new
    exe.write_bytes(data)
    name = KNOWN.get(sha(orig.read_bytes()), 'a RetroArch build with the same code layout')
    print(f'Patched {exe} ({name}, {len(todo)} change(s)); original kept as {orig.name}')
    state, text, _ = status(exe)
    print(text)
    return state == 'patched'


def unpatch(exe: Path):
    orig = exe.with_suffix('.exe.orig')
    if not orig.exists():
        print(f'{orig.name} not found: nothing to put back')
        return False
    exe.write_bytes(orig.read_bytes())
    orig.unlink()
    print(f'Restored the original {exe}')
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('action', choices=['status', 'patch', 'unpatch'])
    ap.add_argument('--exe', type=Path, default=DEFAULT_EXE)
    a = ap.parse_args()
    if not a.exe.exists():
        print(f'{a.exe} not found')
        return 1
    if a.action == 'status':
        state, text, _ = status(a.exe)
        print(text)
        return 0 if state == 'patched' else 1
    if a.action == 'patch':
        return 0 if patch(a.exe) else 1
    return 0 if unpatch(a.exe) else 1


if __name__ == '__main__':
    sys.exit(main())
