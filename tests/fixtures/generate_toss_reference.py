"""Execute retail toss/mover arithmetic with explicit collision-service inputs.

PAL and US instructions, including SDK square-root/normalization and their
tables, execute unchanged. Only collision queries, matrix output and BIOS
services are hooked. GTE SQR/GPF and register transfers are modeled below;
these are CPU arithmetic fixtures, not independent hardware GTE captures.
No original code or assets are exported.
"""
import argparse
import hashlib
import json
import math
import random
import struct
from pathlib import Path

from generate_combat_reference import signed
from generate_heap_reference import Form1Disc, LoaderMachine, NTSC_SEGMENT_SHA
from generate_resource_reference import SEGMENT_SHA

BODY, NODES, PLANE, POINT = 0x801D0000, 0x801D1000, 0x801D1100, 0x801D1200


class TossMachine(LoaderMachine):
    def __init__(self, image, ntsc):
        super().__init__(image, None, ntsc)
        self.gte = [0] * 32
        self.mode = 0
        self.fraction = 2048
        self.normal = [0, 4096, 0]
        self.intent = self.hit = [0, 0, 0]

    def cop2(self, word):
        rs, rt, rd = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
        if rs == 4:  # MTC2
            value = self.r[rt]
            if rd in (8, 9, 10, 11):
                value = signed(value, 16)
            elif rd == 30:
                bits = value ^ (0xFFFFFFFF if value & 0x80000000 else 0)
                self.gte[31] = 32 - bits.bit_length()
            else:
                raise ValueError(f'Unexpected GTE write {word:08x}')
            self.gte[rd] = value
        elif rs == 0:  # MFC2, with the interpreter's normal load delay.
            assert rd in (25, 26, 27, 31)
            return rt, self.gte[rd] & 0xFFFFFFFF
        elif rs >= 16 and (word & 63) in (0x28, 0x3D):
            shift = 12 if word & (1 << 19) else 0
            for i in range(1, 4):
                value = self.gte[8 + i] * (self.gte[8 + i] if word & 63 == 0x28 else self.gte[8])
                value >>= shift
                self.gte[24 + i] = signed(value)
                self.gte[8 + i] = max(-32768, min(32767, value))
        else:
            raise ValueError(f'Unexpected GTE instruction {word:08x}')
        return None

    def vector(self, address):
        return [signed(self.read(address + k * 4, 4)) for k in range(3)]

    def put_vector(self, address, values):
        for k, value in enumerate(values):
            self.write(address + k * 4, 4, value)

    def intercept(self, pc):
        pal = self.canonical(pc)
        if pal == 0x80053974:  # Runtime solid-entity segment query.
            if self.mode == 2:
                start, end = self.vector(self.r[4]), self.vector(self.r[5])
                point = [a + int((b - a) * self.fraction / 4096) for a, b in zip(start, end)]
                self.put_vector(self.r[6], point)
                record = self.address(0x800CBA10)
                for k, value in enumerate(self.normal): self.write(record + 12 + k * 2, 2, value)
                self.write(record + 18, 2, 2)
                self.write(record + 22, 2, 0)
                self.r[2] = 0
            else:
                self.r[2] = 1
        elif pal == 0x80044C44:  # Hull query; supplies a contact, not arithmetic.
            hull = self.r[4]
            start, end = self.vector(self.r[5]), self.vector(self.r[6])
            self.intent = end
            self.hit = [a + int((b - a) * self.fraction / 4096) for a, b in zip(start, end)] if self.mode == 1 else end
            self.put_vector(POINT, self.hit)
            self.write(hull + 0x1A, 2, 0)
            self.r[2] = int(self.mode != 1)
        elif pal == 0x800544EC:  # No damageable actor in these geometry cases.
            self.r[2] = 0
        elif pal == 0x80089E38:  # Rotation matrix output does not feed physics.
            pass
        else:
            return super().intercept(pc)
        key = f'{pc:08x}'
        self.hooks[key] = self.hooks.get(key, 0) + 1
        return True


