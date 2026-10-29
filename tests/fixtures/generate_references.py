"""Regenerate independent GPU/GTE fixtures from the pinned ps1-tests source."""
import argparse
import hashlib
import re
from pathlib import Path

from PIL import Image

DIGESTS = {
    "triangle": "d5a03568f66ee69efed3cc8516d7b79d32a2733e879ba6c543d55612ee9c9144",
    "uv-interpolation": "363c37b1f37555a8f1f0f0810dcaf05c59a0ff7cc6d67afb2da6345221b75113",
    "lines": "2e04988670abba6d0d4d8693b9df55b12c6e8a945f52b9db2b727b6411d8cf5b",
    "vram-to-vram-overlap": "28cd11886418438e76fafd970f990205ffe6fd776e755b846ab427fa86c305af",
    "clut-cache": "c59292e35df4eae6dbc80fa44991ad2d5ff4a432535e65fce3b3681889c56d9c",
    "gte": "10136d1335069b8f8e4d5e3f1c1f197a7c4ec1e7606ece1aa658a8c0d1706240",
}


def verify(path, case):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != DIGESTS[case]:
        raise ValueError(f"Unrecognized source digest: {path}")
    return data


def hash_region(image, x, y, width, height):
    result = 14695981039346656037
    for r, g, b in image.crop((x, y, x + width, y + height)).getdata():
        word = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
        for byte in (word & 255, word >> 8):
            result = ((result ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return result


def gpu_fixture(root):
    images = {}
    for case in DIGESTS:
        if case == "gte":
            continue
        path = root / "gpu" / case / "vram.png"
        verify(path, case)
        images[case] = Image.open(path).convert("RGB")
    arrays = []
    for name, case, x, y, width, height in (
        ("clut_rows", "clut-cache", 0, 0, 1024, 256),
        ("triangle_rows", "triangle", 0, 0, 1024, 512),
        ("uv_rows", "uv-interpolation", 0, 0, 1024, 512),
        ("line_coverage_rows", "lines", 0, 0, 320, 100),
        ("line_colour_rows", "lines", 0, 100, 148, 132),
    ):
        arrays.append((name, [hash_region(images[case], x, y + row, width, 1)
                              for row in range(height)]))
    arrays.append(("move_hashes", [
        hash_region(images["vram-to-vram-overlap"], i * 42 + 1, t * 42 + 1, 40, 26)
        for t in range(7) for i in range(1, 22)
    ]))
    output = "/* RGB555 row hashes from ps1-tests reference VRAM images. See README.md. */\n"
    for name, values in arrays:
        output += f"static const u64 {name}[{len(values)}] = {{\n"
        for start in range(0, len(values), 4):
            output += "    " + ", ".join(f"0x{v:016x}ULL" for v in values[start:start+4]) + ",\n"
        output += "};\n"
    return output


def gte_fixture(root):
    source = verify(root / "gte/test-all/tests.c", "gte").decode()
    source = re.sub(r"//[^\n]*", "", source)
    pattern = r'\.name = "test (\d+)[^"]*",\s*\.input = \{([^}]+)\},\s*\.opcode = (0x[0-9a-f]+),\s*\.output = \{([^}]+)\}'
    vectors = []
    for number, ins, opcode, outs in re.findall(pattern, source, re.S):
        op = int(opcode, 16)
        if op == 0xffffffff:  # upstream register-only cases, no instruction
            continue
        command, sf, lm = op & 63, (op >> 19) & 1, (op >> 10) & 1
        keep = command in (6, 18, 40, 45, 46)
        keep |= command in (1, 48) and sf == 1 and lm == 0
        keep |= command in (19, 20, 22, 27, 28, 30, 32, 63) and sf == 1 and lm == 1
        if keep:
            ins = [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", ins)]
            outs = [int(v, 16) for v in re.findall(r"0x[0-9a-f]+", outs)]
            assert len(ins) == len(outs) == 64
            vectors.append((int(number), op, ins, outs[7:32] + outs[63:64]))
    assert len(vectors) == 368, len(vectors)
    output = "/* ps1-tests captured GTE vectors; MIT, see ps1-tests-LICENSE.txt. */\n"
    output += "static const struct gte_reference { unsigned id; u32 opcode, input[64], output[26]; } gte_vectors[] = {\n"
    for number, op, ins, outs in vectors:
        output += f"    {{ {number}, 0x{op:08x}u, {{\n"
        for values in (ins, outs):
            for start in range(0, len(values), 8):
                output += "        " + ", ".join(f"0x{v:08x}u" for v in values[start:start+8]) + ",\n"
            output += "    }, {\n" if values is ins else "    } },\n"
    return output + "};\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    destination = Path(__file__).resolve().parent
    for name, generate in (("gpu_reference.h", gpu_fixture), ("gte_reference.h", gte_fixture)):
        expected = generate(args.source)
        path = destination / name
        if args.check:
            if path.read_text(encoding="utf-8") != expected:
                raise SystemExit(f"Fixture differs: {path}")
            print(f"Verified {name}")
        else:
            path.write_text(expected, encoding="utf-8")


if __name__ == "__main__":
    main()
