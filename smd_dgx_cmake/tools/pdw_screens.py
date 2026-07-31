"""Dump every packed graphics block of The Pirates of Dark Water as PNG.

Most blocks are not bare tiles — they are small containers that the two
loaders read like this:

    sub_4016 (raw upload)      d7 = (a0)          ; offset to the tile data
                               a0 += d7
                               d0 *= 16           ; d0 counts whole tiles

    sub_4022 (nametable fill)  d1 = 4(a0, d2*2)   ; offset to tilemap #d2
                               a0 += d1
                               per cell: entry = *(a0)++ + d4

So the header is `u16 tiles_off, u16 ?, u16 map0_off, ...`, the tilemap sits
between the header and the tile data, and every map entry is biased by a base
attribute the caller supplies. That bias is why a map alone looks wrong: the
tile field in the stored entry is relative.

Container detection here is structural, not a guess about content: the tile
area has to divide into whole 32-byte tiles and the map area into whole
entries, and the header words have to be ordered and in range. Blocks that
fail are dumped as a plain tile sheet instead.

Usage:  python pdw_screens.py <rom> <outdir> [--palette 4DCCA,4DC0A,4D94A,4D96A]
"""
from __future__ import annotations

import argparse
import json
import struct
import zlib
from pathlib import Path

from pdw_unpack import unpack, find_sites

PORTRAIT_TABLE, PORTRAIT_COUNT = 0x4D73E, 21
DAC = (0, 52, 87, 116, 144, 172, 206, 255)
# Palette lines the menu and dialogue screens actually load; see RE_NOTES.md.
DEFAULT_PALETTES = (0x4DCCA, 0x4DC0A, 0x4D94A, 0x4D96A)
PLANE_WIDTH = 40          # every screen in this game is 40 cells wide


def be16(b, a): return int.from_bytes(b[a:a + 2], "big")
def be32(b, a): return int.from_bytes(b[a:a + 4], "big")


def palette(rom, at):
    return [(DAC[(v >> 1) & 7], DAC[(v >> 5) & 7], DAC[(v >> 9) & 7])
            for v in (be16(rom, at + 2 * i) for i in range(16))]


def write_png(path, w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    path.write_bytes(b"\x89PNG\r\n\x1a\n"
                     + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(raw, 9))
                     + chunk(b"IEND", b""))


def blit(dst, dw, ox, oy, tile, pal, hflip=False, vflip=False):
    for y in range(8):
        sy = 7 - y if vflip else y
        for x in range(8):
            sx = 7 - x if hflip else x
            b = tile[sy * 4 + (sx >> 1)]
            idx = (b >> 4) if (sx & 1) == 0 else (b & 0xF)
            if idx:
                p = ((oy + y) * dw + ox + x) * 4
                dst[p:p + 4] = bytes((*pal[idx], 255))


def tile_sheet(tiles, pal, per_row=16):
    n = len(tiles) // 32
    rows = (n + per_row - 1) // per_row
    w, h = per_row * 8, rows * 8
    buf = bytearray(w * h * 4)
    for i in range(n):
        blit(buf, w, (i % per_row) * 8, (i // per_row) * 8, tiles[i * 32:i * 32 + 32], pal)
    return w, h, buf


def parse_container(blob):
    """Return (tiles_off, map_off) when the block is structurally a container."""
    if len(blob) < 8:
        return None
    tiles_off, map_off = be16(blob, 0), be16(blob, 4)
    if not (6 <= map_off < tiles_off <= len(blob)):
        return None
    if (tiles_off - map_off) % 2 or (len(blob) - tiles_off) % 32:
        return None
    if (tiles_off - map_off) // 2 % PLANE_WIDTH:
        return None
    return tiles_off, map_off


def render_map(blob, tiles_off, map_off, pals):
    """Rasterise the stored tilemap. Tile numbers are relative to the block's
    own tile data, which is exactly what the caller's base attribute makes
    absolute at run time, so rendering them directly is correct here."""
    entries = (tiles_off - map_off) // 2
    cols, rows = PLANE_WIDTH, entries // PLANE_WIDTH
    tiles = blob[tiles_off:]
    ntiles = len(tiles) // 32
    w, h = cols * 8, rows * 8
    buf = bytearray(w * h * 4)
    for i in range(entries):
        e = be16(blob, map_off + i * 2)
        t = e & 0x7FF
        if t >= ntiles:
            continue
        blit(buf, w, (i % cols) * 8, (i // cols) * 8, tiles[t * 32:t * 32 + 32],
             pals[(e >> 13) & 3], bool(e & 0x800), bool(e & 0x1000))
    return w, h, buf


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("outdir")
    ap.add_argument("--palette", default=",".join(f"{p:X}" for p in DEFAULT_PALETTES),
                    help="four ROM offsets, one per palette line")
    args = ap.parse_args()

    rom = Path(args.rom).read_bytes()
    out = Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)
    pals = [palette(rom, int(p, 16)) for p in args.palette.split(",")]

    # Literal call sites plus the one real offset table.
    sources = {src for _, src, _ in find_sites(rom)}
    sources |= {be32(rom, PORTRAIT_TABLE + 4 * i) for i in range(PORTRAIT_COUNT)}

    manifest, containers = [], 0
    for src in sorted(sources):
        try:
            blob = unpack(rom, src)
        except Exception as exc:
            print(f"{src:06X}  unpack failed: {exc}")
            continue

        entry = {"at": f"0x{src:06X}", "unpacked": len(blob)}
        shape = parse_container(blob)
        if shape:
            containers += 1
            tiles_off, map_off = shape
            cols = PLANE_WIDTH
            rows = (tiles_off - map_off) // 2 // cols
            write_png(out / f"{src:06X}_map.png", *render_map(blob, tiles_off, map_off, pals))
            write_png(out / f"{src:06X}_tiles.png",
                      *tile_sheet(blob[tiles_off:], pals[0]))
            entry.update(kind="container", map=[cols, rows],
                         tiles=(len(blob) - tiles_off) // 32)
            print(f"{src:06X}  container: {cols}x{rows} map, "
                  f"{(len(blob) - tiles_off) // 32} tiles")
        else:
            write_png(out / f"{src:06X}_tiles.png", *tile_sheet(blob, pals[0]))
            entry.update(kind="tiles", tiles=len(blob) // 32)
            print(f"{src:06X}  tiles: {len(blob) // 32}")
        manifest.append(entry)

    (out / "manifest.json").write_text(json.dumps({
        "codec": "reverse_lz", "plane_width": PLANE_WIDTH,
        "palettes": args.palette, "blocks": manifest}, indent=2))
    print(f"\n{len(manifest)} blocks ({containers} containers) -> {out}")


if __name__ == "__main__":
    main()
