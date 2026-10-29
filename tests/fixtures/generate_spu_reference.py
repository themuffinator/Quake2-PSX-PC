"""Extract published SPU captures; expected samples never use the local DSP.

Stimuli and capture data: PCSX-Redux authors, MIT (spu_reference_LICENSE.txt).
The source manifest pins every input to a Git blob from one Nugget revision.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

HERE = Path(__file__).resolve().parent
MANIFEST = json.loads((HERE / "spu_reference_sources.json").read_text())
EXCLUDED = {
    "adpcm_edge_shift_12_15",  # reserved-shift behavior is not yet matched
    "adpcm_edge_flag_end_mute",  # finite onset, no repeating comparison period
    "adpcm_edge_flag_end_repeat",  # finite capture; separate boundary tests
}


def read_input(source, name):
    path = "tests/spu/" + name
    raw = (source / path).read_bytes()
    # Normalize text checkouts with Git's Windows CRLF conversion only.
    if name.endswith(".c"):
        raw = raw.replace(b"\r\n", b"\n")
    blob = hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()
    if blob != MANIFEST["git_blobs"][path]:
        raise ValueError(f"source differs from pinned Nugget revision: {path}")
    return raw


def arrays(text):
    result = {}
    for name, size, body in re.findall(
            r"static const uint8_t (\w+)\[(\d+)\][^=]*=\s*\{([^}]+)\}", text):
        body = re.sub(r"//[^\n]*", "", body)
        values = bytes(int(v, 16) for v in re.findall(r"0x[0-9a-fA-F]+", body))
        if len(values) != int(size):
            raise ValueError(f"unrecognized byte array: {name}")
        result[name] = values
    return result


def blocks(headers, payloads, flags=(4, 0, 0, 3)):
    return b"".join(bytes([h, f]) + p for h, f, p in zip(headers, flags, payloads))


def edge_stimulus(name, data):
    # Reproduce only the input builders in the hash-verified spu-adpcm-edge.c.
    mixed, ramp, zero, alt, pos, neg = (
        data["kEdge" + n] for n in
        ("Mixed", "Ramp", "Zero", "Alternating", "Positive", "Negative"))
    if name.startswith("adpcm_edge_predictor_"):
        return blocks([int(name[-1]) * 16 + 4] * 4, [mixed, ramp, zero, alt])
    if name.startswith("adpcm_edge_shift_"):
        first = int(name.split("_")[-2])
        return blocks(range(first, first + 4), [alt] * 4)
    if name.endswith("state_carry"):
        return blocks([0, 0x44, 0x44, 0x44], [pos, zero, zero, zero])
    if "saturation_" in name:
        return blocks([0x40] * 4, [neg if name.endswith("negative") else pos] * 4)
    if name.endswith("invalid_predictor_5_7"):
        return blocks([0x54, 0x64, 0x74, 0xf4], [mixed, ramp, alt, zero])
    if name.endswith("flag_code2_ignored"):
        return blocks([4] * 4, [ramp, pos, neg, zero], [4, 2, 0, 3])
    if name.endswith("flag_loop_start_latch"):
        return blocks([4] * 4, [pos, ramp, neg, zero], [0, 4, 0, 3])
    raise ValueError(f"unrecognized input case: {name}")


def generate(source):
    waves = arrays(read_input(source, "spu.c").decode())
    edges = arrays(read_input(source, "spu-adpcm-edge.c").decode())
    wave_cases = {
        "silent": ("kAdpcmSilent", 0x1000), "sine": ("kAdpcmSine", 0x1000),
        "sine_low": ("kAdpcmSine394Hz", 0x1000),
        "sine_high": ("kAdpcmSine5512Hz", 0x1000),
        "square": ("kAdpcmSquare", 0x1000), "triangle": ("kAdpcmTriangle", 0x1000),
        "loop_t0": ("kAdpcmTriangle", 0x1000), "loop_t1": ("kAdpcmTriangle", 0x1000),
        "sine7_pitch_1300": ("kAdpcmSine7", 0x1300),
    }
    for pitch in (0x0800, 0x1400, 0x1500, 0x2000, 0x3000):
        wave_cases[f"sine_pitch_{pitch:04x}"] = ("kAdpcmSine", pitch)
    out = [
        "/* PCSX-Redux authors, MIT. See spu_reference_LICENSE.txt.",
        " * Silicon captures and stimuli from Nugget " + MANIFEST["commit"] + ".",
        " * Expected samples are retained verbatim, with only the capture phase removed.",
        " * See README.md for the excluded startup/invalid-shift coverage. */",
        "typedef struct spu_reference {",
        "    const char *name; u16 pitch, source_phases; u32 count; u8 adpcm[64]; const s16 *pcm;",
        "} spu_reference;", "",
    ]
    rows = []
    for path in sorted(MANIFEST["git_blobs"]):
        filename = Path(path).name
        if not filename.endswith(".test.pcm"):
            continue
        name = filename.removesuffix(".test.pcm")
        if name in EXCLUDED:
            continue
        if name in wave_cases:
            wave, pitch = wave_cases[name]
            data = waves[wave]
        else:
            data, pitch = edge_stimulus(name, edges), 0x1000
        raw = read_input(source, filename)
        magic, length, warmup, period = struct.unpack_from("<4I", raw)
        if magic != 0x544d4350 or len(raw) != length + warmup + period or len(data) != 64:
            raise ValueError(f"unrecognized capture: {name}")
        pcm = list(struct.unpack("<" + "h" * ((len(raw) - length) // 2), raw[length:]))
        first = warmup // 2
        if name == "adpcm_edge_predictor_3":
            # Its unperiodized prefix retains unknown prior predictor state.
            # Keep the final two complete 112-sample loops verbatim.
            first = len(pcm) - 224
        pcm = pcm[first:]
        if not pcm:
            raise ValueError(f"empty comparison period: {name}")
        out.append("static const s16 spu_ref_" + name + "[] = {")
        for i in range(0, len(pcm), 12):
            out.append("    " + ", ".join(map(str, pcm[i:i+12])) + ",")
        out.extend(["};", ""])
        # Only these non-coprime rates require unknown source-phase classes.
        phases = 7 if name == "sine_pitch_1500" else 2 if name == "sine_pitch_2000" else 1
        rows.append('    {"' + name + '", 0x%04x, %d, %d, {' % (pitch, phases, len(pcm)) +
                    ", ".join(f"0x{x:02x}" for x in data) + "}, spu_ref_" + name + "},")
    if len(rows) != 28:
        raise ValueError(f"expected 28 captures, got {len(rows)}")
    out += ["static const spu_reference spu_references[] = {"] + rows + ["};", ""]
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path, help="pinned Nugget source directory")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    expected = generate(args.source.resolve())
    output = HERE / "spu_reference.h"
    if args.check:
        if output.read_text() != expected:
            raise SystemExit("SPU fixture differs from the published captures")
        print("28 SPU fixtures match hash-verified upstream capture data")
    else:
        output.write_text(expected)
        print(f"Wrote 28 SPU fixtures to {output}")


if __name__ == "__main__":
    main()
