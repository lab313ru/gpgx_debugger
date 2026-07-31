"""Extract the dialogue/hero portraits of The Pirates of Dark Water.

Everything here is derived statically from the ROM — no emulator, no VRAM
dumps. The chain is:

    0x4D73E   21 longs, each a packed graphics block   (loader at 0x4D6D6)
    0x4D96A   21 palettes, 16 colours each, index-matched to the table above
    reverse_lz                                          (unpacker at 0x28A2)

The loader is what pins the geometry down. It uploads 0x300 words to VRAM
0x8D80 and then writes an 8 x 6 nametable rectangle whose first entry is
0x646C: palette line 3, tile 0x46C — and 0x46C * 32 == 0x8D80, which is the
same address it just wrote. So a portrait is 8 x 6 tiles = 64 x 48 pixels,
laid out row-major, drawn through palette line 3.

Palette line 3 is loaded by 0x4D868 from whatever `a2` the caller set, and
the callers walk the array at 0x4D96A in lockstep with the portrait index —
the hero-select screen does `move.w (FF0EB0),d0 / lsl.w #2,d0` to pick both.

Usage:  python pdw_portraits.py <rom> <outdir>
"""
from __future__ import annotations

import json
import struct
import sys
import zlib
from pathlib import Path

from pdw_unpack import unpack

PORTRAIT_TABLE = 0x4D73E
PORTRAIT_COUNT = 21
PALETTE_ARRAY = 0x4D96A
TILES_W, TILES_H = 8, 6

# The menu text at 0x527A0 names the three playable heroes in table order,
# and the dialogue strings at 0x52804+ name portraits 15..20. The rest stay
# numbered rather than guessed at.
KNOWN_NAMES = {
    0: "ren", 1: "tula", 2: "ioz", 3: "niddler",
    15: "konk", 16: "tork", 17: "joat", 18: "mantus", 19: "bloth",
    20: "dark_dweller",
}

# Mega Drive DAC output for the three-bit channels; not a linear ramp.
DAC = (0, 52, 87, 116, 144, 172, 206, 255)


def be32(rom: bytes, at: int) -> int:
    return int.from_bytes(rom[at:at + 4], "big")


def palette(rom: bytes, at: int) -> list[tuple[int, int, int]]:
    """16 colours in bus format `0000 BBB0 GGG0 RRR0`."""
    out = []
    for i in range(16):
        v = int.from_bytes(rom[at + 2 * i:at + 2 * i + 2], "big")
        out.append((DAC[(v >> 1) & 7], DAC[(v >> 5) & 7], DAC[(v >> 9) & 7]))
    return out


def render(tiles: bytes, pal) -> tuple[int, int, bytearray]:
    """8x6 tiles of 4bpp, row-major, into RGBA. Index 0 is transparent."""
    w, h = TILES_W * 8, TILES_H * 8
    buf = bytearray(w * h * 4)
    for n in range(TILES_W * TILES_H):
        ox, oy = (n % TILES_W) * 8, (n // TILES_W) * 8
        tile = tiles[n * 32:n * 32 + 32]
        for row in range(8):
            for col in range(4):
                b = tile[row * 4 + col]
                for half, idx in ((0, b >> 4), (1, b & 0xF)):
                    p = ((oy + row) * w + ox + col * 2 + half) * 4
                    if idx:
                        buf[p:p + 4] = bytes((*pal[idx], 255))
    return w, h, buf


def write_png(path: Path, w: int, h: int, rgba: bytearray) -> None:
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(tag: bytes, data: bytes) -> bytes:
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rom = Path(sys.argv[1]).read_bytes()
    outdir = Path(sys.argv[2])
    outdir.mkdir(parents=True, exist_ok=True)

    manifest = []
    for i in range(PORTRAIT_COUNT):
        src = be32(rom, PORTRAIT_TABLE + 4 * i)
        tiles = unpack(rom, src)
        expected = TILES_W * TILES_H * 32
        if len(tiles) != expected:
            raise SystemExit(f"portrait {i}: got {len(tiles)} bytes, expected {expected}")

        pal_at = PALETTE_ARRAY + 0x20 * i
        name = f"{i:02d}_{KNOWN_NAMES.get(i, 'unknown')}"
        write_png(outdir / f"{name}.png", *render(tiles, palette(rom, pal_at)))

        packed = int.from_bytes(rom[src:src + 2], "little")
        manifest.append({
            "index": i,
            "name": name,
            "tiles_at": f"0x{src:06X}",
            "packed_end": f"0x{src + 2 + packed:06X}",
            "palette_at": f"0x{pal_at:06X}",
            "size": [TILES_W * 8, TILES_H * 8],
        })
        print(f"[{i:2}] {src:06X} -> {name}.png   palette {pal_at:06X}")

    (outdir / "manifest.json").write_text(json.dumps({
        "source": "The Pirates of Dark Water (MD)",
        "codec": "reverse_lz (unpacker at 0x28A2)",
        "table": f"0x{PORTRAIT_TABLE:06X}",
        "palettes": f"0x{PALETTE_ARRAY:06X}",
        "vram": {"tiles": "0x8D80", "nametable_first_entry": "0x646C", "palette_line": 3},
        "portraits": manifest,
    }, indent=2))
    print(f"\n{PORTRAIT_COUNT} portraits -> {outdir}")


if __name__ == "__main__":
    main()
