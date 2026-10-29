"""Execute PAL/US grenade owner gates and complete native actor queries.

No collision, root or quadratic routines are replaced. GTE arithmetic comes
from the toss fixture's explicit model; no hardware-capture claim is made.
Owner cases execute bounded thinker prefixes with initialized registers.
Generated output contains numeric states only, never native code or assets.
"""
import argparse
import hashlib
import json
import random
import struct
from pathlib import Path

from generate_combat_reference import signed
from generate_heap_reference import Form1Disc, NTSC_SEGMENT_SHA
from generate_resource_reference import SEGMENT_SHA
from generate_toss_reference import TossMachine

START, END, OUT, LIST = 0x801D0000, 0x801D0020, 0x801D0040, 0x801D0080
BODY, OWNER = 0x801D0100, 0x801D0600
ACTORS = [0x801D1000 + 0x400 * i for i in range(4)]


class ContactMachine(TossMachine):
    def intercept(self, pc):
        # Execute the real query instead of the toss fixture's empty service.
        if self.canonical(pc) == 0x800544EC:
            return False
        return super().intercept(pc)

    def query(self, skip, count):
        self.registers()
        self.r[29] = 0x801FC000
        self.r[4:8] = [START, END, ACTORS[skip] if skip >= 0 else 0, OUT]
        self.write(self.r[29] + 16, 4, LIST)
        self.write(self.r[29] + 20, 4, LIST + 4 * count)
        self.run(self.address(0x800544EC), limit=20000)
        assert self.r[29] == 0x801FC000
        return ACTORS.index(self.r[2]) if self.r[2] else -1


