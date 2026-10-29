"""Execute the PAL/US bolt sweep with explicit collision/output services.

The complete sweep and blood spawner execute retail instructions. Actor
queries also execute unchanged. Entity contacts and the cached hull marcher
are supplied inputs; rendering, damage, sound and particle allocation are
recording hooks. This qualifies control flow and effect/RNG arguments, not
hardware rendering or the cached hull algorithm. No retail code is exported.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from generate_combat_reference import signed
from generate_heap_reference import Form1Disc, NTSC_SEGMENT_SHA
from generate_resource_reference import SEGMENT_SHA
from generate_toss_reference import TossMachine

ACTOR, OBJECT, LIST, GROUPS = 0x801D0000, 0x801D0400, 0x801D0800, 0x801D1000


def motion_hash(velocities, offsets):
    value = 2166136261
    for particle, velocity in enumerate(velocities):
        for component in velocity + offsets[particle]:
            for byte in (component & 255, (component >> 8) & 255):
                value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


class BoltMachine(TossMachine):
    def intercept(self, pc):
        pal = self.canonical(pc)
        if pal == 0x800544EC:
            self.sequence.append('actors')
            return False  # Full native query, root and clipping arithmetic.
        if pal in (0x8008A178, 0x8008A1A8, 0x8008A118):
            pass  # View/GTE setup; explicit zero views skip body submission.
        elif pal == 0x80053974:
            self.sequence.append('entities')
            start, end = self.vector(self.r[4]), self.vector(self.r[5])
            hit = self.mode == 2 and start[0] < 400 <= end[0]
            if hit:
                self.put_vector(self.r[6], [400, 0, 0])
                self.write(self.address(0x800CBA10) + 18, 2, 2)
            self.r[2] = 0 if hit else 1
        elif pal == 0x80043BDC:
            self.sequence.append('hull')
            if self.mode == 3:
                self.put_vector(self.r[6], [600, 0, 0])
            self.r[2] = 0 if self.mode == 3 else 1
        elif pal == 0x80057D54:
            self.sequence.append('damage')
            self.damage += 1
        elif pal in (0x8007F15C, 0x800435D0):
            self.sequence.append('noise' if pal == 0x8007F15C else 'entity-response')
        elif pal == 0x80075C34:
            self.sequence.append('light')
            self.lights.append([self.vector(self.r[4]), self.r[5], self.r[6], self.r[7]])
        elif pal in (0x8003004C, 0x80030284):
            trail = pal == 0x8003004C
            self.sequence.append('trail' if trail else 'burst')
            stack = self.r[29]
            count = self.read(stack + (20 if trail else 16), 4)
            velocities = self.r[6] if trail else self.r[5]
            vel = [[signed(self.read(velocities + i * 6 + k * 2, 2), 16)
                    for k in range(3)] for i in range(count)]
            offs = [[0, 0, 0] for _ in range(count)]
            if trail:
                for i in range(1, count):
                    offs[i] = [signed(self.read(self.r[5] + i * 6 + k * 2, 2), 16) >> 4
                               for k in range(3)]
            ramp0 = self.r[7] if trail else self.r[6]
            ramp1 = self.read(stack + 16, 4) if trail else self.r[7]
            row = {'origin': self.vector(self.r[4]), 'count': count,
                   'life': self.read(stack + (24 if trail else 20), 4),
                   'size': self.read(stack + (28 if trail else 24), 4),
                   'ramps': [(self.canonical(r) - 0x8009BA60) // 132 for r in (ramp0, ramp1)],
                   'velocity': vel[0], 'motion_hash': motion_hash(vel, offs)}
            address = GROUPS + len(self.groups) * 256
            self.put(address, bytes(256))
            self.groups.append(row)
            self.r[2] = address
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
    m = BoltMachine(image, ntsc)
    rows = []
    inputs = [(flags, mode, remaining, 12, [0, 0, 0], [100, 0, 0])
              for flags in (11, 14, 0, 1, 2, 8, 16, 31)
              for mode in range(4) for remaining in (11, 12, 13, 2560)]
    for i in range(64):
        inputs.append((11 if i % 2 else 14, 0, 2560, (1, 6, 12, 20, 30)[i % 5],
                       [12345, -9876, 4321],
                       [(-1, 3, -5), (-128, 0, 64), (2731, -2731, 33), (2000, -2000, 1)][i % 4]))
    for flags, mode, remaining, dt, start, velocity in inputs:
        m.mode = mode
        m.sequence, m.lights, m.groups, m.damage = [], [], [], 0
        pool = m.address(0x800C91C0)
        m.put(pool, bytes(64 * 104))
        m.write(pool + 0x1A, 2, remaining)
        m.write(pool + 0x22, 2, flags)
        m.write(pool + 0x3A, 2, 1)
        m.write(pool + 0x4A, 2, 6)
        m.put_vector(pool, start)
        for k, component in enumerate(velocity): m.write(pool + 0x52 + k * 2, 2, component)
        m.write(pool + 0x58, 2, 0)
        m.write(m.address(0x800B2DB4), 4, dt)
        m.write(m.address(0x800B2C2C), 4, 0)
        m.put(ACTOR, bytes(1024))
        m.put(OBJECT, bytes(4))
        m.write(ACTOR + 12, 4, OBJECT)  # No posed-model refinement.
        m.put_vector(ACTOR + 0x54, [600, 0, 0])
        m.write(ACTOR + 0x94, 2, 50)
        m.write(ACTOR + 0x96, 2, 572)
        m.write(LIST, 4, ACTOR)
        m.write(m.address(0x800B2B90), 4, LIST)
        m.write(m.address(0x800B2B98), 4, LIST + (4 if mode in (1, 2) else 0))
        seed = (0x12345678 + len(rows) * 12345) & 0xFFFFFFFF
        m.rng_state = seed
        m.call(0x80047C6C)
        for i, group in enumerate(m.groups):
            group['accel_y'] = signed(m.read(GROUPS + i * 256 + 98, 2), 16)
        rows.append({'flags': flags, 'mode': mode, 'remaining': remaining,
                     'dt': dt, 'start': start, 'velocity': list(velocity),
                     'seed': seed, 'position': m.vector(pool),
                     'live': int(m.read(pool + 0x1A, 2) != 0),
                     'rng': m.rng_state, 'damage': m.damage,
                     'sequence': m.sequence, 'lights': m.lights, 'groups': m.groups})
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest,
            'cases': rows, 'hooks': m.hooks}


def vec(values):
    return '{' + ','.join(map(str, values)) + '}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pal', type=Path)
    parser.add_argument('us', type=Path)
    parser.add_argument('--trace', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).with_name('bolt_reference.h'))
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    runs = [execute(args.pal, False), execute(args.us, True)]
    assert runs[0]['cases'] == runs[1]['cases'], 'PAL/US bolt sequences differ'
    lines = ['/* Full PAL/US bolt sweep; explicit services in generate_bolt_reference.py. */',
             '#ifndef Q2_BOLT_REFERENCE_H', '#define Q2_BOLT_REFERENCE_H',
             'typedef struct {',
             '    int flags, mode, remaining, dt, start[3], velocity[3]; unsigned seed;',
             '    int position[3], live; unsigned rng; int damage, light_count;',
             '    struct { int position[3]; unsigned colour, radii, decay; } lights[2];',
             '    int group_count;',
             '    struct { int origin[3], count, life, size, ramps[2], velocity[3];',
             '             unsigned motion_hash; int accel_y; } groups[3];',
             '} native_bolt_case;',
             'static const native_bolt_case native_bolts[] = {']
    for r in runs[0]['cases']:
        lights = ['{' + ','.join([vec(l[0]), *(str(v) + 'u' for v in l[1:])]) + '}' for l in r['lights']]
        groups = ['{' + ','.join([vec(g['origin']), str(g['count']), str(g['life']), str(g['size']),
                  vec(g['ramps']), vec(g['velocity']), str(g['motion_hash']) + 'u', str(g['accel_y'])]) + '}'
                  for g in r['groups']]
        lines.append('    {' + ','.join([str(r['flags']), str(r['mode']), str(r['remaining']),
                     str(r['dt']), vec(r['start']), vec(r['velocity']),
                     str(r['seed']) + 'u', vec(r['position']), str(r['live']), str(r['rng']) + 'u',
                     str(r['damage']), str(len(lights)), '{' + (','.join(lights) or '0') + '}',
                     str(len(groups)), '{' + (','.join(groups) or '0') + '}']) + '},')
    text = '\n'.join(lines + ['};', '#endif', ''])
    if args.check:
        assert args.output.read_text(encoding='utf-8') == text
    else:
        args.output.write_text(text, encoding='utf-8')
    args.trace.write_text(json.dumps(runs, indent=2) + '\n', encoding='utf-8')
    print(f"Native bolt sequences: {len(runs[0]['cases'])} per region; PAL/US agree")


if __name__ == '__main__':
    main()
