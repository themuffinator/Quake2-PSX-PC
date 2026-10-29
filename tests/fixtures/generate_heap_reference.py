"""Trace PAL/NTSC loader allocations using the user's disc and native instructions.

This is an allocation investigation, not a general console emulator. Disc I/O
and GPU transfers use explicit hooks; the native heap allocator, file rounding,
shrink/move operations and archive consumers execute in isolated R3000 RAM.
The JSON records hooks and allocation snapshots so its evidence scope is visible.
No retail instructions or file contents are written to the output.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from generate_resource_reference import ResourceMachine, SEGMENT_SHA

NTSC_SEGMENT_SHA = '8f61c897173a4109532191c4f25a00bd198d6f3a4d4d6bdcfd9e1204f768a214'
# Verified executable correspondence, also checked against the US MAIN.SYM.
# This selects hook/global addresses; the US instructions and assets execute
# unchanged. In particular, physical RAM/stack bounds are not relocated.
NTSC_LAYOUT = (
    (0x80018000, 0x8001FA30, 0), (0x8001FA40, 0x8006FA2C, -4),
    (0x8006FA2C, 0x800701B4, 0x21C), (0x800701B4, 0x800701B8, -0x78C),
    (0x800702A0, 0x800718B4, 0x130), (0x800718BC, 0x80082620, 0x134),
    (0x800826A4, 0x800ABFA4, 0x254), (0x800ABFD8, 0x800AC2C4, 0x250),
    (0x800AC2C4, 0x800AD93C, 0x254), (0x800AD93C, 0x80200000, 0x270))


class Form1Disc:
    """Read-only ISO9660 access to a MODE2/2352 data track."""
    def __init__(self, cue):
        source = cue.read_text(encoding='utf-8-sig')
        match = re.search(r'FILE\s+"([^"]+)"\s+BINARY\s+TRACK\s+01\s+MODE2/2352\s+INDEX\s+01\s+00:00:00', source, re.I)
        if not match:
            raise ValueError('Expected track 1 MODE2/2352 with INDEX 01 at 00:00:00')
        self.stream = (cue.parent / match[1]).open('rb')
        self.files = {}
        pvd = self.sectors(16, 2048)
        if pvd[:7] != b'\x01CD001\x01':
            raise ValueError('Missing primary ISO9660 descriptor')
        self.walk('', *struct.unpack_from('<I4xI', pvd, 158))

    def sectors(self, lba, length):
        self.stream.seek(lba * 2352)
        raw = self.stream.read(((length + 2047) // 2048) * 2352)
        result = bytearray()
        for at in range(0, len(raw), 2352):
            sector = raw[at:at + 2352]
            if len(sector) != 2352 or sector[15] != 2 or sector[18] & 0x20:
                raise ValueError(f'Expected Form 1 sector {lba + at // 2352}')
            result.extend(sector[24:2072])
        if len(result) < length:
            raise ValueError('Truncated disc read')
        return bytes(result[:length])

    def walk(self, prefix, lba, length):
        data, at = self.sectors(lba, length), 0
        while at < len(data):
            count = data[at]
            if not count:
                at = (at // 2048 + 1) * 2048
                continue
            row = data[at:at + count]
            if len(row) < 34:
                raise ValueError('Truncated ISO directory record')
            name = row[33:33 + row[32]]
            if name not in (b'\0', b'\1'):
                path = prefix + '/' + name.decode('ascii').split(';')[0].upper()
                child_lba, size = struct.unpack_from('<I4xI', row, 2)
                if row[25] & 2:
                    self.walk(path, child_lba, size)
                else:
                    self.files[path] = (child_lba, size)
            at += count

    def info(self, name):
        return self.files[name.replace('\\', '/').upper().split(';')[0]]

    def read(self, name, padded=False):
        lba, size = self.info(name)
        return self.sectors(lba, (size + 2047) & ~2047 if padded else size)


class LoaderMachine(ResourceMachine):
    def __init__(self, image, disc, ntsc=False):
        self.ntsc = ntsc
        super().__init__(image, self.address(0x800B26D0) & 0x1FFFFFFF,
                         self.address(0x800EE718) & 0x1FFFFFFF,
                         self.address(0x800AE600))
        self.disc = disc
        self.events = []
        self.hooks = {}
        self.pending_calls = []
        self.phase = 'initial'
        self.spu_registers = bytearray(512)
        self.spu_writes = 0
        self.rng_state = 1
        self.write(self.address(0x800AE8D0), 4, 1)  # Retail CD path, not the development host.

    def address(self, pal):
        if not self.ntsc:
            return pal
        for begin, end, delta in NTSC_LAYOUT:
            if begin <= pal < end:
                return pal + delta
        raise ValueError(f'No verified NTSC address for {pal:08x}')

    def canonical(self, address):
        if not self.ntsc:
            return address
        for begin, end, delta in NTSC_LAYOUT:
            if begin + delta <= address < end + delta:
                return address - delta
        return None  # Changed code without a PAL equivalent has no hooks.

    def string(self, address):
        start = address & 0x1FFFFFFF
        end = self.mem.find(0, start, start + 256)
        if end < 0:
            raise ValueError(f'Unterminated string at {address:08x}')
        return bytes(self.mem[start:end]).decode('ascii')

    def read(self, address, size, sign=False):
        physical = address & 0x1FFFFFFF
        if 0x1F801C00 <= physical and physical + size <= 0x1F801E00:
            at = physical - 0x1F801C00
            return int.from_bytes(self.spu_registers[at:at + size], 'little', signed=sign) & 0xFFFFFFFF
        return super().read(address, size, sign)

    def write(self, address, size, value):
        physical = address & 0x1FFFFFFF
        if 0x1F801C00 <= physical and physical + size <= 0x1F801E00:
            at = physical - 0x1F801C00
            self.spu_registers[at:at + size] = (value & ((1 << (8 * size)) - 1)).to_bytes(size, 'little')
            self.spu_writes += 1
            return
        super().write(address, size, value)

    def put(self, address, data):
        address &= 0x1FFFFFFF
        if address + len(data) > len(self.mem):
            raise ValueError('Write beyond native RAM')
        self.mem[address:address + len(data)] = data

    def snapshot(self):
        rows, node = [], self.read(self.address(0x800B2D58), 4)
        while node:
            if len(rows) >= 128 or (rows and node <= rows[-1]['header']):
                raise ValueError('Invalid heap allocation list')
            tag_at = (node & 0x1FFFFFFF) + 12
            tag = bytes(self.mem[tag_at:tag_at + 16]).split(b'\0')[0].decode('ascii', errors='backslashreplace')
            rows.append({'header': node, 'size': self.read(node + 8, 4), 'tag': tag})
            node = self.read(node + 4, 4)
        return rows

    def intercept(self, pc):
        self.last_pc = pc
        if pc < 0x80018000:
            raise ValueError(f'Unhooked BIOS call {self.r[9]:02x}, return {self.r[31]:08x}')
        if self.pending_calls and pc == self.pending_calls[-1]['return']:
            call = self.pending_calls.pop()
            call.update(result=self.r[2], allocations=self.snapshot())
            self.events.append(call)
        native_pc, pc = pc, self.canonical(pc)
        # Full-map unload contains two inlined frees rather than calls.
        if pc in (0x8006E7FC, 0x8006E82C):
            self.pending_calls.append({'operation': 'free', 'pc': native_pc,
                'phase': self.phase, 'return': native_pc + 32,
                'args': [0, 0, 0], 'payload': self.r[3]})
        operations = {0x8006E468: 'malloc', 0x8006E5A8: 'realloc',
                      0x8006E860: 'free', 0x8006E890: 'free', 0x8006EA48: 'move_header',
                      0x8006EACC: 'largest_free'}
        if pc in operations:
            event = {'operation': operations[pc], 'pc': native_pc, 'phase': self.phase,
                     'return': self.r[31], 'args': self.r[4:7]}
            if operations[pc] == 'free':
                event['payload'] = self.read(self.r[4], 4)
            self.pending_calls.append(event)
        if pc == 0x800763D4:  # CdSearchFile wrapper; no heap allocation.
            self.r[2] = self.disc.info(self.string(self.r[4]))[1]
        elif pc == 0x8007641C:  # Synchronous CD file read; no heap allocation.
            self.put(self.r[5], self.disc.read(self.string(self.r[4]), padded=True))
            self.r[2] = 1
        elif pc == 0x80089E18:  # memset.
            self.put(self.r[4], bytes([self.r[5] & 255]) * self.r[6])
            self.r[2] = self.r[4]
        elif pc in (0x8008A0F8, 0x8008A208):  # BIOS strcpy / strcat.
            offset = len(self.string(self.r[4])) if pc == 0x8008A208 else 0
            self.put(self.r[4] + offset, self.string(self.r[5]).encode('ascii') + b'\0')
            self.r[2] = self.r[4]
        elif pc == 0x8008AE1C:  # ASCII toupper for disc filenames.
            value = self.r[4]
            self.r[2] = value - 32 if 97 <= value <= 122 else value
        elif pc == 0x80089D48:  # BIOS strcmp; all loader names are ASCII.
            a, b = self.string(self.r[4]) + '\0', self.string(self.r[5]) + '\0'
            self.r[2] = next((ord(x) - ord(y) for x, y in zip(a, b) if x != y), 0) & 0xFFFFFFFF
        elif pc == 0x8008A0E8:  # BIOS strlen.
            self.r[2] = len(self.string(self.r[4]))
        elif pc == 0x8008AA68 and self.r[31] == self.address(0x8006E9AC):
            # malloc's debug strstr(tag, ".opt") result is immediately discarded.
            self.r[2] = 0
        elif pc in (0x8008AD9C, 0x8008ADAC):  # memcpy / overlapping backward move.
            source = self.r[5] & 0x1FFFFFFF
            if source + self.r[6] > len(self.mem):
                raise ValueError('Read beyond native RAM')
            self.put(self.r[4], bytes(self.mem[source:source + self.r[6]]))
            self.r[2] = self.r[4]
        elif pc in (0x80083648, 0x8008339C):  # GPU upload / wait.
            self.r[2] = 0
        elif pc in (0x80082930, 0x80082940, 0x80082950):  # Critical section / cache.
            self.r[2] = 1
        elif pc == 0x8008A4D8:  # BIOS printf in creature initialization.
            self.r[2] = 0
        elif pc == 0x80089E28:  # BIOS A(2Fh), seeded explicitly for this trace.
            self.rng_state = (self.rng_state * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
            self.r[2] = (self.rng_state >> 16) & 0x7FFF
        elif pc == 0x800724EC:  # Sample upload uses SPU RAM, not the main heap.
            self.r[2] = 0
        else:
            return super().intercept(native_pc)
        key = f'{native_pc:08x}'
        self.hooks[key] = self.hooks.get(key, 0) + 1
        return True

    def call(self, pc, *args):
        pc = self.address(pc)
        self.registers()
        stack = self.r[29]
        self.r[4:4 + len(args)] = args
        self.run(pc, limit=10000000)
        if self.r[29] != stack:
            raise ValueError(f'Native call {pc:08x} stopped with an unbalanced stack')
        # A top-level native function returns to the zero stop address.
        if self.pending_calls and self.pending_calls[-1]['return'] == 0:
            call = self.pending_calls.pop()
            call.update(result=self.r[2], allocations=self.snapshot())
            self.events.append(call)
        return self.r[2]


def configure_level(m, name, views, multiplayer):
    m.put(m.address(0x800DDDA8), f'\\Q2DATA\\LEVELS\\{name.upper()}\\'.encode('ascii') + b'\0')
    table = m.address(0x8009C6C8)
    row = next(table + 56 * i for i in range(52)
               if m.string(table + 56 * i + 12) == name.upper())
    m.write(m.address(0x800B2F90), 4, row)
    m.write(m.address(0x800B2F94), 4, table)
    m.put(m.address(0x800E46B4), m.mem[(row & 0x1FFFFFFF):(row & 0x1FFFFFFF) + 12])
    m.put(m.address(0x800C8CD0), b'Default\0\0\0\0\0')
    m.write(m.address(0x800B2C2C), 4, views)
    m.write(m.address(0x800AEBCC), 4, int(multiplayer))
    m.write(m.address(0x800B3356), 2, views if multiplayer else 0)
    m.write(m.address(0x800B2DA0), 2, 512)
    m.write(m.address(0x800B2DA2), 2, 240 if m.ntsc else 248)
    m.write(m.address(0x800B2D3C), 1, 1)  # Loading animation has reached its fade boundary.
    return row


def load_state(m):
    resource = m.read(m.address(0x800B2F8C), 4)
    return {'phase': m.phase,
        'zone': m.string(resource),
        'packet_control': m.read(resource + 12, 4),
        'particle_groups': m.read(resource + 24, 4),
        'requested_meshes': m.read(resource + 28, 4),
        'packet_capacity': m.read(m.address(0x800B2D98), 2),
        'subdiv_capacity': m.read(m.address(0x800B2CE0), 4),
        'heap': m.snapshot()}


def select_mission(m, row, mission=None, multiplayer=False):
    """Execute the native dispatcher, including its direct-level table lookup.

    A multiplayer restart uses GameReRunMPLevel, which selects the null mission
    (7), not Tribute (6). An explicit zero means no record; None uses the
    selected level's actual +0x16 mission id. Intermission callers can pass the
    departing campaign id, retaining that session rather than a direct launch.
    """
    name = bytes(m.mem[(row & 0x1FFFFFFF):(row & 0x1FFFFFFF) + 12])
    m.put(m.address(0x800E46CC), name)
    m.put(m.address(0x800E46C0), name)
    m.write(m.address(0x800B2D7C), 4, 0 if mission is None else mission or 7)
    m.write(m.address(0x800B2E1C), 4, 0)
    if multiplayer:
        m.call(0x80041958)  # GameReRunMPLevel: NextMission=7, NewMission=0.
    m.call(0x8003F37C)     # GameNewMission, including the actual unit initializer.
    pointer = m.read(m.address(0x800B2A30), 4)
    selected = m.read(m.address(0x800B2E20), 4)
    if pointer and not 1 <= selected <= 6:
        raise ValueError('Unexpected native mission allocation')
    return selected if pointer else 0


def run_case(image, disc, name, mission=None, views=1, multiplayer=False, streams=(), reload=False, ntsc=False):
    m = LoaderMachine(image, disc, ntsc)
    m.call(0x8006E8C0)
    row = configure_level(m, name, views, multiplayer)
    error, phases, unloaded = None, [], None

    def record_phase():
        phases.append(load_state(m))

    try:
        mission = select_mission(m, row, mission, multiplayer)
        m.call(0x80070C94)
        m.call(0x80078A88)
        m.call(0x8007146C)
        record_phase()
        for ordinal, zone in enumerate(streams):
            if not 0 <= zone <= 9:
                raise ValueError('Only the native single-digit zone names are supported')
            m.phase = f'stream-{ordinal + 1}-zone-{zone}'
            packed = struct.unpack('<III', f'Zone{zone}'.encode('ascii').ljust(12, b'\0'))
            m.call(0x800793B8, *packed)
            record_phase()
        if reload:
            m.phase = 'full-unload'
            m.call(0x8006E7D0)
            unloaded = m.snapshot()
            m.phase = 'full-reload'
            # TestIt's loading-screen setup clears the old image-list alias
            # before LoadLevel (0x80018C88, store at 0x8006E25C).
            m.write(m.address(0x800B2D14), 4, 0)
            m.call(0x80070C94)
            m.call(0x80078A88)
            m.call(0x8007146C)
            record_phase()
    except Exception as exc:
        error = f'{exc} at {m.last_pc:08x}; registers {m.r}'
    largest = [e['result'] for e in m.events if e['operation'] == 'largest_free']
    print(f'{name} ({views} views, mission {mission}): {len(m.events)} operations; '
          f'largest {largest}; packets {[p["packet_capacity"] for p in phases]}; '
          f'{error or "PASS"}', flush=True)
    return {'map': name, 'mission': mission, 'views': views, 'multiplayer': multiplayer, 'ntsc': ntsc,
        'level_kind': m.read(row + 32, 1), 'phases': phases,
        'hooks': m.hooks, 'rng_seed': 1, 'spu_register_writes': m.spu_writes,
        'events': m.events, 'heap': m.snapshot(), 'unloaded_heap': unloaded, 'error': error}


def run_session(image, disc, ntsc=False):
    """Selected sessions, preserving native RAM between full loads.

    The outer caller unloads at 0x80018820, then replaces the mission at
    0x8003F528/0x8003F7F4..84C, before loading the selected directory. This
    executes the dispatcher and its direct-level lookup. Intermission/movie
    loads explicitly retain the departing campaign id; this is not execution
    of the entire outer campaign state machine.
    """
    route = [('QFRONT', 1, False), ('BASE1', 1, False),
             ('BASE2', 1, False), ('QENDMIS1', 1, False),
             ('JAIL2', 1, False), ('QENDMIS2', 1, False),
             ('POWER1', 1, False), ('QENDMIS3', 1, False),
             ('LAB', 1, False), ('QENDMIS4', 1, False),
             ('BOSS1', 1, False), ('QENDMIS5', 1, False),
             ('QFMV', 1, False), ('QFRONT', 1, False),
             ('MATRIX6', 2, True), ('MATRIX6', 4, True),
             ('QFRONT', 1, False)]
    m = LoaderMachine(image, disc, ntsc)
    m.call(0x8006E8C0)
    phases, error = [], None
    try:
        mission = 0
        for ordinal, (name, views, multiplayer) in enumerate(route):
            m.phase = f'load-{ordinal + 1}-{name}'
            if ordinal:
                m.call(0x8006E7D0)
            row = configure_level(m, name, views, multiplayer)
            retained = mission if (name.startswith('QENDMIS') or name == 'QFMV') and 1 <= mission <= 5 else None
            mission = select_mission(m, row, retained, multiplayer)
            m.write(m.address(0x800B2D14), 4, 0)
            m.call(0x80070C94)
            m.call(0x80078A88)
            m.call(0x8007146C)
            phase = load_state(m)
            phase.update(map=name, mission=mission, views=views,
                         multiplayer=multiplayer, level_kind=m.read(row + 32, 1))
            phases.append(phase)
            print(f'{m.phase}: {phase["packet_capacity"]} packets; PASS', flush=True)
    except Exception as exc:
        error = f'{exc} at {m.last_pc:08x}; registers {m.r}'
        print(error, flush=True)
    return {'map': 'SESSION', 'mission': 0, 'views': 1, 'multiplayer': False,
            'level_kind': 0, 'ntsc': ntsc, 'session': True, 'phases': phases,
            'hooks': m.hooks, 'rng_seed': 1, 'spu_register_writes': m.spu_writes,
            'events': m.events, 'heap': m.snapshot(), 'error': error}


def fixture_header(cases):
    text = ['/* Native PAL allocator traces; numeric results only.',
            f' * Segment SHA-256: {SEGMENT_SHA}',
            ' * Generated by generate_heap_reference.py. See README.md. */',
            'static const struct heap_reference_case {',
            '    unsigned first, count;',
            '} heap_reference_cases[] = {']
    offset = 0
    for case in cases:
        text.append(f'    {{{offset}, {len(case["events"])}}}, /* {case["map"]}, {case["views"]} views */')
        offset += len(case['events'])
    text += ['};', 'static const struct heap_reference_step {',
             '    u32 operation, a, b, c, result, count, hash;',
             '} heap_reference_steps[] = {']
    operations = {'malloc': 0, 'realloc': 1, 'free': 2, 'move_header': 3, 'largest_free': 4}
    for case in cases:
        for event in case['events']:
            operation = event['operation']
            a, b, c = event['args']
            if operation == 'free':
                a, b, c = event['payload'], 0, 0
            elif operation == 'malloc':
                c = 0
            elif operation == 'move_header':
                c = 0
            elif operation == 'largest_free':
                a, b, c = 0, 0, 0
            # Ordered addresses and requested sizes, including the sentinel.
            digest = 2166136261
            for block in event['allocations']:
                for value in (block['header'], block['size']):
                    for byte in struct.pack('<I', value):
                        digest = ((digest ^ byte) * 16777619) & 0xFFFFFFFF
            result = event['result'] if operation != 'free' else 0
            values = (operations[operation], a, b, c, result, len(event['allocations']), digest)
            text.append('    {' + ', '.join(f'0x{x:08x}u' for x in values) + '},')
    return '\n'.join(text + ['};', ''])


def budget_header(cases, disc, ntsc=False):
    def chunk_offset(data, wanted):
        directory_size = struct.unpack_from('<I', data, 12)[0]
        return next(struct.unpack_from('<I', data, at + 12)[0]
                    for at in range(0, directory_size, 16)
                    if data[at:at + 12].split(b'\0')[0].decode('ascii').lower() == wanted)

    session = any(case.get('session') for case in cases)
    symbol = 'level_memory' + ('_ntsc' if ntsc else '') + ('_session' if session else '') + '_reference'
    lines = ['/* Numeric file metadata and native final heap states. No retail code/assets.',
             f' * {"NTSC" if ntsc else "PAL"} segment SHA-256: {NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA}',
             ' * Generated by generate_heap_reference.py ' + ('--session' if session else '--suite --reload') + (' --ntsc' if ntsc else '') + '. */',
             '#ifndef Q2_LEVEL_MEMORY_REFERENCE_TYPE',
             '#define Q2_LEVEL_MEMORY_REFERENCE_TYPE',
             'struct level_memory_reference {',
             '    const char *map; unsigned zone;',
             '    q2_level_memory_files files;',
             '    q2_zone_resources resources;',
             '    u32 mission, views, multiplayer, game_level, reset, stream;',
             '    u32 heap_base, largest, packets, meshes, count, hash;',
             '};', '#endif',
             'static const struct level_memory_reference ' + symbol + '[] = {']
    for case in cases:
        for ordinal, phase in enumerate(case['phases']):
            name = phase.get('map', case['map'])
            views = phase.get('views', case['views'])
            prefix = '/Q2DATA/LEVELS/' + name.upper() + '/'
            common, media = disc.read(prefix + 'COMMON.DAT'), disc.read(prefix + 'SNDVRAM.DAT')
            keep, sound = struct.unpack_from('<II', media)
            zone = disc.read(prefix + phase['zone'] + '.DAT')
            values = (len(common), chunk_offset(common, 'resources'), len(zone),
                      chunk_offset(zone, 'scene'), len(media), sound, keep,
                      2 * (media[keep + 2] + media[keep + 3]))
            resources = [phase[k] for k in ('packet_control', 'particle_groups', 'requested_meshes')]
            resources = [v if v < 0x80000000 else v - 0x100000000 for v in resources]
            largest = next(e['result'] for e in case['events']
                           if e['operation'] == 'largest_free' and e['phase'] == phase['phase'])
            digest = 2166136261
            for block in phase['heap']:
                for value in (block['header'], block['size']):
                    for byte in struct.pack('<I', value):
                        digest = ((digest ^ byte) * 16777619) & 0xFFFFFFFF
            tail = (phase.get('mission', case['mission']), views,
                    int(phase.get('multiplayer', case['multiplayer'])),
                    int(phase.get('level_kind', case['level_kind']) != 0),
                    int(ordinal == 0), int(phase['phase'].startswith('stream-')), phase['heap'][0]['header'], largest,
                    phase['packet_capacity'], phase['subdiv_capacity'], len(phase['heap']), digest)
            lines += [f'    /* {name}, {phase["phase"]}, {views} views */',
                      f'    {{"{name}", {int(phase["zone"][4:])}, ' + '{' + ','.join(map(str, values)) + '}, {' + ','.join(map(str, resources)) + '},',
                      '     ' + ','.join(f'{v}u' for v in tail) + '},']
    return '\n'.join(lines + ['};', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('segment', type=Path)
    parser.add_argument('disc', type=Path)
    parser.add_argument('--map', default='BASE1')
    parser.add_argument('--mission', type=int, choices=range(7), help='Override the level-table mission: 0=no record, 1..5=campaign, 6=Tribute')
    parser.add_argument('--views', type=int, choices=range(1, 5), default=1)
    parser.add_argument('--multiplayer', action='store_true')
    parser.add_argument('--stream', type=int, nargs='*', default=[])
    parser.add_argument('--reload', action='store_true', help='Execute full unload then reload the same map')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--header', type=Path)
    parser.add_argument('--budget-header', type=Path)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--suite', action='store_true', help='Representative campaign, front-end and split-screen loads')
    parser.add_argument('--ntsc', action='store_true', help='Execute the verified US SLUS-00757 segment and disc')
    parser.add_argument('--session', action='store_true', help='Preserve native RAM through 17 selected map/session loads')
    args = parser.parse_args()
    image = args.segment.read_bytes()
    expected_sha = NTSC_SEGMENT_SHA if args.ntsc else SEGMENT_SHA
    if hashlib.sha256(image).hexdigest() != expected_sha:
        raise ValueError('Expected verified ' + ('NTSC SLUS-00757' if args.ntsc else 'PAL SLES-01534') + ' segment')
    if args.ntsc and args.header:
        raise ValueError('Use --budget-header for NTSC states; the primitive fixture is PAL-specific')
    disc = Form1Disc(args.disc)
    cases = []
    scenarios = [(args.map, args.mission, args.views, args.multiplayer, args.stream)]
    if args.suite:
        scenarios = [('BASE1', None, 1, False, [1, 0]), ('JAIL2', None, 1, False, []),
                     ('POWER1', None, 1, False, []), ('LAB', None, 1, False, []),
                     ('BOSS1', None, 1, False, []), ('QFRONT', None, 1, False, []),
                     ('QFMV', None, 1, False, [])]
        scenarios += [('MATRIX6', None, views, True, []) for views in (2, 3, 4)]
        scenarios += [('TIMS', None, 4, True, [])]
    try:
        if args.session:
            cases.append(run_session(image, disc, ntsc=args.ntsc))
        else:
            for scenario in scenarios:
                cases.append(run_case(image, disc, *scenario, reload=args.reload, ntsc=args.ntsc))
    finally:
        args.output.write_text(json.dumps({'segment_sha256': expected_sha,
            'scope': 'Isolated native loader/allocator execution. CD and BIOS strings/copies are host hooks; GPU transfers, cache/critical sections and SPU sample upload are stubbed. No hardware timing or whole-session claim.',
            'cases': cases}, indent=2) + '\n', encoding='utf-8')
        disc.stream.close()
    if not cases or any(case['error'] for case in cases):
        raise ValueError('Native loader trace failed; partial evidence saved')
    if args.header:
        content = fixture_header(cases)
        if args.check:
            if args.header.read_text(encoding='utf-8') != content:
                raise ValueError('Stored heap fixture differs from native execution')
        else:
            args.header.write_text(content, encoding='utf-8')
    if args.budget_header:
        disc = Form1Disc(args.disc)
        try:
            content = budget_header(cases, disc, ntsc=args.ntsc)
        finally:
            disc.stream.close()
        if args.check:
            if args.budget_header.read_text(encoding='utf-8') != content:
                raise ValueError('Stored level budget fixture differs from native execution')
        else:
            args.budget_header.write_text(content, encoding='utf-8')


if __name__ == '__main__':
    main()
