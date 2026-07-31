"""Dump the graphics a Mega Drive game has loaded, straight out of a running
emulator, via the SMD DGX bridge.

Why runtime and not the ROM: assets in a Mega Drive cartridge are almost always
compressed, and every game rolls its own scheme. Reversing the decompressor is
a project of its own; VRAM at the moment the scene is on screen already holds
the decompressed result. So: reach the scene, run this, get usable PNGs — no
knowledge of the game's formats required. The trade is coverage, since you only
ever get what is currently loaded, which is why the tool is meant to be run
once per scene (save states make that repeatable).

Outputs, per run:
  palettes.png / palettes.json   the four CRAM palettes
  tiles_pal<N>.png               every VRAM tile as a sheet, one per palette
  plane_a.png / plane_b.png      the nametables as they are composed on screen
  vram.bin / cram.bin            raw, for a port to consume directly
  manifest.json                  VDP mode, bases, sizes, tile count

Usage:
    python extract_assets.py --out DIR [--host H] [--port P] [--tiles-per-row N]
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import struct
import zlib

# --------------------------------------------------------------------------
# bridge
# --------------------------------------------------------------------------


class Bridge:
    def __init__(self, host: str, port: int) -> None:
        self.sock = socket.create_connection((host, port), timeout=30)
        self.buf = b""

    def cmd(self, line: str) -> str:
        self.sock.sendall(line.encode() + b"\n")
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise RuntimeError("bridge closed the connection")
            self.buf += chunk
        raw, _, self.buf = self.buf.partition(b"\n")
        reply = raw.decode(errors="replace").strip()
        if reply.startswith("err"):
            raise RuntimeError(reply[3:].strip() or "command failed")
        return reply[2:].strip() if reply.startswith("ok") else reply

    def read_region(self, rid: int, size: int, chunk: int = 0x8000) -> bytes:
        """Regions can be 64K+; the protocol caps a single read, so page it."""
        out = bytearray()
        while len(out) < size:
            n = min(chunk, size - len(out))
            out += bytes.fromhex(self.cmd(f"readregion {rid} {len(out):x} {n}"))
        return bytes(out)


# --------------------------------------------------------------------------
# PNG (stdlib only — no Pillow dependency for a tool meant to be run anywhere)
# --------------------------------------------------------------------------


def write_png(path: str, rgb: bytes, w: int, h: int) -> None:
    rows = bytearray()
    stride = w * 3
    for y in range(h):
        rows.append(0)                       # filter: none
        rows += rgb[y * stride:(y + 1) * stride]

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 6)))
        f.write(chunk(b"IEND", b""))


# --------------------------------------------------------------------------
# Mega Drive decoding
# --------------------------------------------------------------------------

def cram_to_rgb(packed: int) -> tuple[int, int, int]:
    """CRAM holds the core's packed 9-bit BBBGGGRRR, not the 16-bit bus format.

    Three bits scale to 0..224 (v << 5), matching what the debugger views show,
    so extracted art lines up with screenshots.
    """
    return (((packed >> 0) & 7) << 5,
            ((packed >> 3) & 7) << 5,
            ((packed >> 6) & 7) << 5)


def parse_palettes(cram: bytes) -> list[list[tuple[int, int, int]]]:
    # 64 entries, native 16-bit little-endian words (this region is not swapped)
    pals = []
    for p in range(4):
        pal = []
        for i in range(16):
            off = (p * 16 + i) * 2
            pal.append(cram_to_rgb(cram[off] | (cram[off + 1] << 8)))
        pals.append(pal)
    return pals


def tile_pixels(vram: bytes, index: int) -> list[int]:
    """One 8x8 tile as 64 palette indices. 4bpp, 4 bytes per row, high nibble
    is the left pixel. VRAM arrives in logical byte order already."""
    base = index * 32
    out = []
    for y in range(8):
        for x in range(4):
            b = vram[base + y * 4 + x]
            out.append(b >> 4)
            out.append(b & 0xF)
    return out


def render_tilesheet(vram: bytes, pal: list[tuple[int, int, int]],
                     per_row: int) -> tuple[bytes, int, int]:
    count = len(vram) // 32
    rows = (count + per_row - 1) // per_row
    w, h = per_row * 8, rows * 8
    img = bytearray(w * h * 3)
    for t in range(count):
        px = tile_pixels(vram, t)
        ox, oy = (t % per_row) * 8, (t // per_row) * 8
        for y in range(8):
            for x in range(8):
                r, g, b = pal[px[y * 8 + x]]
                o = ((oy + y) * w + ox + x) * 3
                img[o:o + 3] = bytes((r, g, b))
    return bytes(img), w, h


def decode_plane_geometry(regs: bytes, plane: str) -> tuple[int, int, int]:
    """Base address and size in cells — the same math the Plane Explorer uses."""
    h40 = bool(regs[0x0C] & 0x01)
    if plane == "a":
        base = (regs[0x02] & 0x38) << 10
    elif plane == "b":
        base = (regs[0x04] & 0x07) << 13
    else:
        raise ValueError(plane)

    scr = regs[0x10]
    hsz, vsz = scr & 3, (scr >> 4) & 3
    mh = hsz
    mv = ((vsz & 0x1) & ((~hsz & 0x02) >> 1)) | ((vsz & 0x02) & ((~hsz & 0x01) << 1))
    pw, ph = (mh + 1) * 32, (mv + 1) * 32
    if mh == 2:
        pw, ph = 32, 1                      # prohibited H setting
    elif mv == 2:
        pw, ph = 32, 32                     # prohibited V setting
    return base, pw, ph


def render_plane(vram: bytes, pals, base: int, pw: int, ph: int) -> tuple[bytes, int, int]:
    w, h = pw * 8, ph * 8
    img = bytearray(w * h * 3)
    for cy in range(ph):
        for cx in range(pw):
            off = (base + (cy * pw + cx) * 2) & 0xFFFF
            entry = (vram[off] << 8) | vram[off + 1]     # big-endian in logical order
            tile = entry & 0x7FF
            pal = pals[(entry >> 13) & 3]
            hflip, vflip = bool(entry & 0x0800), bool(entry & 0x1000)
            px = tile_pixels(vram, tile)
            for y in range(8):
                sy = 7 - y if vflip else y
                for x in range(8):
                    sx = 7 - x if hflip else x
                    r, g, b = pal[px[sy * 8 + sx]]
                    o = ((cy * 8 + y) * w + cx * 8 + x) * 3
                    img[o:o + 3] = bytes((r, g, b))
    return bytes(img), w, h


# --------------------------------------------------------------------------
def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--host", default=os.environ.get("SMD_DGX_HOST", "127.0.0.1"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("SMD_DGX_PORT", "27042")))
    ap.add_argument("--tiles-per-row", type=int, default=32)
    ap.add_argument("--no-planes", action="store_true", help="skip the nametable renders")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    br = Bridge(args.host, args.port)
    print("bridge:", br.cmd("status"))

    # Pausing makes the dump internally consistent: VRAM, CRAM and the registers
    # would otherwise be read a frame or two apart and disagree with each other.
    was_paused = "paused=1" in br.cmd("status")
    if not was_paused:
        br.cmd("pause")
    try:
        vram = br.read_region(3, 0x10000)
        cram = br.read_region(4, 0x80)
        regs = bytes.fromhex(br.cmd("vdp").split()[0].split("=", 1)[1])
    finally:
        if not was_paused:
            br.cmd("resume")

    open(os.path.join(args.out, "vram.bin"), "wb").write(vram)
    open(os.path.join(args.out, "cram.bin"), "wb").write(cram)

    pals = parse_palettes(cram)
    json.dump([[list(c) for c in p] for p in pals],
              open(os.path.join(args.out, "palettes.json"), "w"), indent=1)

    # palette swatch: 4 rows of 16, 16px cells
    sw, sh = 16 * 16, 4 * 16
    swatch = bytearray(sw * sh * 3)
    for p in range(4):
        for i in range(16):
            r, g, b = pals[p][i]
            for y in range(16):
                for x in range(16):
                    o = ((p * 16 + y) * sw + i * 16 + x) * 3
                    swatch[o:o + 3] = bytes((r, g, b))
    write_png(os.path.join(args.out, "palettes.png"), bytes(swatch), sw, sh)

    for p in range(4):
        img, w, h = render_tilesheet(vram, pals[p], args.tiles_per_row)
        write_png(os.path.join(args.out, f"tiles_pal{p}.png"), img, w, h)
    print(f"tiles: {len(vram)//32} at {args.tiles_per_row}/row")

    manifest = {
        "tile_count": len(vram) // 32,
        "h40": bool(regs[0x0C] & 1),
        "vdp_regs": regs.hex(),
    }

    if not args.no_planes:
        for name in ("a", "b"):
            base, pw, ph = decode_plane_geometry(regs, name)
            img, w, h = render_plane(vram, pals, base, pw, ph)
            write_png(os.path.join(args.out, f"plane_{name}.png"), img, w, h)
            manifest[f"plane_{name}"] = {"base": f"{base:04X}", "cells": [pw, ph]}
            print(f"plane {name.upper()}: base {base:04X}, {pw}x{ph} cells -> {w}x{h}px")

    json.dump(manifest, open(os.path.join(args.out, "manifest.json"), "w"), indent=1)
    print("written to", args.out)


if __name__ == "__main__":
    main()
