"""Execute PAL resource selection and allocator code to obtain numeric fixtures.

The heap cases execute the real allocator, free and largest-block probe. Loader
cases inject an explicitly selected largest-block result, then execute both
retail call sites and packet initialization up to its allocation call. These
are isolated policy fixtures, not measurements of a complete loaded level.
No retail instructions or assets are copied into the generated header.
"""
import argparse
import hashlib
import json
from pathlib import Path

from generate_combat_reference import ArmourMachine

SEGMENT_SHA = "2dbdb53efd709a4cb5930ce91a39b55e8edad162c5fb773a7c6fa164a960fda8"


class ResourceMachine(ArmourMachine):
    def __init__(self, image, bss_begin=0xB26D0, bss_end=0xEE718, gp=0x800AE600):
        super().__init__(image)
        self.mem[bss_begin:bss_end] = bytes(bss_end - bss_begin)
        self.gp = gp
        self.largest_override = None
        self.registers()

    def registers(self):
        self.r = [0] * 32
        self.r[28], self.r[29] = self.gp, 0x801FFFF0
        self.load = None

    def intercept(self, pc):
        if pc == 0x8006EACC and self.largest_override is not None:
            self.r[2] = self.largest_override
            return True
        return False

    def call(self, pc, *args):
        self.registers()
        self.r[4:4 + len(args)] = args
        self.run(pc)
        return self.r[2]


def heap_reference(image):
    m = ResourceMachine(image)
    m.call(0x8006E8C0)
    empty = m.call(0x8006EACC)
    if empty != 1117872:
        raise ValueError("Unexpected native empty-heap payload capacity")
    allocations = []
    # First-fit and last-fit placement leave two separate holes after freeing B.
    for size, top in ((100000, 0), (300000, 0), (100000, 0), (300000, 1)):
        pointer = m.call(0x8006E468, size, top)
        if not pointer:
            raise ValueError("Native test allocation failed")
        allocations.append(pointer)
    m.write(0x800EE600, 4, allocations[1])
    m.call(0x8006E890, 0x800EE600)
    if m.read(0x800EE600, 4):
        raise ValueError("Native Free did not clear its caller's pointer")
    largest = m.call(0x8006EACC)
    node, top = m.read(0x800B2D58, 4), 0x801FF600
    gaps = []
    while node:
        following = m.read(node + 4, 4)
        span = (m.read(node + 8, 4) + 31) & ~3
        gap = (following or top) - node - span
        if gap:
            gaps.append(gap)
        node = following
    if largest != max(gaps) - 28 or sum(gaps) <= max(gaps):
        raise ValueError("The native probe did not select the largest fragmented gap")
    return {"empty_largest_payload": empty, "allocations": allocations,
            "fragmented_gaps": gaps, "fragmented_largest_payload": largest}


def policy_reference(image, control, meshes, largest):
    results = []
    for entry in (0x80070EA4, 0x80071498):
        m = ResourceMachine(image)
        m.largest_override = largest
        record = 0x800EE600
        m.write(0x800B2F8C, 4, record)
        m.write(record + 12, 4, control)
        m.write(record + 28, 4, meshes)
        m.run(entry, stop=0x80070AA8)
        selected = m.r[4]
        m.run(0x80070AA8, stop=0x80070BBC)
        results.append({"selected": selected,
                        "packets": m.read(0x800B2D98, 2),
                        "meshes": m.read(0x800B2CE0, 4),
                        "allocation_bytes": m.r[4]})
    if results[0] != results[1]:
        raise ValueError("Initial and streamed loader policies disagree")
    return results[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('segment', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    image = args.segment.read_bytes()
    if hashlib.sha256(image).hexdigest() != SEGMENT_SHA:
        raise ValueError("Expected the PAL SLES-01534 loaded segment")
    heap = heap_reference(image)
    rows, automatic = [], []
    for control in (-1, -31, -32, -33, -3000, -8192):
        for meshes in (-1, 0, 40, 112, 180):
            # An override must survive radically different heap states.
            expected = None
            for largest in (0, 100000, heap['fragmented_largest_payload'], heap['empty_largest_payload']):
                result = policy_reference(image, control, meshes, largest)
                if expected is not None and result != expected:
                    raise ValueError("Forced count depends on free heap")
                expected = result
            rows.append((control, meshes, expected['packets'], expected['meshes']))
    for meshes in (40, 112, 180):
        reserve = 1882 * meshes + 2
        for largest in (0, reserve, reserve + 3329, reserve + 3330,
                        heap['fragmented_largest_payload'], heap['empty_largest_payload']):
            expected = None
            for control in (0, 33, 4096, 6600, 0x7FFFFFFF):
                result = policy_reference(image, control, meshes, largest)
                if expected is not None and result != expected:
                    raise ValueError("Positive control unexpectedly determines the automatic count")
                expected = result
            automatic.append({"requested_meshes": meshes, "largest_payload": largest, **expected})
    text = ["/* Numeric results from both PAL loader paths and packet initialization.",
            f" * Segment SHA-256: {SEGMENT_SHA}",
            " * Generated by generate_resource_reference.py. See README.md.",
            " * control, requested meshes, allocated packets, allocated meshes */",
            "static const s32 world_resource_reference[][4] = {"]
    text += ['    {' + ', '.join(map(str, row)) + '},' for row in rows]
    text += ['};', '', '/* requested meshes, largest payload, packets, allocated meshes */',
             'static const u32 world_resource_automatic[][4] = {']
    text += ['    {' + ', '.join(f'{row[key]}u' for key in
             ('requested_meshes', 'largest_payload', 'packets', 'meshes')) + '},' for row in automatic]
    text += ['};', '']
    text = '\n'.join(text)
    if args.check:
        if args.output.read_text(encoding='utf-8') != text:
            raise ValueError("Stored resource vectors differ from native execution")
    else:
        args.output.write_text(text, encoding='utf-8')
    args.trace.write_text(json.dumps({"segment_sha256": SEGMENT_SHA, "heap": heap,
        "forced_vectors": len(rows), "native_loader_runs": 420,
        "automatic_policy_only": automatic,
        "limit": "Automatic policy inputs are synthetic; complete level heap history remains unmodeled."}, indent=2), encoding='utf-8')
    print(f'{len(rows)} forced-count vectors; 420 native loader runs; fragmented largest payload {heap["fragmented_largest_payload"]}')


if __name__ == '__main__':
    main()
