"""Execute retail player_anim and its complete advance/install helpers.

The ten move records are synthetic, in native runtime units. The original
PAL/US instructions choose and advance them; only BIOS rand is hooked. Output
contains numeric state, not retail instructions or model assets. This does not
claim to execute the caller's physics, weapon driver, or a complete PS1 frame.
"""
import argparse
import hashlib
import json
import random
import struct
from pathlib import Path

from generate_heap_reference import Form1Disc, LoaderMachine, NTSC_SEGMENT_SHA
from generate_resource_reference import SEGMENT_SHA

BODY = 0x801D0000
MOVES = 0x801D1000
GLOBALS = (0x800B2988, 0x800B2964, 0x800B2990, 0x800B299C,
           0x800B29A4, 0x800B29AC, 0x800B29B4, 0x800B2970,
           0x800B2978, 0x800B2980)
RANGES = ((100, 180, 100), (200, 250, 200), (300, 360, 360),
          (400, 480, 480), (500, 600, 600), (610, 690, 690),
          (700, 760, 760), (800, 840, 840), (900, 950, 950),
          (1000, 1060, 1060))
NAMES = ('Stand', 'Run', 'Attak', 'Death 1', 'Death 2', 'Death 3',
         'Jump', 'Pain 1', 'Pain 2', 'Pain 3')


