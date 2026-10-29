"""Execute PAL/US music selection using the user's original discs.

The native GetToc, SDK TOC builder, PlayTrackList, CdCheckMusic and SeekAndPlay
execute in bounded R3000 RAM. CD commands and file lookup are explicit hooks;
this proves selection/timers, not drive latency, audio decoding or waveforms.
No retail instructions or audio are written to the fixture.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from generate_heap_reference import Form1Disc, LoaderMachine, NTSC_SEGMENT_SHA
from generate_resource_reference import SEGMENT_SHA


def bcd(value):
    return (value // 10) * 16 + value % 10


def position(lba):
    sectors = lba + 150
    return bytes((bcd(sectors // 4500), bcd(sectors // 75 % 60),
                  bcd(sectors % 75), 0))


def cue_toc(cue):
    # The two verified releases use separate data/audio files. Refuse a
    # different layout rather than silently deriving an incorrect TOC.
    text = cue.read_text(encoding='utf-8-sig')
    files = re.findall(r'FILE\s+"([^"]+)"\s+BINARY', text, re.I)
    audio = re.search(r'TRACK\s+02\s+AUDIO\s+INDEX\s+00\s+00:00:00\s+'
                      r'INDEX\s+01\s+(\d+):(\d+):(\d+)', text, re.I)
    if len(files) != 2 or not audio:
        raise ValueError('Expected the original two-file data/audio CUE')
    sizes = [(cue.parent / name).stat().st_size for name in files]
    if any(size % 2352 for size in sizes):
        raise ValueError('Incomplete raw sector in CUE backing')
    lengths = [size // 2352 for size in sizes]
    mm, ss, ff = map(int, audio.groups())
    return [sum(lengths), 0, lengths[0] + (mm * 60 + ss) * 75 + ff]


class MusicMachine(LoaderMachine):
    def __init__(self, image, disc, toc, ntsc):
        super().__init__(image, disc, ntsc)
        self.toc = toc
        self.commands = []
        self.cd_hooks = {}
        self.write(self.address(0x800AE8D4), 4, 1)

    def intercept(self, native_pc):
        pc = self.canonical(native_pc)
        a0, a1, a2 = self.r[4:7]
        if pc in (0x800890B4, 0x80088E44):  # CdControlB / CdControl.
            widths = {0x0E: 1, 0x0D: 2, 0x1B: 4, 3: 4, 0x14: 1}
            params = [self.read(a1 + i, 1) for i in range(widths.get(a0, 0))]
            self.commands.append({'command': a0, 'params': params})
            if a0 == 0x13:  # GetTN: first/last track, BCD.
                self.put(a2, bytes((2, 1, bcd(len(self.toc) - 1))))
            elif a0 == 0x14:  # GetTD: whole-second absolute start/lead-out.
                track = (params[0] >> 4) * 10 + (params[0] & 15)
                self.put(a2, bytes((2,)) + position(self.toc[track])[:2])
            elif a0 not in (0x0E, 0x0D, 0x1B, 3, 8, 9):
                raise ValueError(f'Unexpected CD command {a0:02x}')
            self.r[2] = 1
        elif pc in (0x80088E1C, 0x80088D60, 0x80089200):
            # Callback registration and CD matrix latch have no selector writes.
            self.r[2] = 0
        elif pc == 0x8008B2FC:  # CdSearchFile returns CdlFILE metadata.
            lba, size = self.disc.info(self.string(a1))
            self.put(a0, position(lba) + struct.pack('<I', size))
            self.r[2] = a0
        else:
            return super().intercept(native_pc)
        key = f'{native_pc:08x}'
        self.cd_hooks[key] = self.cd_hooks.get(key, 0) + 1
        return True


def walk(m, playlist):
    sequence = []
    m.call(0x80071B34, playlist)
    for _ in range(15):  # At least two full cycles of every authored loop.
        m.write(m.address(0x800B2710), 4, 0)
        before = len(m.commands)
        m.call(0x800718E4)
        playing = bool(m.read(m.address(0x800AEBFC), 4))
        sequence.append({'id': m.read(m.address(0x800B2E5C), 4) if playing else -1,
                         'cursor': m.read(m.address(0x800AEC00), 4) - playlist,
                         'fields': m.read(m.address(0x800B2708), 4) if playing else 0,
                         'commands': m.commands[before:]})
        if not playing:
            break
    return sequence


def run(cue, ntsc):
    disc = Form1Disc(cue)
    try:
        exe = disc.read('/SLUS_007.57' if ntsc else '/SLES_015.34')
        image = exe[2048:2048 + struct.unpack_from('<I', exe, 28)[0]]
        sha = hashlib.sha256(image).hexdigest()
        if sha != (NTSC_SEGMENT_SHA if ntsc else SEGMENT_SHA):
            raise ValueError('Expected the verified original executable')
        m = MusicMachine(image, disc, cue_toc(cue), ntsc)
        table = m.address(0x800A1DD8)
        music_before = bytes(m.mem[table & 0x1FFFFFFF:(table & 0x1FFFFFFF) + 132])
        m.call(0x80071548)  # Includes the actual SDK's CdGetToc implementation.
        if music_before != bytes(m.mem[table & 0x1FFFFFFF:(table & 0x1FFFFFFF) + 132]):
            raise ValueError('GetToc modifies the static music entries')
        toc = [[m.read(m.address(0x800DF14C) + i * 4 + j, 1) for j in range(4)]
               for i in range(len(m.toc))]
        entries = []
        for track in range(22):
            before = len(m.commands)
            m.write(m.address(0x800B2E5C), 4, track)
            m.call(0x80071718)
            file = m.read(table + track * 6, 1)
            entries.append({'id': track, 'file': file if file < 128 else file - 256,
                            'channel': m.read(table + track * 6 + 1, 1),
                            'tenths': m.read(table + track * 6 + 4, 2),
                            'fields': m.read(m.address(0x800B2708), 4),
                            'commands': m.commands[before:]})
        noops = []
        for track in (0, 1, 2, 21):
            before, commands = bytes(m.mem), len(m.commands)
            m.call(0x80071B5C, track)
            if bytes(m.mem) != before or len(m.commands) != commands:
                raise ValueError('PlayThisXA unexpectedly has an effect')
            noops.append(track)
        rows = []
        for index in range(64):
            row = m.address(0x8009C6C8) + index * 56
            if not m.read(row, 4):
                break
            sequence = walk(m, row + 0x22)
            if any(step['id'] in (0, 1) for step in sequence):
                raise ValueError('A named playlist selects the CD-DA branch')
            period = 0
            if sequence[0]['id'] >= 0:
                period = next((i for i in range(1, len(sequence))
                               if sequence[i] == sequence[0]), 0)
                if not period or any(step != sequence[i % period]
                                     for i, step in enumerate(sequence)):
                    raise ValueError('Playlist does not repeat within fifteen selections')
            rows.append({'directory': m.string(row + 12), 'period': period, 'sequence': sequence})
        else:
            raise ValueError('Missing named-level sentinel')
        # Deliberate RAM inputs exercise termination after music has played;
        # authored lists only terminate immediately or loop explicitly.
        cases = []
        for name, data in [('finite-two', [2, 3, 0]), ('finite-one', [15, 0]),
                           ('explicit-loop', [2, 3, 254]), ('empty', [0])]:
            m.put(0x800F0000, bytes(data))
            cases.append({'name': name, 'input': data, 'sequence': walk(m, 0x800F0000)})
        return {'segment_sha256': sha, 'toc': toc, 'entries': entries, 'rows': rows,
                'cases': cases, 'noop_ids': noops, 'hooks': m.cd_hooks, 'libc_hooks': m.hooks}
    finally:
        disc.stream.close()


def header(evidence):
    pal, ntsc = evidence['pal'], evidence['ntsc']
    lines = ['/* Numeric native PAL/US music selection and timers; no retail code.',
             f' * PAL SHA-256: {SEGMENT_SHA}', f' * US SHA-256: {NTSC_SEGMENT_SHA}',
             ' * Generated by generate_music_reference.py. See README.md. */',
             'static const struct music_entry_reference {',
             '    int file; unsigned channel, tenths, pal_fields, ntsc_fields;',
             '} music_entry_reference[] = {']
    if len(pal['rows']) != len(ntsc['rows']):
        raise ValueError('PAL/US playlist counts differ')
    for a, b in zip(pal['entries'], ntsc['entries']):
        if any(a[key] != b[key] for key in ('file', 'channel', 'tenths')):
            raise ValueError('PAL/US music entries differ')
        lines.append('    {' + ', '.join(str(x) for x in
            (a['file'], a['channel'], a['tenths'], a['fields'], b['fields'])) + '},')
    lines += ['};', '', 'static const struct music_playlist_reference {',
              '    const char *directory;', '    unsigned count;',
              '    int ids[15], cursors[15];',
              '} music_playlist_reference[] = {']
    for a, b in zip(pal['rows'], ntsc['rows']):
        ids = [step['id'] for step in a['sequence']]
        cursors = [step['cursor'] for step in a['sequence']]
        if (a['directory'] != b['directory'] or
                ids != [step['id'] for step in b['sequence']] or
                cursors != [step['cursor'] for step in b['sequence']]):
            raise ValueError('PAL/US playlist traversal differs')
        lines.append('    {' + json.dumps(a['directory']) + ', ' + str(len(ids)) + ', {' +
                     ', '.join(map(str, ids)) + '}, {' +
                     ', '.join(map(str, cursors)) + '}},')
    lines += ['};', '', '/* Explicit RAM inputs, not authored retail playlists. */',
              'static const struct music_policy_reference {',
              '    unsigned input[3], count;', '    int ids[15];',
              '} music_policy_reference[] = {']
    for a, b in zip(pal['cases'], ntsc['cases']):
        ids = [step['id'] for step in a['sequence']]
        if a['input'] != b['input'] or ids != [step['id'] for step in b['sequence']]:
            raise ValueError('PAL/US synthetic playlist policy differs')
        lines.append('    {{' + ', '.join(map(str, a['input'])) + '}, ' +
                     str(len(ids)) + ', {' + ', '.join(map(str, ids)) + '}},')
    return '\n'.join(lines + ['};', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('pal_cue', type=Path)
    parser.add_argument('ntsc_cue', type=Path)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    evidence = {'pal': run(args.pal_cue, False), 'ntsc': run(args.ntsc_cue, True)}
    content = header(evidence)
    output = Path(__file__).with_name('music_playlist_reference.h')
    if args.check:
        if output.read_text(encoding='utf-8') != content:
            raise ValueError('Stored music fixture differs from native execution')
    else:
        output.write_text(content, encoding='utf-8')
    args.trace.write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    starts = sum(step['id'] >= 0 for revision in evidence.values()
                 for row in revision['rows'] for step in row['sequence'])
    print(f'102 native playlists, {starts} starts, 44 direct selectors, '
          '8 no-op calls and 8 synthetic policy cases verified')


if __name__ == '__main__':
    main()