def execute(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
    finally:
        disc.stream.close()
    image = exe[2048:2048 + struct.unpack_from('<I', exe, 0x1C)[0]]
    digest = hashlib.sha256(image).hexdigest()
    assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
    m = TossMachine(image, ntsc)
    # Verify the formula used to reconstruct the two SDK tables in the port.
    for n in range(64, 256):
        assert m.read(m.address(0x800A9C54) + (n - 64) * 2, 2) == math.isqrt((1 << 30) // n)
        assert m.read(m.address(0x800AA340) + (n - 64) * 2, 2) == math.isqrt(n << 18)
    assert m.read(m.address(0x800AE974), 4) == 0x00030900
    m.write(m.address(0x800AE8B4), 4, 0)  # Explicitly outside the menu.
    rng = random.Random(0x46B98)
    rows = []
    normals = [(4096, 0, 0), (-4096, 0, 0), (0, 4096, 0), (0, -4096, 0),
               (0, 0, 4096), (0, 0, -4096), (2896, 2896, 0), (-2365, 2365, 2365)]
    for i in range(2592):
        dt = (1, 6, 10, 12, 20, 30)[i % 6]
        gravity = (0, 32, -32)[(i // 6) % 3]
        flags = (0, 0, 0, 0, 0x20, 0x40)[(i // 18) % 6]
        start = [rng.randrange(-50000, 50001) for _ in range(3)]
        velocity = [rng.randrange(-8000, 8001) for _ in range(3)]
        if i < 24: velocity = [(0, 0, 0), (0, 900, 0), (0, 1600, 0), (1, 1601, -1)][i % 4]
        angles = [rng.randrange(-32768, 32768) for _ in range(3)]
        seed = rng.getrandbits(32)
        m.mode = (i // 108) % 3
        m.fraction = (0, 1, 1024, 2048, 4095)[(i // 18) % 5]
        m.normal = list(normals[(i // 324) % len(normals)])
        m.put(BODY, bytes(0x400))
        m.put_vector(BODY + 0x54, start)
        for k in range(3):
            m.write(BODY + 0xE0 + k * 2, 2, velocity[k])
            m.write(BODY + 0xE6 + k * 2, 2, angles[k])
            m.write(PLANE + 6 + k * 2, 2, m.normal[k])
        m.write(BODY + 0x98, 4, flags)
        m.write(BODY + 0x9C, 2, 2600)
        m.write(m.address(0x800AE924), 4, gravity)
        m.write(m.address(0x800B2DB4), 4, dt)
        hull = m.address(0x800C8E90)
        m.write(hull + 4, 4, NODES)
        m.write(hull + 0x20, 4, PLANE)
        m.write(hull + 0x28, 4, POINT)
        m.write(NODES + 32, 1, 3)
        m.rng_state = seed
        m.intent = m.hit = list(start)
        contact = m.call(0x80046B98, BODY, 0, 0, 0x00030900)
        out = [*m.vector(BODY + 0x54),
               *[signed(m.read(BODY + 0xE0 + k * 2, 2), 16) for k in range(3)],
               *[signed(m.read(BODY + 0xE6 + k * 2, 2), 16) for k in range(3)],
               m.read(BODY + 0x98, 4), m.rng_state, contact]
        rows.append([dt, gravity, flags, seed, *start, *velocity, *angles,
                     m.mode, m.fraction, *m.normal, *m.intent, *m.hit, *out])
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest,
            'cases': rows, 'hooks': m.hooks}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('pal', type=Path)
    p.add_argument('us', type=Path)
    p.add_argument('--trace', required=True, type=Path)
    p.add_argument('--output', type=Path, default=Path(__file__).with_name('toss_reference.h'))
    p.add_argument('--check', action='store_true')
    args = p.parse_args()
    results = [execute(args.pal, False), execute(args.us, True)]
    assert results[0]['cases'] == results[1]['cases'], 'PAL/US toss differs'
    lines = ['/* Native toss/mover executions; collision services are explicit inputs. */',
             '#ifndef Q2_TOSS_REFERENCE_H', '#define Q2_TOSS_REFERENCE_H',
             'static const struct {',
             '    int dt, gravity; unsigned flags, seed;',
             '    int start[3], velocity[3], angles[3], mode, fraction, normal[3];',
             '    int intent[3], hit[3], position[3], result_velocity[3], result_angles[3];',
             '    unsigned result_flags, result_seed; int contact;',
             '} native_toss[] = {']
    for r in results[0]['cases']:
        fields = [str(r[0]), str(r[1]), f'{r[2]}u', f'{r[3]}u']
        fields += ['{' + ','.join(map(str, r[a:a+3])) + '}' for a in (4, 7, 10)]
        fields += [str(r[13]), str(r[14])]
        fields += ['{' + ','.join(map(str, r[a:a+3])) + '}' for a in (15, 18, 21, 24, 27, 30)]
        fields += [f'{r[33]}u', f'{r[34]}u', str(r[35])]
        lines.append('    {' + ', '.join(fields) + '},')
    text = '\n'.join(lines + ['};', '#endif', ''])
    if args.check: assert args.output.read_text(encoding='utf-8') == text
    else: args.output.write_text(text, encoding='utf-8')
    args.trace.write_text(json.dumps([{k: v for k, v in r.items() if k != 'cases'} |
                                     {'cases': len(r['cases'])} for r in results], indent=2) + '\n', encoding='utf-8')
    print(f"Native toss: {len(results[0]['cases'])} calls per region; PAL/US agree")


if __name__ == '__main__':
    main()