def execute(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
    finally:
        disc.stream.close()
    size = struct.unpack_from('<I', exe, 0x1C)[0]
    image = exe[2048:2048 + size]
    digest = hashlib.sha256(image).hexdigest()
    assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
    m = LoaderMachine(image, None, ntsc)
    # Execute the actual load-time scaling and player-model binding prefix.
    # Animation block C is absent; block B's eight chains are empty. This
    # isolates block D without hooking any instruction in these functions.
    model, data = 0x801D2000, 0x801D2100
    m.put(model + 8, b'Male2\0')
    m.write(model + 0x34, 4, model)
    m.write(model + 0x38, 4, data)
    m.write(model + 0x30, 4, 128)
    for i, (name, (start, end, rest)) in enumerate(zip(NAMES, RANGES)):
        m.put(data + i * 20, name.encode().ljust(12, b'\0') +
              struct.pack('<HHHH', start // 5, end // 5, end // 5, 1))
    m.call(0x8006C214, model)
    loaded = [list(struct.unpack('<HHH', bytes(m.mem[(data & 0x1FFFFFFF) + i * 20 + 12:
                                                     (data & 0x1FFFFFFF) + i * 20 + 18])))
              for i in range(10)]
    assert loaded == [[start, end, end] for start, end, _ in RANGES]
    m.registers()
    m.r[4] = model
    m.run(m.address(0x8003C59C), stop=m.address(0x8003CC08), limit=10000)
    bound = []
    for i, address in enumerate(GLOBALS):
        ptr = m.read(m.address(address), 4)
        assert ptr == data + i * 20
        bound.append([m.read(ptr + k, 2) for k in (12, 14, 16)])
    assert bound == [list(row) for row in RANGES]
    selectors = []
    client = 0x801D3000
    # Execute four bounded live-player selector blocks. The jump and weapon
    # return values are explicit inputs; their physics/fire functions are not
    # called by this fixture. No instructions between these blocks are claimed.
    for jump in (0, 1):
        for x, z in ((0, 0), (1, 0), (0, 65535), (32768, 32767)):
            for fire in (0, 1, 2):
                for pain in (0, 1, 128):
                    m.registers()
                    m.r[17], m.r[19], m.r[21], m.r[2] = client, BODY, 0, jump
                    m.write(client + 4, 2, x)
                    m.write(client + 8, 2, z)
                    m.write(BODY + 0xDF, 1, pain)
                    m.run(m.address(0x8003A924), stop=m.address(0x8003A938), limit=100)
                    m.run(m.address(0x8003AB3C), stop=m.address(0x8003AB60), limit=100)
                    m.r[2] = fire
                    m.run(m.address(0x8003ADA0), stop=m.address(0x8003ADB8), limit=100)
                    m.run(m.address(0x8003ADF8), stop=m.address(0x8003AE10), limit=100)
                    assert m.read(BODY + 0xDF, 1) == 0
                    selectors.append([jump, x, z, fire, pain, m.r[21]])
    for i, (address, move) in enumerate(zip(GLOBALS, RANGES)):
        m.put(MOVES + i * 32, bytes(12) + struct.pack('<HHHH', *move, 1))
        m.write(m.address(address), 4, MOVES + i * 32)
    rows = []
    rng = random.Random(0x3CE14)

    def run(state, want, dt, contact, seed):
        move, position, flags = state
        m.write(BODY + 0x2E0, 4, MOVES + move * 32 if move >= 0 else 0)
        m.write(BODY + 0x100, 2, position)
        m.write(BODY + 0x102, 2, flags)
        m.write(BODY + 0x98, 4, contact)
        m.write(m.address(0x800B2DB4), 4, dt)
        m.rng_state = seed
        m.call(0x8003CE14, BODY, want)
        ptr = m.read(BODY + 0x2E0, 4)
        result = ((ptr - MOVES) // 32 if ptr else -1,
                  struct.unpack('<h', struct.pack('<H', m.read(BODY + 0x100, 2)))[0],
                  m.read(BODY + 0x102, 2))
        assert -1 <= result[0] < 10
        rows.append((*state, want, dt, contact, seed, *result, m.rng_state))
        return result, m.rng_state

    # Every current/request pair, low animation flags and end/jump boundaries.
    for current in range(-1, 10):
        start, end, _ = RANGES[current] if current >= 0 else (0, 0, 0)
        for want in range(6):
            for flags in (0, 1, 2, 3, 0x80):
                for position in (start, start + 29, start + 30, start + 31,
                                 end - 10, end - 1, end, end + 1):
                    run((current, position, flags), want,
                        rng.choice((1, 5, 10, 12, 30, 90)),
                        rng.choice((0, 1 << 5, 1 << 6, 1 << 11, 1 << 13)),
                        rng.getrandbits(32))
    # Successive calls retain flags, position and random state. These exercise
    # long holds, repeated attacks/pains, landing, interrupted clips and death.
    for sequence in range(12):
        state, seed = (-1, 0, 0), sequence + 1
        for tick in range(120):
            if tick < 5:
                want = 0
            elif tick == 5:
                want = 2
            elif tick < 25:
                want = 0 if sequence % 2 else 1
            elif tick < 60:
                want = rng.randrange(6) if sequence > 7 else rng.choice((0, 1, 3, 5))
            else:
                want = 4
            state, seed = run(state, want, (1, 5, 10, 12, 30)[sequence % 5],
                              0 if 5 <= tick < 20 else 1 << 5, seed)
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest,
            'loaded_ranges': loaded, 'bound_ranges': bound,
            'selectors': selectors,
            'cases': rows, 'hooks': m.hooks}


def header(regions):
    assert regions[0]['cases'] == regions[1]['cases'], 'PAL/US animation differs'
    assert regions[0]['selectors'] == regions[1]['selectors'], 'PAL/US selection differs'
    lines = ['/* Generated from complete PAL/US player_anim executions. */',
             '#ifndef Q2_PLAYER_ANIMATION_REFERENCE_H', '#define Q2_PLAYER_ANIMATION_REFERENCE_H',
             'static const q2_player_anim_range native_player_ranges[Q2_PMOVE_COUNT] = {']
    lines += ['    {' + ', '.join(map(str, row)) + '},' for row in RANGES]
    lines += ['};', 'static const int native_player_selection[][6] = {']
    lines += ['    {' + ', '.join(map(str, row)) + '},' for row in regions[0]['selectors']]
    lines += ['};', 'static const struct {',
              '    int move, position, flags, want, dt; unsigned contact, seed;',
              '    int result_move, result_position, result_flags; unsigned result_seed;',
              '} native_player_animation[] = {']
    for row in regions[0]['cases']:
        lines.append('    {' + ', '.join(str(v) + ('u' if i in (5, 6, 10) else '')
                                         for i, v in enumerate(row)) + '},')
    return '\n'.join(lines + ['};', '#endif', ''])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pal', type=Path)
    p.add_argument('us', type=Path)
    p.add_argument('--output', type=Path, default=Path(__file__).with_name('player_animation_reference.h'))
    p.add_argument('--trace', required=True, type=Path)
    p.add_argument('--check', action='store_true')
    args = p.parse_args()
    regions = [execute(args.pal, False), execute(args.us, True)]
    result = header(regions)
    if args.check:
        assert args.output.read_text(encoding='utf-8') == result
    else:
        args.output.write_text(result, encoding='utf-8')
    args.trace.write_text(json.dumps([{k: v for k, v in r.items() if k != 'cases'} |
                                    {'cases': len(r['cases'])} for r in regions], indent=2) + '\n', encoding='utf-8')
    print(f"Native player animation: {len(regions[0]['cases'])} complete calls per region; PAL/US agree")


if __name__ == '__main__':
    main()
