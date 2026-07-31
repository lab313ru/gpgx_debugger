"""Index every compressed asset in a Mega Drive ROM by finding the calls that
unpack them.

Pirates of Dark Water has no table of graphics offsets — each load site names
its source directly:

    lea  ($00052F28).l,a0     ; 41F9 <src32>     compressed data
    lea  ($00FF3AA0).l,a1     ; 43F9 <dst32>     staging buffer
    jsr  ($28A2).w            ; 4EB8 28A2        unpack

So the asset index is the set of call sites. Scanning for the jsr and reading
back the two lea immediates recovers it statically, without running anything.

Call sites that do NOT match the idiom are reported too, and they are the
interesting ones: those load the source some other way (computed, or indexed
off a real table), which is where a table would show up if one exists.

Usage:
    python find_packed_assets.py ROM [--unpacker 28A2]
"""

from __future__ import annotations

import argparse
import struct


def be32(b: bytes, off: int) -> int:
    return struct.unpack_from(">I", b, off)[0]


def be16(b: bytes, off: int) -> int:
    return struct.unpack_from(">H", b, off)[0]


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--unpacker", default="28A2",
                    help="address of the unpacker, as used by jsr (xxxx).w")
    args = ap.parse_args()

    rom = open(args.rom, "rb").read()
    target = int(args.unpacker, 16)
    # jsr (target).w — absolute short addressing
    needle = b"\x4e\xb8" + struct.pack(">H", target)

    sites, odd = [], []
    pos = 0
    while True:
        pos = rom.find(needle, pos)
        if pos < 0:
            break
        # the idiom puts the two lea immediates in the 12 bytes before the jsr
        src = dst = None
        if pos >= 12 and rom[pos - 12:pos - 10] == b"\x41\xf9" \
                     and rom[pos - 6:pos - 4] == b"\x43\xf9":
            src, dst = be32(rom, pos - 10), be32(rom, pos - 4)
        if src is not None and src < len(rom):
            sites.append((pos, src, dst))
        else:
            odd.append(pos)
        pos += 2

    print(f"{len(sites)} direct call sites, {len(odd)} that load the source another way\n")
    print(f"{'call':>8}  {'source':>8}  {'dest':>8}  {'packed':>7}  first bytes")
    for call, src, dst in sorted(sites, key=lambda t: t[1]):
        # the stream starts with a 16-bit little-endian block length, which the
        # unpacker adds to the pointer to reach the end and decode backwards
        size = be16(rom, src) if src + 2 <= len(rom) else 0
        size = ((size & 0xFF) << 8) | (size >> 8)          # ror.w #8
        head = rom[src + 2:src + 10].hex()
        print(f"{call:08X}  {src:08X}  {dst:08X}  {size:7d}  {head}")

    if odd:
        print("\ncall sites worth reading by hand (source not a literal lea):")
        for c in odd:
            print(f"  {c:08X}")

    if sites:
        lo = min(s for _, s, _ in sites)
        hi = max(s for _, s, _ in sites)
        print(f"\ncompressed data spans {lo:06X}..{hi:06X}")


if __name__ == "__main__":
    main()
