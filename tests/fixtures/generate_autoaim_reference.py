"""Execute PAL/US FindPlayerTarget, including its real actor query and SDK math.

Actor cylinders and camera state are synthetic inputs. PrimaryColl visibility
is an explicit per-ray input; no hull algorithm or hardware capture is claimed.
The initial projected-screen filter executes, including the unconditional
replacement with the full actor list. No original instructions are exported.
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

CLIENT, LIST, SCRATCH, MODEL = 0x801D8000, 0x801D9000, 0x801DA000, 0x801DB000


class AimMachine(TossMachine):
    def intercept(self, pc):
        pal = self.canonical(pc)
        if pal == 0x800544EC:
            self.ray += 1
            self.rays.append([self.vector(self.r[4]), self.vector(self.r[5])])
            return False
        if pal == 0x80089E38:
            return False
        if pal == 0x80044C44:
            self.visibility.append([self.ray, self.vector(self.r[5]),
                                    self.vector(self.r[6]), signed(self.r[7])])
            self.r[2] = int(not (self.blocked & (1 << self.ray)))
            return True
        return super().intercept(pc)


def execute(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
    finally:
        disc.stream.close()
    image = exe[2048:2048 + struct.unpack_from('<I', exe, 0x1C)[0]]
    digest = hashlib.sha256(image).hexdigest()
    assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
    m = AimMachine(image, ntsc)
    assert m.vector(m.address(0x800AC3F8)) == [0, 0, 8192]
    rng = random.Random(0x39DDC)
    cases = []
    # Each elevation, the outer misses, overlap/ray priority, rejected screen
    # projections, blocked rays, null view, arbitrary yaw/pitch/roll and origins.
    for case in range(256):
        angles = [0, 0, 0] if case < 32 else [rng.randrange(-768, 769),
                   rng.randrange(-4096, 4097), rng.randrange(-128, 129)]
        eye = [0, 0, 0] if case < 32 else [rng.randrange(-12000, 12001) for _ in range(3)]
        blocked = (0, 1, 3, 0x7FF)[(case // 16) % 4]
        view = case % 4
        missing_view = case >= 248
        owner = m.address(0x800CBA28)
        window = m.address(0x800D5C30) + view * 784
        m.put(owner, bytes(768 * 5))
        m.put(CLIENT, bytes(256))
        m.put(window, bytes(784))
        m.put(MODEL, bytes(16))
        m.write(CLIENT, 4, 0 if missing_view else window)
        m.write(CLIENT + 0x28, 4, owner)
        for k, value in enumerate([111, -222, 333, 17, -44]):
            m.write(CLIENT + 0x3C + k * 2, 2, value)
        m.put_vector(window, eye)
        m.write(window + 0x92, 2, 7)
        m.write(window + 0x112, 2, 256 if view else 512)
        m.write(m.address(0x800B2D68), 2, 19)
        m.write(m.address(0x800B2B90), 4, LIST)
        m.write(m.address(0x800B2B98), 4, LIST + 5 * 4)
        m.write(m.address(0x800B2DFC), 4, SCRATCH)
        for k, value in enumerate(angles): m.write(owner + 0xE6 + k * 2, 2, value)
        m.call(0x80089E38, owner + 0xE6, owner + 0x2C0)
        targets = []
        for i in range(5):
            actor = owner + i * 768
            m.write(LIST + i * 4, 4, actor)
            m.write(actor + 12, 4, MODEL)  # No posed-model refinement.
            distance = 4096 if case < 32 else rng.randrange(1400, 7500)
            y = ((case % 13) - 6) * 512
            if i > 1: y += rng.randrange(-512, 513)
            # Place synthetic actors along the actual native rotation basis.
            m.put_vector(SCRATCH, [0 if i < 2 else rng.randrange(-300, 301), y, distance])
            m.call(0x8006FD38, owner + 0x2C0, SCRATCH, SCRATCH + 16)
            world = m.vector(SCRATCH + 16)
            world[1] = -world[1]
            position = [eye[k] + world[k] for k in range(3)]
            position[1] -= 236  # 100-high cylinder centred on the ray point.
            radius, height = (80, 100) if i < 2 else (rng.randrange(40, 500), rng.randrange(80, 800))
            if case == 0: position = [eye[0] + 15000, eye[1], eye[2] - 5000]
            m.put_vector(actor + 0x54, position)
            m.write(actor + 0x94, 2, radius)
            m.write(actor + 0x96, 2, height)
            # Every other case fails the original visibility filter. The
            # overwritten list must still let it be acquired.
            m.write(actor + 0x11A + view * 100, 2, 0 if case % 2 else 19)
            m.write(actor + 0x164 + view * 100, 2, 0 if case % 2 else 256)
            targets.append([*position, radius, height])
        m.ray, m.rays, m.visibility, m.blocked = -1, [], [], blocked
        m.call(0x80039DDC, CLIENT)
        result = [signed(m.read(CLIENT + 0x3C + k * 2, 2), 16) for k in range(5)]
        cases.append({'angles': angles, 'eye': eye, 'view': view,
                      'missing_view': int(missing_view), 'blocked': blocked,
                      'actors': targets, 'result': result, 'rays': m.rays,
                      'visibility': m.visibility})
    return {'region': 'US' if ntsc else 'PAL', 'sha256': digest, 'cases': cases}


def vec(values):
    return '{' + ','.join(map(str, values)) + '}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pal', type=Path)
    parser.add_argument('us', type=Path)
    parser.add_argument('--trace', required=True, type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).with_name('autoaim_reference.h'))
    args = parser.parse_args()
    runs = [execute(args.pal, False), execute(args.us, True)]
    assert runs[0]['cases'] == runs[1]['cases'], 'PAL/US autoaim differs'
    lines = ['/* Native PAL/US FindPlayerTarget. See generate_autoaim_reference.py. */',
             '#ifndef Q2_AUTOAIM_REFERENCE_H', '#define Q2_AUTOAIM_REFERENCE_H',
             'typedef struct {',
             '    int angles[3], eye[3], missing_view; unsigned blocked;',
             '    int actors[5][5], result[5], ray_count, visible_count;',
             '    struct { int ray, start[3], end[3], hint; } visibility[11];',
             '} native_autoaim_case;', 'static const native_autoaim_case native_autoaim[] = {']
    for c in runs[0]['cases']:
        vis = ['{' + ','.join((str(v[0]), vec(v[1]), vec(v[2]), str(v[3]))) + '}' for v in c['visibility']]
        lines.append('    {' + ','.join((vec(c['angles']), vec(c['eye']), str(c['missing_view']),
                     str(c['blocked']) + 'u', '{' + ','.join(vec(a) for a in c['actors']) + '}',
                     vec(c['result']), str(len(c['rays'])), str(len(vis)),
                     '{' + ','.join(vis or ['{0}']) + '}')) + '},')
    lines += ['};', '#endif', '']
    args.output.write_text('\n'.join(lines), encoding='utf8')
    args.trace.write_text(json.dumps(runs, indent=2) + '\n', encoding='utf8')
    print(f'PAL/US: {len(runs[0]["cases"])} identical autoaim cases per region')


if __name__ == '__main__':
    main()
