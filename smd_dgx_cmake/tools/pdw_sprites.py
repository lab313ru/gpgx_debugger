"""Find and decode the sprite piece lists of The Pirates of Dark Water.

`sub_4096` takes a0 = piece list, d0/d1 = world position, d5 = the VRAM byte
address of the frame's tiles, d6 = attribute bits (bit 15 priority, 14-13
palette, 12 vflip, 11 hflip). It skips two bytes and then walks 10-byte
records, stopping *after* the record whose size word has bit 15 set — the
terminator is a flag on the last real piece, not a separate entry.

    +0  u16  size, bits 11-10 = width-1 in cells, 9-8 = height-1
             bit 15 marks the last piece
    +2  s8   x offset
    +3  s8   y offset
    +4  s16  x offset to use when the sprite is h-flipped
    +6  s16  y offset to use when v-flipped
    +8  u16  tile offset, added to (d5 >> 5) | d6

The two mirror fields are the useful part. They are not free-form: the game
precomputes them as -(x + width*8) and -(y + height*8) so the flipped draw
needs no arithmetic. That makes them a very strong structural signature —
a run of records where both identities hold for every piece is a piece list
and essentially cannot be anything else. Searching on the size word alone
gives thousands of false hits; searching on the identity gives real lists.

Usage:  python pdw_sprites.py <rom> [--at 72258] [--json out.json]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

RECORD = 10


def be16(b, a): return int.from_bytes(b[a:a + 2], "big")
def s16(v): return v - 0x10000 if v & 0x8000 else v
def s8(v): return v - 0x100 if v & 0x80 else v


def decode(rom: bytes, at: int, limit: int = 64):
    """Walk one piece list. Returns None if it does not hold together."""
    pieces = []
    a = at + 2
    while len(pieces) < limit:
        if a + RECORD > len(rom):
            return None
        size = be16(rom, a)
        w = ((size >> 10) & 3) + 1
        h = ((size >> 8) & 3) + 1
        x, y = s8(rom[a + 2]), s8(rom[a + 3])
        mx, my = s16(be16(rom, a + 4)), s16(be16(rom, a + 6))
        # The identity that makes this a piece list rather than arbitrary bytes.
        if mx != -(x + w * 8) or my != -(y + h * 8):
            return None
        pieces.append({"w": w, "h": h, "x": x, "y": y, "tile": be16(rom, a + 8)})
        if size & 0x8000:
            return pieces
        a += RECORD
    return None


def consistent_tiles(pieces) -> bool:
    """Tile offsets normally run consecutively, one step per cell. Used only
    to rank finds, never to reject them — a few frames do share tiles."""
    t = pieces[0]["tile"]
    for p in pieces:
        if p["tile"] != t:
            return False
        t += p["w"] * p["h"]
    return True


def scan(rom: bytes, min_pieces: int = 2):
    found, a = [], 0x200
    while a < len(rom) - 12:
        pieces = decode(rom, a)
        if pieces and len(pieces) >= min_pieces:
            found.append((a, pieces))
            a += 2 + len(pieces) * RECORD
        else:
            a += 2
    return found


def extent(pieces):
    x0 = min(p["x"] for p in pieces)
    y0 = min(p["y"] for p in pieces)
    x1 = max(p["x"] + p["w"] * 8 for p in pieces)
    y1 = max(p["y"] + p["h"] * 8 for p in pieces)
    return x0, y0, x1 - x0, y1 - y0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--at", help="decode a single list at this hex offset")
    ap.add_argument("--json", help="write all finds here")
    ap.add_argument("--min", type=int, default=2, help="minimum pieces (default 2)")
    args = ap.parse_args()
    rom = Path(args.rom).read_bytes()

    if args.at:
        at = int(args.at, 16)
        pieces = decode(rom, at)
        if not pieces:
            raise SystemExit(f"{at:06X}: not a piece list")
        x, y, w, h = extent(pieces)
        print(f"{at:06X}: {len(pieces)} pieces, {w}x{h} px at ({x},{y})"
              f"{'' if consistent_tiles(pieces) else '  [tiles not consecutive]'}")
        for i, p in enumerate(pieces):
            print(f"  [{i}] {p['w']}x{p['h']} cells  x={p['x']:+4} y={p['y']:+4}"
                  f"  tile+{p['tile']:04X}")
        return

    found = scan(rom, args.min)
    clean = [f for f in found if consistent_tiles(f[1])]
    print(f"{len(found)} piece lists ({len(clean)} with consecutive tile numbering)")
    by_size = sorted(found, key=lambda f: -len(f[1]))[:10]
    print("\nlargest:")
    for at, pieces in by_size:
        x, y, w, h = extent(pieces)
        print(f"  {at:06X}  {len(pieces):3} pieces  {w:3}x{h:<3} px")

    if args.json:
        Path(args.json).write_text(json.dumps([
            {"at": f"0x{at:06X}", "pieces": p,
             "extent": dict(zip(("x", "y", "w", "h"), extent(p))),
             "consecutive_tiles": consistent_tiles(p)}
            for at, p in found], indent=1))
        print(f"\n-> {args.json}")


if __name__ == "__main__":
    main()
