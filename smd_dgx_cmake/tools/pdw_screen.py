"""Rebuild the hero-select screen of The Pirates of Dark Water from ROM only.

This is the end-to-end proof that the formats are understood: it fills a VRAM
image the way `scene_hero_select` (0x4B4F4) does, writes the nametable the way
the loaders do, and rasterises plane A. No emulator, no VRAM dump.

Every number below is read out of the listing, not chosen to look right:

  container 0x53920  ->  66 tiles at VRAM 0x8020, 40x25 map at 0xC000, base 0x8401
                         (sub_4CB50 redraws the frame; the scene's own 0x52F28
                          is replaced before the sub-screen appears)
  container 0x537D6  ->  12 tiles at VRAM 0x8C00, 40x3  map at 0xCC80, base 0x8460
  0x52D02 (raw)      ->  16 tiles at VRAM 0xB9C0   two 9-patch sets, 0x5CE and 0x5D6
  0x5A516 (raw)      -> 215 tiles at VRAM 0x9380
  descriptor 0x5C272 ->  five raw uploads, including the font to 0xAE60
  portrait 0         ->  0x600 bytes at 0x8D80, 8x6 nametable at 0xC082 from 0x646C

Menu items come from `sub_4D3A0`, which draws the string, then frames it from
(col-1, row-1), then registers a slot with the handler in a1. The selected item
is drawn from the second 9-patch set (`d5 = 8`), whose highlight colour is
index 15 — the one the fire ramp animates.

Usage:  python pdw_screen.py <rom> <out.png> [--hero 0] [--fire-step 0]
"""
from __future__ import annotations

import argparse
from pathlib import Path

from pdw_unpack import unpack
from pdw_screens import DAC, be16, be32, blit, palette, write_png
from pdw_sprites import decode

PLANE_A = 0xC000
GLYPH_BASE, BOX_BASE = 0x573, 0x5CE
FIRE_RAMP = 0x4D87E
PORTRAIT_TABLE, PORTRAIT_PALETTES = 0x4D73E, 0x4D96A

# (col, row, string, palette bits) — straight out of 0x4CCFA..0x4CDB2.
TITLE = (12, 3, 0x5279C, 0x4000)
HEROES = [(0x0E, 5, 0x527F7), (0x16, 5, 0x527FB), (0x1F, 5, 0x52800)]
FOOTER = [(0x03, 0x19, 0x527D6, 0x0000), (0x15, 0x19, 0x527B5, 0x0000)]
SCORE = (0x4D368, 0x18, 1)          # sub_4D342: label, then BCD digits at col 24

# Per hero, from the handlers at 0x4CE54 / 0x4CFBC / 0x4D0BE. The figure is a
# *sprite*, and its packed block holds the piece list first and the tiles right
# after it — the `adda.w #$5C` in Ren's handler is just his list's length.
BIO = [0x52A0A, 0x52B11, 0x52C0E]
FIGURE = [(0x59698, 0xDA, 0x3C), (0x59AFC, 0xE3, 0x47), (0x59F32, 0xDE, 0x46)]
FIGURE_VRAM = 0xD000
# sub_4BA2E: one blink patch per hero, drawn over the portrait for ~11 frames
# out of every 26..153. Record is {u32 piece list, u16 tile base, u16 attrs};
# the position is hardcoded in each branch.
BLINK = [(0x4BAC4, 0x20, 0x10), (0x4BACC, 0x16, 0x0F), (0x4BAD4, 0x20, 0x18)]

# sub_4C120: the selection marker is one 8x8 tile drawn at the four corners of
# a 24-pixel box, with the flips that turn one corner into all four. Position
# from an 8-entry grid indexed by FF0EB6; frame from a 2-entry table that
# FF0EB8 picks between. It goes through sub_4216, which unlike sprite_emit
# takes d5 as a tile *index* and points a0 at the list header rather than past
# it, so the tile base needs no shifting here.
MARKER_POSITIONS, MARKER_FRAMES = 0x4C3D8, 0x5C2AE