def execute(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
    finally:
        disc.stream.close()
    image = exe[2048:2048 + struct.unpack_from('<I', exe, 0x1C)[0]]
    digest = hashlib.sha256(image).hexdigest()
    assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
    m = ContactMachine(image, ntsc)
    rng = random.Random(0x544EC)
    owners, queries = [], []
    for kind in range(2):
        for i in range(384):
            centre = [rng.randrange(-50000, 50001) for _ in range(3)]
            mins = [-rng.randrange(1, 601) for _ in range(3)]
            maxs = [rng.randrange(1, 601) for _ in range(3)]
            axis = (0, 2)[i % 2]
            edge = (mins[axis] - 40, maxs[axis] + 40)[(i // 2) % 2]
            point = list(centre)
            point[axis] += edge + (-1, 0, 1)[(i // 4) % 3]
            point[1] += (-1000000, 0, 1000000)[(i // 12) % 3]
            cleared = (i // 36) % 2
            present = (i // 72) % 2
            m.put(BODY, bytes(0x400))
            m.put_vector(BODY + 0x54, point)
            m.put_vector(OWNER + 0x78, [a + b for a, b in zip(centre, mins)])
            m.put_vector(OWNER + 0x84, [a + b for a, b in zip(centre, maxs)])
            m.write(BODY + 0x50, 4, 0 if cleared else 0xFFFFFFFF)
            m.write(BODY + 0x2E4, 4, OWNER if present else 0)
            m.registers()
            m.r[29] = 0x801FC000
            m.r[17] = BODY + 0x54 if kind else BODY
            m.r[18] = BODY if kind else OWNER
            # Grenade2's prologue keeps the position in s3; Grenade3 keeps
            # it in s1 and its owner in s3. The bounded prefix needs both.
            m.r[19] = OWNER if kind else BODY + 0x54
            begin, stop = (0x8004A838, 0x8004A8B8) if kind else (0x80049EF0, 0x80049F70)
            m.run(m.address(begin), stop=m.address(stop), limit=1000)
            owners.append([kind, present, cleared, centre, mins, maxs, point,
                           int(m.read(BODY + 0x50, 4) == 0), int(m.r[5] != 0)])

    for i in range(3072):
        start = [rng.randrange(-20000, 20001) for _ in range(3)]
        delta = [rng.randrange(-2000, 2001) for _ in range(3)]
        if i % 8 == 0: delta[0] = delta[2] = 0
        if i % 16 == 0: delta[1] = 0
        if i % 16 == 1: delta = [1000, 0, 0]
        skip, count = (i // 5) % 5 - 1, i % 5
        actors = []
        for j, address in enumerate(ACTORS):
            radius = (1, 10, 124, 286, 590)[(i + j) % 5]
            height = (1, 572, 607, 918, 1749)[(i // 5 + j) % 5]
            fraction = (0, 1, 1024, 2048, 4095, 4096, 8192)[(i + j) % 7]
            centre = [a + int(b * fraction / 4096) for a, b in zip(start, delta)]
            if i % 4:
                centre[2] += rng.randrange(-radius, radius + 1)
            damageable = (i + j) % 2
            actors.append([centre, radius, height, damageable])
            m.put(address, bytes(0x400))
            m.put_vector(address + 0x54, centre)
            m.write(address + 0x94, 2, radius)
            m.write(address + 0x96, 2, height)
            m.write(address + 0x1C, 4, 0x40000000 if damageable else 0)
            m.write(LIST + j * 4, 4, address)
        m.put_vector(START, start)
        m.put_vector(END, [a + b for a, b in zip(start, delta)])
        hit = m.query(skip, count)
        queries.append([start, delta, skip, count, actors, hit, m.vector(OUT)])

    # Small named cases used by the simulation integration regression. Keep
    # the retail outputs in the trace as well as the larger generated corpus.
    integration = []
    for name, start, end, skip, actors in [
        ('owner-between', [-100, 0, 0], [900, 0, 0], 0,
         [([0, 0, 0], 286, 572, 1), ([600, 0, 0], 50, 572, 1)]),
        ('return', [500, 0, 0], [0, 0, 0], -1, [([0, 0, 0], 286, 572, 1)]),
        ('immune', [0, 0, 0], [1000, 0, 0], -1,
         [([400, 0, 0], 50, 572, 0), ([800, 0, 0], 50, 572, 1)]),
        ('bfg-owner-between', [-100, 0, 0], [140, 0, 0], 0,
         [([0, 0, 0], 286, 572, 1), ([50, 0, 0], 50, 572, 1)]),
        ('bfg-immune', [0, 0, 0], [240, 0, 0], -1,
         [([50, 0, 0], 50, 572, 0), ([150, 0, 0], 50, 572, 1)]),
    ]:
        m.put_vector(START, start)
        m.put_vector(END, end)
        for j, (centre, radius, height, damageable) in enumerate(actors):
            address = ACTORS[j]
            m.put(address, bytes(0x400))
            m.put_vector(address + 0x54, centre)
            m.write(address + 0x94, 2, radius)
            m.write(address + 0x96, 2, height)
            m.write(address + 0x1C, 4, 0x40000000 if damageable else 0)
            m.write(LIST + j * 4, 4, address)
        hit = m.query(skip, len(actors))
        integration.append({'name': name, 'start': start, 'end': end, 'ignore': skip,
                            'actors': actors, 'hit': hit, 'point': m.vector(OUT)})
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest,
            'owners': owners, 'queries': queries, 'integration': integration, 'hooks': m.hooks}


def vec(values):
    return '{' + ','.join(map(str, values)) + '}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pal', type=Path)
    parser.add_argument('us', type=Path)
    parser.add_argument('--trace', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).with_name('projectile_contact_reference.h'))
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    runs = [execute(args.pal, False), execute(args.us, True)]
    for key in ('owners', 'queries', 'integration'):
        assert runs[0][key] == runs[1][key], f'PAL/US {key} differ'
    lines = ['/* Native grenade owner prefixes and complete actor queries. */',
             '#ifndef Q2_PROJECTILE_CONTACT_REFERENCE_H', '#define Q2_PROJECTILE_CONTACT_REFERENCE_H',
             'static const struct {',
             '    int hand, present, cleared, centre[3], mins[3], maxs[3], point[3];',
             '    int result_cleared, result_ignored;',
             '} native_projectile_owners[] = {']
    for r in runs[0]['owners']:
        fields = list(map(str, r[:3])) + [vec(v) for v in r[3:7]] + list(map(str, r[7:]))
        lines.append('    {' + ','.join(fields) + '},')
    lines += ['};', 'static const struct {',
              '    int start[3], delta[3], skip, count;',
              '    struct { int centre[3], radius, height, damageable; } actors[4];',
              '    int hit, point[3];', '} native_projectile_contacts[] = {']
    for start, delta, skip, count, actors, hit, point in runs[0]['queries']:
        bodies = ['{' + vec(a[0]) + ',' + ','.join(map(str, a[1:])) + '}' for a in actors]
        lines.append('    {' + ','.join([vec(start), vec(delta), str(skip), str(count),
                     '{' + ','.join(bodies) + '}', str(hit), vec(point)]) + '},')
    text = '\n'.join(lines + ['};', '#endif', ''])
    if args.check: assert args.output.read_text(encoding='utf-8') == text
    else: args.output.write_text(text, encoding='utf-8')
    args.trace.write_text(json.dumps([{k: v for k, v in r.items() if k not in ('owners', 'queries')} |
                           {'owner_cases': len(r['owners']), 'actor_queries': len(r['queries'])}
                           for r in runs], indent=2) + '\n', encoding='utf-8')
    print(f"Native contacts: {len(runs[0]['owners'])} owner gates and {len(runs[0]['queries'])} actor queries per region; PAL/US agree")


if __name__ == '__main__':
    main()
