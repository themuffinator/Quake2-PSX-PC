"""Execute retail actor allocation, spawn and ambient-fade instructions.

Only numeric results and trace metadata are exported, never disc code/assets.
The player cases stop after the native colour stores, before later placement.
Creature cases execute the shared spawner and each distinct module's export 0;
geometry, sound and model lookup hooks are recorded explicitly in the trace.
This is a bounded initialization comparison, not a whole-machine emulator.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from generate_heap_reference import LoaderMachine, Form1Disc, NTSC_SEGMENT_SHA
from generate_resource_reference import SEGMENT_SHA

BODY = 0x801E0000
EDICT = 0x801E1000
MODULE = 0x80100000
MODEL = 0x801D0000
CLASS = 0x801D1000
PLACEMENT = 0x801D2000
TRAP = 0x801F8000
# Native module ABI slots used by initialization. Unsupported calls trap.
IMPORTS = {0x14: 0x80089E28, 0x18: 0x8008A4D8, 0x1C: 0x8005C8C8,
           0x20: 0x8007270C, 0x24: 0x80073518, 0x34: 0x8006CA2C,
           0x3C: 0x80073AA4, 0x44: 0x80073A34, 0x48: 0x800E46D8,
           0x4C: 0x800AECF8, 0x50: 0x800CBA28, 0x74: 0x8006D330,
           0xD4: 0x8005C7B8,
           0xFC: 0x80062240, 0x100: 0x8006229C, 0x104: 0x80062268,
           0x118: 0x80061DE4}


def chunks(data):
    if len(data) < 16:
        return {}
    end = struct.unpack_from('<I', data, 12)[0]
    if end < 32 or end > 1024 or end % 16:
        return {}
    result = {}
    for at in range(0, end - 16, 16):
        name = data[at:at + 12].split(b'\0')[0].decode('ascii')
        start, stop = (struct.unpack_from('<I', data, x)[0] for x in (at + 12, at + 28))
        assert end <= start <= stop <= len(data)
        result[name] = data[start:stop]
    return result


def modules(disc):
    seen = {}
    for path in sorted(disc.files):
        if not path.startswith('/Q2DATA/LEVELS/') or not path.endswith('.DAT'):
            continue
        archive = chunks(disc.read(path))
        code, rel = archive.get('CreAIBin', b''), archive.get('CreAIRel', b'')
        a = b = 0
        while a + 16 < len(code) and b + 16 < len(rel):
            name = code[a:a + 12].split(b'\0')[0].decode('ascii')
            an, bn = struct.unpack_from('<I', code, a + 12)[0], struct.unpack_from('<I', rel, b + 12)[0]
            body, fixups = code[a + 16:an or len(code)], rel[b + 16:bn or len(rel)]
            key = (name, hashlib.sha256(body + fixups).hexdigest())
            if key not in seen:
                seen[key] = {'name': name, 'sha256': key[1], 'paths': [], 'body': body, 'fixups': fixups}
            seen[key]['paths'].append(path)
            if not an or not bn:
                assert an == bn == 0
                break
            assert an > a + 16 and bn > b + 16
            a, b = an, bn
    return list(seen.values())


def relocate(body, fixups):
    image = bytearray(body)
    at = 0
    while at + 4 <= len(fixups):
        entry = struct.unpack_from('<I', fixups, at)[0]
        at += 4
        if entry == 0xFFFFFFFF:
            return image
        offset, kind = entry & ~3, entry & 3
        value = struct.unpack_from('<I', image, offset)[0]
        if kind == 0:
            value += MODULE
        elif kind == 1:
            addend = struct.unpack_from('<I', fixups, at)[0]
            at += 4
            value = (value & 0xFFFF0000) | (((addend + MODULE + 0x8000) >> 16) & 0xFFFF)
        elif kind == 2:
            value = (value & 0xFFFF0000) | ((value + MODULE) & 0xFFFF)
        else:
            value = (value & 0xFC000000) | ((((value & 0x3FFFFFF) << 2) + MODULE) >> 2 & 0x3FFFFFF)
        struct.pack_into('<I', image, offset, value & 0xFFFFFFFF)
    raise ValueError('Unterminated relocation stream')


class ActorMachine(LoaderMachine):
    def __init__(self, image, ntsc):
        super().__init__(image, None, ntsc)
        self.ambient_writes = []
        self.spawn_hooks = {}
        self.methods = []
        self.write(self.address(0x800B2BAC), 4, self.address(0x800D4A28))
        self.write(self.address(0x800D4A28), 4, BODY)

    def colour(self):
        return [self.read(BODY + offset, 1) for offset in range(0x2AC, 0x2B4)]

    def write(self, address, size, value):
        super().write(address, size, value)
        if BODY + 0x2AC <= address < BODY + 0x2B4:
            self.ambient_writes.append({'pc': f'{self.last_pc:08x}', 'offset': address - BODY,
                                        'size': size, 'value': value & ((1 << (size * 8)) - 1)})

    def intercept(self, pc):
        self.last_pc = pc
        if TRAP <= pc < TRAP + 0x130:
            raise ValueError(f'Unsupported module import +{pc - TRAP:x}')
        canonical = self.canonical(pc)
        if canonical == 0x8005F898:  # AI-edict allocation, separate from physical entity.
            self.put(EDICT, bytes(512))
            self.r[2] = EDICT
        elif canonical == 0x8006DC1C:  # GetCreHitSize: model geometry only.
            self.put(self.r[4], struct.pack('<HH', 286, 600))
        elif canonical == 0x80050FA0:  # Successful SetupBoxDrop with neutral position.
            self.put(self.r[4], bytes(12))
            self.r[2] = 1
        elif canonical == 0x80078978:  # Blob shadow registration.
            self.r[2] = 0
        elif canonical == 0x8006D330:  # Valid neutral animation record.
            self.r[2] = MODEL + 128
        elif canonical in (0x80073518, 0x80073AA4):  # Valid sound request/clone.
            self.r[2] = MODEL + 256
        elif canonical in (0x80073A34, 0x8007270C):  # Sound parameters/playback.
            self.r[2] = 0
        elif canonical == 0x8006CA2C:  # Model attachment initialization succeeds.
            self.r[2] = 1
        elif canonical == 0x80061DE4:  # Class-method registration, no entity mutation.
            self.methods.append(self.r[4:6])
        else:
            return super().intercept(pc)
        key = f'{pc:08x}'
        self.spawn_hooks[key] = self.spawn_hooks.get(key, 0) + 1
        return True


def creature_spawns(image, disc, ntsc):
    results = []
    for module in modules(disc):
        m = ActorMachine(image, ntsc)
        m.put(MODULE, relocate(module['body'], module['fixups']))
        m.write(MODULE + 16, 4, 0x00010130)
        for slot in range(0x14, 0x130, 4):
            m.write(MODULE + slot, 4, m.address(IMPORTS[slot]) if slot in IMPORTS else TRAP + slot)
        for slot in range(3):
            m.write(m.address(0x800AECF8) + 4 * slot, 4, MODEL + 512 + 64 * slot)
        m.write(m.address(0x800B2FD0), 4, MODEL + 1024)
        m.registers()
        stack = m.r[29]
        m.r[4] = MODEL
        try:
            m.run(m.read(MODULE + 8, 4), limit=100000)
            assert m.r[29] == stack
            assert m.call(0x8006C098, 1) == BODY
            m.ambient_writes.clear()
            m.write(CLASS + 0x24, 4, m.read(MODULE, 4))
            assert m.call(0x8007E440, BODY, PLACEMENT, CLASS, 0) == 1
        except Exception as error:
            raise ValueError(f"{module['name']} ({module['paths'][0]}) at {m.last_pc:08x}: {error}") from error
        results.append({k: v for k, v in module.items() if k not in ('body', 'fixups')})
        results[-1].update(ambient=m.colour(), ambient_writes=m.ambient_writes,
                           hooks=m.hooks, spawn_hooks=m.spawn_hooks, class_methods=m.methods)
    return results


def region(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
        image = exe[2048:2048 + struct.unpack_from('<I', exe, 28)[0]]
        digest = hashlib.sha256(image).hexdigest()
        assert digest == (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA)
        m = ActorMachine(image, ntsc)
        assert m.call(0x8006C098, 1) == BODY
        allocation = m.colour()
        players = []
        for mode in range(-1, 6):
            for owner in range(1 if mode < 0 else 4):
                m = ActorMachine(image, ntsc)
                m.write(m.address(0x800AEBCC), 4, int(mode >= 0))
                m.write(m.address(0x800B2C2C), 4, 1 if mode < 0 else 4)
                m.write(m.address(0x800B3354), 2, max(mode, 0))
                m.registers()
                m.r[4], m.r[5] = owner, 0
                m.run(m.address(0x8003B250), stop=m.address(0x8003B474), limit=10000)
                players.append({'mode': mode, 'owner': owner, 'ambient': m.colour(), 'hooks': m.hooks})
        player = players[0]['ambient']
        assert all(p['ambient'] == player for p in players)
        fades = []
        for target in (allocation[0], player[0]):
            m = ActorMachine(image, ntsc)
            values = []
            for current in range(256):
                m.put(BODY + 0x2AC, bytes([current] * 3 + [0, target, target, target, 0]))
                m.call(0x80075E14, BODY, 7)
                colour = m.colour()
                assert colour[0] == colour[1] == colour[2]
                assert colour[4:7] == [target] * 3
                values.append(colour[0])
            fades.append(values)
        creatures = creature_spawns(image, disc, ntsc)
        assert creatures and all(c['ambient'] == allocation and not c['ambient_writes'] for c in creatures)
        return {'region': 'US' if ntsc else 'PAL', 'executable_sha256': digest,
                'creatures': creatures,
                'allocation': allocation, 'players': players, 'fades': fades}
    finally:
        disc.stream.close()


def header(regions):
    assert regions[0]['allocation'] == regions[1]['allocation']
    assert regions[0]['players'][0]['ambient'] == regions[1]['players'][0]['ambient']
    assert regions[0]['fades'] == regions[1]['fades']
    r = regions[0]
    lines = ['/* Generated by generate_actor_light_reference.py from PAL and US instructions. */',
             '#ifndef Q2_ACTOR_LIGHT_REFERENCE_H', '#define Q2_ACTOR_LIGHT_REFERENCE_H',
             f"static const unsigned char native_actor_ambient = {r['allocation'][0]};",
             f"static const unsigned char native_player_ambient = {r['players'][0]['ambient'][0]};",
             'static const unsigned char native_actor_fade[2][256] = {']
    for row in r['fades']:
        lines.append('    {')
        for i in range(0, 256, 16):
            lines.append('        ' + ', '.join(str(v) for v in row[i:i + 16]) + ',')
        lines.append('    },')
    lines.extend(['};', '#endif', ''])
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pal', type=Path)
    parser.add_argument('us', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).with_name('actor_light_reference.h'))
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    rows = [region(args.pal, False), region(args.us, True)]
    result = header(rows)
    if args.check:
        assert args.output.read_text(encoding='utf-8') == result
    else:
        args.output.write_text(result, encoding='utf-8')
    args.trace.write_text(json.dumps(rows, indent=2) + '\n', encoding='utf-8')
    print(f"Native actor lighting: {sum(len(r['players']) for r in rows)} player spawns, "
          f"{sum(len(r['creatures']) for r in rows)} distinct creature modules, "
          "1024 fade calls; PAL/US agree")


if __name__ == '__main__':
    main()