# The item grid, drawn by 0x4BFF4. Slot nametable positions come from 0x4C3B2
# (negative-terminated), the item in each slot from a per-hero table of 18
# bytes at 0x4C1E4, and the icon itself is tilemap #id inside container
# 0x5A516 - 4x4 cells for ids under 14, 4x3 above. The marker grid at 0x4C3D8
# lines up with the first eight slots.
#
# This is sub-screen 1 of the same group: scene 0 dispatches through
# 0x4DD4A[FF1F8E], entry 1 is 0x4BE9E, and its title string reads
# "SELECT AN ITEM." The map screens reach it by setting FF1F8E = 1 at 0x5CD58
# and calling into 0x4B470. Same frame, same VRAM, different screen - which is
# why the icon tiles are uploaded by scenes 0, 1 and 2 alike.
ITEM_SLOTS, ITEM_TABLE, ITEM_CONTAINER = 0x4C3B2, 0x4C1E4, 0x5A516
ITEM_TILE_BASE = 0x49C
ITEM_TITLE = 0x529E6                                  # {col 16, row 4}
ITEM_FOOTER = [(0x03, 0x19, 0x529FA), (0x15, 0x19, 0x4D330)]
MARKER_CORNERS = ((0, 0, 0x0000), (0x18, 0, 0x0800),
                  (0x18, 0x18, 0x1800), (0, 0x18, 0x1000))
WRAP_COL = 0x25                     # sub_4D5E6 wraps past this


class Screen:
    def __init__(self, rom):
        self.rom = rom
        self.mem = bytearray(0x10000)

    def upload(self, data, dest, nbytes):
        self.mem[dest:dest + nbytes] = data[:nbytes]

    def upload_container(self, blob, dest, tiles):
        self.upload(blob[be16(blob, 0):], dest, tiles * 32)

    def fill_map(self, blob, dest, base, cols, rows):
        p = be16(blob, 4)
        for row in range(rows):
            a = dest + row * 0x80
            for _ in range(cols):
                self.mem[a:a + 2] = ((be16(blob, p) + base) & 0xFFFF).to_bytes(2, "big")
                p += 2
                a += 2

    def cell(self, col, row, entry):
        a = PLANE_A + row * 0x80 + col * 2
        self.mem[a:a + 2] = (entry & 0xFFFF).to_bytes(2, "big")

    def text(self, col, row, s, attr):
        for i, ch in enumerate(s):
            c = ord(ch)
            tile = 0 if c == 0x20 else GLYPH_BASE + (c - 0x21)
            self.cell(col + i, row, tile | attr | 0x8000)

    def box(self, col, row, width, attr, set_offset):
        """sub_4D4C6: corners 4/5/7/6, edges 0/2, sides 3/1."""
        b = BOX_BASE + set_offset
        for dy, (left, right) in enumerate(((4, 5), (3, 1), (7, 6))):
            self.cell(col, row + dy, (b + left) | attr)
            self.cell(col + 1 + width, row + dy, (b + right) | attr)
        for i in range(width):
            self.cell(col + 1 + i, row, (b + 0) | attr)
            self.cell(col + 1 + i, row + 2, (b + 2) | attr)

    def item(self, col, row, at, attr, selected=False):
        s = zstr(self.rom, at)
        self.text(col, row, s, attr)
        # sub_4D3A0 counts with `tst.b 1(a0,d4.w)` from d4 = 1, giving len-1,
        # and draw_box_9patch then runs its middle loop d4+1 times — so the
        # frame spans exactly len(s) columns and clears the text on both sides.
        self.box(col - 1, row - 1, len(s), attr, 8 if selected else 0)

    def flow_text(self, col, row, at, attr, indent=3):
        """sub_4D5D4 / sub_4D5E0: word wrap past column 0x25, with 0x0D as a
        plain newline and 0x0E as a newline that re-applies the indent."""
        rom = self.rom
        base, c, r, off = col, col + indent, row, indent
        i = at
        while rom[i] not in (0, 0x0C):
            ch = rom[i]
            if ch in (0x0D, 0x0E):
                off = indent if ch == 0x0E else 0
                c, r, i = base + off, r + 1, i + 1
                continue
            j = i                       # measure the next word
            while rom[j] not in (0, 0x20, 0x0C, 0x0D, 0x0E):
                j += 1
            if off + (j - i) + 1 > WRAP_COL:
                c, r, off = base, r + 1, 0
                continue
            for k in range(i, j + (1 if rom[j] == 0x20 else 0)):
                self.text(c, r, chr(rom[k]), attr)
                c += 1
                off += 1
            i = j + (1 if rom[j] == 0x20 else 0)

    def sprite(self, pieces, vram, x, y, flags):
        """sprite_emit does `lsr.w #5,d5` then `or.w d6,d5`, so the attribute
        word is (VRAM byte address >> 5) | flags and each piece adds its own
        tile offset on top. Screen position is d0/d1 plus the piece offset —
        the +0x80 the emitter adds is the VDP's sprite bias and cancels out."""
        attr = (vram >> 5) | flags
        return [(x + p["x"], y + p["y"], p["w"], p["h"], p["tile"], attr)
                for p in pieces]

    def render(self, pals, sprites=(), cols=40, rows=28):
        w, h = cols * 8, rows * 8
        # VDP register 7 is 0 on this screen, so the backdrop is palette 0
        # colour 0 - the dark olive the panels show. Leaving it transparent
        # made every cream glyph invisible against a white page.
        buf = bytearray(bytes((*pals[0][0], 255)) * (w * h))
        for row in range(rows):
            for col in range(cols):
                e = be16(self.mem, PLANE_A + row * 0x80 + col * 2)
                t = e & 0x7FF
                blit(buf, w, col * 8, row * 8, self.mem[t * 32:t * 32 + 32],
                     pals[(e >> 13) & 3], bool(e & 0x800), bool(e & 0x1000))
        # Sprites sit on top. Cells within a piece run down each column first,
        # which is how the VDP walks a multi-cell sprite.
        for sx, sy, pw, ph, tile, attr in sprites:
            base = (attr & 0x7FF) + tile
            for cx in range(pw):
                for cy in range(ph):
                    t = base + cx * ph + cy
                    blit(buf, w, sx + cx * 8, sy + cy * 8,
                         self.mem[t * 32:t * 32 + 32], pals[(attr >> 13) & 3])
        return w, h, buf


