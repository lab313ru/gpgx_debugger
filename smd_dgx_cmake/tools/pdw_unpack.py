"""Decompressor for the packed graphics format in Pirates of Dark Water (MD).

Reconstructed from the 68000 routine at ROM 0x28A2. It is a bitstream LZ77
that runs *backwards*: the block starts with a 16-bit little-endian length used
to seek to its end, and from there both the input pointer and the output
pointer walk downwards. Bits come MSB-first out of bytes consumed in
descending address order.

Block layout
    +0   uint16 le   size of the packed payload
    ...              payload
    end-2 uint16 le  size of the unpacked output   (read first, backwards)
    end-3 uint8      initial bit count
    end-4 uint8      initial bit buffer

Bit fields, exactly as the routine reads them:
    get_bit()        one bit; also latches its value in d2
    get_bits(n)      n+1 bits
    get_sized()      table[d2]+1 bits, table = {3, 7, 15, 0} at ROM 0x2990,
                     so d2 selects a 4-, 8-, 16- or 1-bit field

The code below mirrors the routine instruction group by instruction group
rather than paraphrasing it: the format has enough shared state between steps
(d2 in particular is set as a side effect of reading a bit and then used to
pick a field width) that a tidied-up version is easy to get subtly wrong.

Usage:
    python pdw_unpack.py ROM --at 52F28 [--out FILE]
    python pdw_unpack.py ROM --all            # every asset the scanner finds
"""

from __future__ import annotations

import argparse
import struct

TABLE = (3, 7, 15, 0)          # ROM 0x2990, indexed by d2


class Unpacker:
    def __init__(self, rom: bytes, src: int) -> None:
        self.rom = rom
        self.a0 = src
        self.d2 = 0
        self.d4 = 0            # bits left in the buffer
        self.d5 = 0            # bit buffer (8 bits)

    # -- bit reader (ROM 0x2960 / 0x297A) ---------------------------------
    def _shift_in(self, d0: int) -> int:
        if self.d4 == 0:                       # refill from the byte below
            self.d5 = self.rom[self.a0]
            self.a0 -= 1
            self.d4 = 8
        bit = (self.d5 >> 7) & 1               # asl.b #1,d5 -> X
        self.d5 = (self.d5 << 1) & 0xFF
        d0 = ((d0 << 1) | bit) & 0xFFFF        # roxl.w #1,d0
        self.d4 -= 1
        return d0

    def get_bit(self) -> int:
        d0 = self._shift_in(0)
        self.d2 = d0                           # the routine leaves it in d2
        return d0

    def get_bits(self, n: int) -> int:
        """n+1 bits, matching the dbra-style loop."""
        d0 = 0
        for _ in range(n + 1):
            d0 = self._shift_in(d0)
        self.d2 = d0
        return d0

    def get_sized(self) -> int:
        """table[d2]+1 bits — the width depends on whatever last set d2."""
        return self.get_bits(TABLE[self.d2 & 3])

    # -- main loop (ROM 0x28A2) -------------------------------------------
    def run(self) -> bytes:
        rom = self.rom

        packed = struct.unpack_from("<H", rom, self.a0)[0]   # move.w + ror.w #8
        self.a0 += 2
        self.a0 += packed                                    # seek to the end

        self.a0 -= 1
        out_size = rom[self.a0] << 8
        self.a0 -= 1
        out_size = (out_size & 0xFF00) | rom[self.a0]

        self.a0 -= 1
        self.d4 = rom[self.a0]                 # initial bit count
        self.a0 -= 1
        self.d5 = rom[self.a0]                 # initial bit buffer
        self.a0 -= 1

        out = bytearray(out_size)
        a1 = out_size                          # output fills downwards
        written = 0                            # d6

        # Both copies bounds-check on purpose. A misdecode naturally produces a
        # negative index, and Python would quietly wrap it to the far end of
        # the buffer — turning a broken decoder into one that still fills
        # exactly the declared number of bytes and looks correct.
        def copy_from_output(a3: int, length: int) -> None:
            nonlocal a1, written
            a1 -= length
            if a1 < 0 or a3 < 0 or a3 + length > out_size:
                raise RuntimeError(f"output copy out of range: dst={a1} src={a3} len={length}")
            for i in range(length):
                out[a1 + i] = out[a3 + i]
            written += length

        def copy_from_stream(a3: int, length: int) -> None:
            nonlocal a1, written
            a1 -= length
            if a1 < 0 or a3 < 0 or a3 + length > len(rom):
                raise RuntimeError(f"literal copy out of range: dst={a1} src={a3:X} len={length}")
            out[a1:a1 + length] = rom[a3:a3 + length]
            written += length

        guard = 0
        while written < out_size:
            guard += 1
            if guard > out_size + 64:
                raise RuntimeError("decoder made no progress")

            if self.get_bit():                         # literal run
                d0 = self.get_bits(1)                  # 2 bits
                if d0 >= 3:
                    if self.get_bit():
                        d0 = self.get_bit() + 1
                    else:
                        d0 = 0
                    self.d2 = d0
                    d0 = self.get_sized() + 3
                d0 += 1
                length = d0
                self.a0 -= d0
                copy_from_stream(self.a0 + 1, length)
                if written >= out_size:
                    break

            # a match always follows a literal run, and is also the path taken
            # when the first bit was 0
            if self.get_bit():
                d0 = self.get_sized()                  # d2 == 1 -> 8-bit offset
                length = 2
            else:
                if self.get_bit():
                    self.get_bit()
                    d0 = self.get_sized() + 1
                else:
                    d0 = 0
                d0 += 3
                length = d0
                self.get_bit()                         # selects the width below
                self.d2 = (self.d2 + 1) & 0xFFFF
                d0 = self.get_sized()

            src = a1 + d0 - 1
            copy_from_output(src, length)

        return bytes(out)


