"""Run PAL damage machine code to generate numeric regression fixtures.

Input is the header-stripped segment written by q2psx-inspect exe. No retail
code is embedded in the generated headers. This bounded R3000 interpreter models
branch/load delay slots. Isolated armour cases record SetWinFade. Optional player
cases execute real fade/shock stores and view composition, with explicit hooks
for memset, axis-only normalisation and sound dispatch. It is not a general
PlayStation emulator.
"""
import argparse
import hashlib
import struct
from pathlib import Path


def signed(value, bits=32):
    value &= (1 << bits) - 1
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class ArmourMachine:
    def __init__(self, image):
        self.mem = bytearray(2 * 1024 * 1024)
        self.mem[0x18000:0x18000 + len(image)] = image
        self.r = [0] * 32
        self.hi = self.lo = 0
        self.load = None
        self.fades = 0

    def read(self, address, size, sign=False):
        address &= 0x1FFFFFFF
        if address + size > len(self.mem):
            raise ValueError(f"Read outside RAM: {address:08x}")
        return int.from_bytes(self.mem[address:address + size], 'little', signed=sign) & 0xFFFFFFFF

    def write(self, address, size, value):
        address &= 0x1FFFFFFF
        if address + size > len(self.mem):
            raise ValueError(f"Write outside RAM: {address:08x}")
        self.mem[address:address + size] = (value & ((1 << (size * 8)) - 1)).to_bytes(size, 'little')

    def intercept(self, pc):
        if pc == 0x8007832C:  # SetWinFade: no armour/inventory writes.
            self.fades += 1
            return True
        return False

    def run(self, entry, stop=0, limit=2000):
        pc, next_pc = entry, entry + 4
        for _ in range(limit):
            if pc == stop:
                return signed(self.r[2], 16)
            if self.intercept(pc):
                if self.load:
                    self.r[self.load[0]] = self.load[1]
                    self.load = None
                pc = self.r[31]
                next_pc = pc + 4
                continue
            word = self.read(pc, 4)
            op, rs, rt, rd = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
            shift, fn, imm = (word >> 6) & 31, word & 63, signed(word, 16)
            a, b = self.r[rs], self.r[rt]
            writes, pending, destination = {}, None, next_pc + 4
            if op == 0:
                if fn == 0: writes[rd] = b << shift
                elif fn == 2: writes[rd] = b >> shift
                elif fn == 3: writes[rd] = signed(b) >> shift
                elif fn == 4: writes[rd] = b << (a & 31)
                elif fn == 6: writes[rd] = b >> (a & 31)
                elif fn == 7: writes[rd] = signed(b) >> (a & 31)
                elif fn == 8: destination = a
                elif fn == 9:
                    destination = a
                    writes[rd] = pc + 8
                elif fn == 16: writes[rd] = self.hi
                elif fn == 18: writes[rd] = self.lo
                elif fn == 24:
                    product = signed(a) * signed(b)
                    self.hi, self.lo = (product >> 32) & 0xFFFFFFFF, product & 0xFFFFFFFF
                elif fn == 25:
                    product = a * b
                    self.hi, self.lo = (product >> 32) & 0xFFFFFFFF, product & 0xFFFFFFFF
                elif fn == 26:
                    if b == 0: raise ValueError('Division by zero')
                    quotient = abs(signed(a)) // abs(signed(b))
                    if (signed(a) < 0) != (signed(b) < 0): quotient = -quotient
                    self.lo, self.hi = quotient & 0xFFFFFFFF, (signed(a) - quotient * signed(b)) & 0xFFFFFFFF
                elif fn == 27:
                    if b == 0: raise ValueError('Division by zero')
                    self.lo, self.hi = a // b, a % b
                elif fn in (32, 33): writes[rd] = a + b
                elif fn in (34, 35): writes[rd] = a - b
                elif fn == 36: writes[rd] = a & b
                elif fn == 37: writes[rd] = a | b
                elif fn == 38: writes[rd] = a ^ b
                elif fn == 39: writes[rd] = ~(a | b)
                elif fn == 42: writes[rd] = int(signed(a) < signed(b))
                elif fn == 43: writes[rd] = int(a < b)
                else: raise ValueError(f'Unsupported SPECIAL at {pc:08x}: {word:08x}')
            elif op == 1 and rt in (0, 1):
                if (signed(a) < 0) == (rt == 0): destination = pc + 4 + imm * 4
            elif op in (2, 3):
                destination = ((pc + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if op == 3: writes[31] = pc + 8
            elif op in (4, 5, 6, 7):
                take = {4: a == b, 5: a != b, 6: signed(a) <= 0, 7: signed(a) > 0}[op]
                if take: destination = pc + 4 + imm * 4
            elif op in (8, 9): writes[rt] = a + imm
            elif op == 10: writes[rt] = int(signed(a) < imm)
            elif op == 11: writes[rt] = int(a < (imm & 0xFFFFFFFF))
            elif op == 12: writes[rt] = a & (word & 65535)
            elif op == 13: writes[rt] = a | (word & 65535)
            elif op == 14: writes[rt] = a ^ (word & 65535)
            elif op == 15: writes[rt] = (word & 65535) << 16
            elif op == 18 and hasattr(self, 'cop2'):
                pending = self.cop2(word)
            elif op in (32, 33, 35, 36, 37):
                size = {32: 1, 33: 2, 35: 4, 36: 1, 37: 2}[op]
                pending = (rt, self.read(a + imm, size, op in (32, 33)))
            elif op in (34, 38):  # Little-endian LWL/LWR; paired loads forward.
                address = a + imm
                memory = self.read(address & ~3, 4)
                prior = self.load[1] if self.load and self.load[0] == rt else b
                shift = ((3 - (address & 3)) if op == 34 else (address & 3)) * 8
                mask = ((1 << shift) - 1) if op == 34 else (0xFFFFFFFF << (32 - shift)) & 0xFFFFFFFF
                value = (memory << shift) if op == 34 else (memory >> shift)
                pending = (rt, ((prior & mask) | value) & 0xFFFFFFFF)
            elif op in (40, 41, 43): self.write(a + imm, {40: 1, 41: 2, 43: 4}[op], b)
            elif op == 42:  # SWL
                address = a + imm
                self.write(address & ~3, (address & 3) + 1, b >> ((3 - (address & 3)) * 8))
            elif op == 46:  # SWR
                address = a + imm
                self.write(address, 4 - (address & 3), b)
            else: raise ValueError(f'Unsupported opcode at {pc:08x}: {word:08x}')
            if self.load:
                self.r[self.load[0]] = self.load[1]
            for register, value in writes.items(): self.r[register] = value & 0xFFFFFFFF
            self.r[0], self.load = 0, pending
            pc, next_pc = next_pc, destination
        raise ValueError(f'Native routine exceeded {limit} instructions at {pc:08x}')


class PlayerDamageMachine(ArmourMachine):
    """Damage, post-hit feedback and view composition; no display or sound device."""
    def intercept(self, pc):
        if pc == 0x80089E18:  # memset, confined to the supplied destination.
            for i in range(self.r[6]): self.write(self.r[4] + i, 1, self.r[5])
            self.r[2] = self.r[4]
            return True
        if pc == 0x8008A588:  # Normalise: these fixtures only supply axis vectors.
            values = [signed(self.read(self.r[4] + i * 4, 4)) for i in range(3)]
            if sum(v != 0 for v in values) != 1:
                raise ValueError('Only nonzero axis normalisation is supported')
            for i, value in enumerate(values):
                self.write(self.r[5] + i * 4, 4, 4096 if value > 0 else -4096 if value < 0 else 0)
            return True
        if pc in (0x8007270C, 0x80073704):  # Burn/pain sample dispatch only.
            return True
        # Unlike the isolated armour fixtures, execute the real fade and shock
        # routines. Their writes go to GameWindow, not the client amplitudes.
        return False


def evaluate_camera(image, damage, mod, armour, skill, side, pitch, roll):
    m = PlayerDamageMachine(image)
    entity, attacker, client, window = 0x80100000, 0x80100800, 0x800C7C60, 0x800D5C30
    point, output = 0x80101000, 0x80101020
    m.write(entity + 12, 4, client)
    m.write(attacker + 0x2EC, 4, 0x80102000)  # A live creature, including skill-0 halving.
    m.write(client, 4, window)
    m.write(entity + 0x108, 2, 100)
    m.write(entity + 0x10A, 2, 100)
    m.write(client + 0x4C, 1, 2)
    m.write(client + 0x4E, 2, 100 if armour else 0)
    m.write(client + 0x50, 2, 100 if armour else 0)
    m.write(client + 0x74, 2, 100 if armour == 2 else 0)
    m.write(client + 0x5C, 4, 0x18000 if armour == 2 else 0)
    m.write(client + 0x9C, 2, pitch)
    m.write(client + 0x9E, 2, roll)
    m.write(0x800B334A, 2, skill)
    m.write(0x800AEBAC, 4, 1000)
    m.write(0x800AEBCC, 4, 0)
    m.write(0x800B29EC, 2, 0)
    m.write(point, 4, side * 1000)
    m.r[4:8] = [attacker, entity, damage & 0xFFFFFFFF, mod]
    m.r[28], m.r[29] = 0x800AE600, 0x801FF000
    m.write(m.r[29] + 16, 4, point if side else 0)
    m.run(0x80057D54)
    after_hit = [signed(m.read(client + off, 2), 16) for off in (0x9C, 0x9E)]
    # This is a basic block of PlayerThink. Supply its established saved-register
    # bindings; stop before animation selection, after pain/flash processing.
    m.r[17], m.r[19] = client, entity
    m.run(0x8003AE10, stop=0x8003AFA8)
    after_feedback = [signed(m.read(client + off, 2), 16) for off in (0x9C, 0x9E)]
    if after_hit != [pitch, roll] or after_feedback != [pitch, roll]:
        raise ValueError('Native damage or feedback changed the camera amplitudes')
    m.r[4:6], m.r[31] = [output, entity], 0
    m.run(0x80038260)
    health = signed(m.read(entity + 0x108, 2), 16)
    held = signed(m.read(client + 0x4E, 2), 16)
    cells = signed(m.read(client + 0x74, 2), 16)
    deadline = signed(m.read(client + 0xD0, 4))
    angles = [signed(m.read(output + i * 2, 2), 16) for i in range(3)]
    return (damage, mod, armour, skill, side, pitch, roll, health, held, cells, deadline, *angles)


def evaluate(image, kind, damage, held, cls=0, energy=0, skill=1, power=1, fired=0):
    m = ArmourMachine(image)
    entity, client = 0x80100000, 0x80100400
    m.write(entity + 12, 4, client)
    m.write(client, 4, 0x800D5C30)
    m.write(client + 0x74, 2, held)
    m.write(client + 0x5C, 4, 0x18000 if power else 0)
    m.write(client + 0x4C, 1, cls)
    m.write(client + 0x4E, 2, held)
    m.write(0x800B334A, 2, skill)
    m.write(0x800AEAA0, 1, fired)
    m.r[4:7] = [entity, damage & 0xFFFFFFFF, energy]
    m.r[28], m.r[29] = 0x800AE600, 0x801FF000
    result = m.run(0x80057A9C if kind == 0 else 0x80057BE4)
    remaining = signed(m.read(client + (0x74 if kind == 0 else 0x4E), 2), 16)
    fade = (1 if kind == 0 else 2) if m.fades else (0 if kind == 0 else fired)
    return (kind, damage, held, cls, energy, skill, power, fired, result, remaining, fade)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('segment', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--camera-output', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    image = args.segment.read_bytes()
    if hashlib.sha256(image).hexdigest() != '2dbdb53efd709a4cb5930ce91a39b55e8edad162c5fb773a7c6fa164a960fda8':
        raise ValueError('Expected the verified PAL SLES-01534 loaded segment')
    # Verify both prologues and key branch/shift instructions for this PAL build.
    for address, word in ((0x80057A9C, 0x27BDFFD8), (0x80057AB8, 0x10A00044),
                          (0x80057BE4, 0x27BDFFD8), (0x80057C84, 0x00028302)):
        if struct.unpack_from('<I', image, address - 0x80018000)[0] != word:
            raise ValueError('Unsupported executable: expected PAL SLES-01534 armour code')
    cases = [evaluate(image, 0, damage, 100) for damage in (-32768, -30, -3, -2, -1, 0, 1, 2, 3, 90, 32767)]
    cases += [evaluate(image, 0, damage, held) for damage in (-30, 30) for held in (-32768, -1, 0, 1, 16383, 16384, 32767)]
    cases += [evaluate(image, 0, -30, 100, power=0)]
    cases += [evaluate(image, 1, damage, 100, cls, energy, skill)
              for damage in (-32768, -30, -1, 0, 1, 100, 32767)
              for cls in range(3) for energy in range(2) for skill in range(2)]
    cases += [evaluate(image, 1, damage, held, cls=2, fired=fired)
              for damage in (-30, 30) for held in (-32768, -1, 0, 1, 32767) for fired in range(2)]
    text = '/* Generated by generate_combat_reference.py from PAL retail machine code.\n'
    text += f' * Segment SHA-256: {hashlib.sha256(image).hexdigest()}\n'
    text += ' * kind, damage, held, class, energy, skill, power, fired, result, remaining, fade */\n'
    text += 'static const s32 combat_armour_reference[][11] = {\n'
    text += ''.join('    {' + ', '.join(map(str, case)) + '},\n' for case in cases)
    text += '};\n'
    if args.check:
        if args.output.read_text() != text:
            raise ValueError('Stored armour vectors differ from native execution')
    else:
        args.output.write_text(text)
    print(f'{len(cases)} retail armour vectors {"verified" if args.check else "written"}')
    if args.camera_output:
        camera = [evaluate_camera(image, damage, mod, 0, 1, 0, pitch, roll)
                  for mod in range(22) for damage in (20, 65)
                  for pitch, roll in ((0, 0), (31, -47))]
        camera += [evaluate_camera(image, damage, mod, 0, 1, side, 0, 0)
                   for mod in (3, 13, 15, 18) for side in (-1, 1)
                   for damage in (-20, 0, 20)]
        camera += [evaluate_camera(image, damage, 18, armour, skill, 0, 31, -47)
                   for armour in (1, 2) for skill in (0, 1) for damage in (-20, 0, 1, 20, 65)]
        text = '/* Generated by generate_combat_reference.py from PAL retail machine code.\n'
        text += f' * Segment SHA-256: {hashlib.sha256(image).hexdigest()}\n'
        text += ' * Damage and post-hit feedback preserve client+0x9C/+0x9E.\n'
        text += ' * damage, mod, armour, skill, side, pitch, roll, health, held, cells, pain_time, angles[3] */\n'
        text += 'static const s32 combat_camera_reference[][14] = {\n'
        text += ''.join('    {' + ', '.join(map(str, case)) + '},\n' for case in camera)
        text += '};\n'
        if args.check:
            if args.camera_output.read_text() != text:
                raise ValueError('Stored camera vectors differ from native execution')
        else:
            args.camera_output.write_text(text)
        print(f'{len(camera)} retail camera vectors {"verified" if args.check else "written"}')


if __name__ == '__main__':
    main()