def zstr(rom, at):
    end = at
    while rom[end]:
        end += 1
    return rom[at:end].decode("latin1")


def descriptors(rom, at):
    out = []
    while True:
        src = be32(rom, at)
        if src & 0x80000000:
            return out
        out.append((src, be16(rom, at + 4), be16(rom, at + 6)))
        at += 8


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("out")
    ap.add_argument("--hero", type=int, default=0, choices=(0, 1, 2))
    ap.add_argument("--fire-step", type=int, default=0, help="0..15 of the flame ramp")
    ap.add_argument("--lit", type=int, default=4,
                    help="which item burns: 0-2 heroes, 3 MAP SCREEN, 4 START LEVEL")
    ap.add_argument("--blink", action="store_true",
                    help="draw the hero's blink patch (sub_4BA2E)")
    ap.add_argument("--marker", type=int, default=None,
                    help="draw the selection marker at grid slot 0..7 (sub_4C120)")
    ap.add_argument("--marker-frame", type=int, default=0, choices=(0, 1))
    ap.add_argument("--items", action="store_true",
                    help="draw the item grid over this frame (NOT its real screen)")
    args = ap.parse_args()
    rom = Path(args.rom).read_bytes()
    s = Screen(rom)

    a = unpack(rom, 0x52F28)
    s.upload_container(a, 0x8020, 95)
    s.fill_map(a, PLANE_A, 0x8401, 40, 25)
    b = unpack(rom, 0x537D6)
    s.upload_container(b, 0x8C00, 12)
    s.fill_map(b, 0xCC80, 0x8460, 40, 3)
    s.upload_container(rom[0x52D02:], 0xB9C0, 16)
    s.upload_container(rom[0x5A516:], 0x9380, 215)
    for src, words, dest in descriptors(rom, 0x5C272):
        s.upload(rom[src:], dest, words * 2)

    s.upload(unpack(rom, be32(rom, PORTRAIT_TABLE + 4 * args.hero)), 0x8D80, 0x600)
    for i in range(48):
        s.cell(1 + i % 8, 1 + i // 8, 0x646C + i)

    label, digit_col, digit_row = SCORE
    s.text(be16(rom, label), be16(rom, label + 2), zstr(rom, label + 4), 0x4000)
    s.text(digit_col, digit_row, "00000000", 0x4000)
    if args.items:
        s.text(be16(rom, ITEM_TITLE), be16(rom, ITEM_TITLE + 2),
               zstr(rom, ITEM_TITLE + 4), 0x4000)
        for col, row, at in ITEM_FOOTER:
            s.item(col, row, at, 0x0000)
    else:
        s.text(TITLE[0], TITLE[1], zstr(rom, TITLE[2]), TITLE[3])
        s.flow_text(2, 8, BIO[args.hero], 0x2000)
        for n, (col, row, at) in enumerate(HEROES):
            s.item(col, row, at, 0x4000, selected=(n == args.hero))
        for col, row, at, attr in FOOTER:
            s.item(col, row, at, attr)

    # Line 1 settles on 0x4DC6A+ (the handler's last load), not the 0x4DC0A
    # it passes through while fading in.
    pals = [palette(rom, p) for p in
            (0x4DCCA, 0x4DC6A + 0x20 * args.hero, 0x4D94A,
             PORTRAIT_PALETTES + 0x20 * args.hero)]
    c = be16(rom, FIRE_RAMP + 2 * (args.fire_step & 15))
    for line in (0, 1, 2):
        pals[line][15] = (DAC[(c >> 1) & 7], DAC[(c >> 5) & 7], DAC[(c >> 9) & 7])

    src, fx, fy = FIGURE[args.hero]
    block = unpack(rom, src)
    pieces = decode(block, 0)
    if pieces is None:
        raise SystemExit(f"figure {src:06X}: piece list did not validate")
    figure_tiles = block[2 + len(pieces) * 10:]
    s.upload(figure_tiles, FIGURE_VRAM, len(figure_tiles))
    sprites = s.sprite(pieces, FIGURE_VRAM, fx, fy, 0x2000)

    if args.blink:
        rec, bx, by = BLINK[args.hero]
        blink = decode(rom, be32(rom, rec))
        if blink is None:
            raise SystemExit(f"blink {rec:06X}: piece list did not validate")
        base, attr = be16(rom, rec + 4), be16(rom, rec + 6)
        # The patch tiles are already in VRAM from the screen's descriptor list.
        sprites += s.sprite(blink, base, bx, by, attr & 0xF800)

    if args.items:
        # sub_4D29E clears 39x15 cells from 0xC402 before the mode switches.
        for row in range(8, 23):
            for col in range(1, 40):
                s.cell(col, row, 0)
        blob = rom[ITEM_CONTAINER:]
        slot = ITEM_SLOTS
        n = 0
        while not be16(rom, slot) & 0x8000:
            pos = be16(rom, slot)
            item = rom[ITEM_TABLE + args.hero * 0x12 + n]
            if item < 0x80:
                rows_ = 4 if item < 14 else 3
                off = be16(blob, 4 + 2 * item)
                col, row = (pos // 2) % 64, (pos // 2) // 64
                for i in range(4 * rows_):
                    e = be16(blob, off + 2 * i) + ITEM_TILE_BASE
                    s.cell(col + i % 4, row + i // 4, e)
            slot += 2
            n += 1

    if args.marker is not None:
        slot = MARKER_POSITIONS + 4 * args.marker
        mx, my = be16(rom, slot), be16(rom, slot + 2)
        rec = MARKER_FRAMES + 8 * args.marker_frame
        corner = decode(rom, be32(rom, rec))
        if corner is None:
            raise SystemExit(f"marker {rec:06X}: piece list did not validate")
        tile, extra = be16(rom, rec + 4), be16(rom, rec + 6)
        for dx, dy, flip in MARKER_CORNERS:
            sprites += [(mx + dx + p["x"], my + dy + p["y"], p["w"], p["h"],
                         p["tile"], tile | flip | extra) for p in corner]

    write_png(Path(args.out), *s.render(pals, sprites))
    print(f"hero {args.hero}, fire step {args.fire_step} -> {args.out}")


if __name__ == "__main__":
    main()