def unpack(rom: bytes, src: int) -> bytes:
    return Unpacker(rom, src).run()


# --------------------------------------------------------------------------
def find_sites(rom: bytes, target: int = 0x28A2):
    needle = b"\x4e\xb8" + struct.pack(">H", target)
    out, pos = [], 0
    while True:
        pos = rom.find(needle, pos)
        if pos < 0:
            return out
        if pos >= 12 and rom[pos - 12:pos - 10] == b"\x41\xf9" \
                     and rom[pos - 6:pos - 4] == b"\x43\xf9":
            src = struct.unpack_from(">I", rom, pos - 10)[0]
            dst = struct.unpack_from(">I", rom, pos - 4)[0]
            if src < len(rom):
                out.append((pos, src, dst))
        pos += 2


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--at", help="unpack a single block at this hex ROM offset")
    ap.add_argument("--all", action="store_true", help="unpack every block found")
    ap.add_argument("--out", help="write the result here (with --at)")
    args = ap.parse_args()

    rom = open(args.rom, "rb").read()

    if args.at:
        src = int(args.at, 16)
        data = unpack(rom, src)
        print(f"{src:06X}: {len(data)} bytes unpacked")
        if args.out:
            open(args.out, "wb").write(data)
        return

    if not args.all:
        ap.error("pass --at or --all")

    sites = find_sites(rom)
    seen, ok, bad = set(), 0, []
    for _, src, dst in sorted(sites, key=lambda t: t[1]):
        if src in seen:
            continue
        seen.add(src)
        packed = struct.unpack_from("<H", rom, src)[0]
        declared = rom[src + 2 + packed - 1] << 8 | rom[src + 2 + packed - 2]
        try:
            data = unpack(rom, src)
            status = "ok" if len(data) == declared else f"SIZE {len(data)}!={declared}"
        except Exception as exc:                        # noqa: BLE001 - report, keep going
            status = f"FAIL {exc}"
        if status == "ok":
            ok += 1
        else:
            bad.append((src, status))
        print(f"  {src:06X} -> {dst:06X}  packed {packed:6d}  out {declared:6d}  {status}")

    print(f"\n{ok}/{len(seen)} blocks unpacked to exactly the declared size")
    for src, why in bad:
        print(f"  {src:06X}: {why}")


if __name__ == "__main__":
    main()
