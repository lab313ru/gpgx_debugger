"""Walk the scene table of The Pirates of Dark Water and report what each
scene loads: packed blocks, raw uploads, tilemaps, palettes, VDP registers.

The table at 0x4B4CC holds 10 code entry points. Scene setup is written as a
straight run of calls, so the assets can be recovered by looking for the
call idioms and reading back the immediate arguments — no CFG walk needed.

This deliberately reads *arguments*, not control flow: it finds each call site
and scans backwards a short distance for the `lea`/`moveq`/`move.w #imm` that
set the registers. A branch could in principle put a different value in a
register than the nearest preceding immediate, so treat the output as a strong
lead rather than proof; every fact worth relying on should be confirmed in the
listing. In practice the setup code is linear and this matches what scene 0
does exactly.

Usage:  python pdw_scenes.py <rom> [--json scenes.json]
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

SCENE_TABLE, SCENE_COUNT = 0x4B4CC, 10
WINDOW = 0x600          # bytes of each routine to look at
BACK = 28               # how far back to look for an argument


def be16(b, a): return int.from_bytes(b[a:a + 2], "big")
def be32(b, a): return int.from_bytes(b[a:a + 4], "big")
def s16(v): return v - 0x10000 if v & 0x8000 else v


class Reader:
    def __init__(self, rom): self.rom = rom

    def arg_long(self, at, opcode):
        """Nearest preceding `lea (imm).l,aN` before `at`."""
        for back in range(2, BACK, 2):
            if be16(self.rom, at - back) == opcode:
                return be32(self.rom, at - back + 2)
        return None

    def arg_imm(self, at, opcode):
        """Nearest preceding `move.w #imm,dN`."""
        for back in range(2, BACK, 2):
            if be16(self.rom, at - back) == opcode:
                return be16(self.rom, at - back + 2)
        return None

    def arg_moveq(self, at, reg):
        want = 0x7000 | (reg << 9)
        for back in range(2, BACK, 2):
            w = be16(self.rom, at - back)
            if w & 0xFF00 == want:
                return w & 0xFF
        return None


LEA = {0: 0x41F9, 1: 0x43F9, 2: 0x45F9, 3: 0x47F9}
MOVEW = {0: 0x303C, 1: 0x323C, 2: 0x343C, 3: 0x363C,
         4: 0x383C, 5: 0x3A3C, 6: 0x3C3C, 7: 0x3E3C}


def scrape(rom, start, seen, depth=0):
    """Collect asset references in one routine, following calls one level."""
    r = Reader(rom)
    out = {"unpack": [], "upload": [], "tilemap": [], "palette": [],
           "vdp_regs": [], "desc_list": [], "portrait": []}
    end = min(start + WINDOW, len(rom) - 8)
    calls = []

    a = start
    while a < end:
        w = be16(rom, a)

        if w == 0x4EB8 and be16(rom, a + 2) == 0x28A2:
            src = r.arg_long(a, LEA[0])
            if src is not None:
                out["unpack"].append(f"0x{src:06X}")
        elif w == 0x4EB8 and be16(rom, a + 2) == 0x4016:
            out["upload"].append({"src": _hex(r.arg_long(a, LEA[0])),
                                  "tiles": r.arg_imm(a, MOVEW[0]),
                                  "vram": _hex16(r.arg_imm(a, MOVEW[1]))})
        elif w == 0x4EB8 and be16(rom, a + 2) == 0x4022:
            out["tilemap"].append({"src": _hex(r.arg_long(a, LEA[0])),
                                   "vdp": _hex16(r.arg_imm(a, MOVEW[0])),
                                   "base": _hex16(r.arg_imm(a, MOVEW[4])),
                                   "cols": _plus1(r.arg_imm(a, MOVEW[6])),
                                   "rows": _plus1(r.arg_imm(a, MOVEW[7]))})
        elif w == 0x4EB9 and be32(rom, a + 2) == 0x4D868:
            out["palette"].append({"line": r.arg_moveq(a, 0),
                                   "src": _hex(r.arg_long(a, LEA[2]))})
        elif w == 0x4EB9 and be32(rom, a + 2) == 0x4D6D6:
            idx = r.arg_moveq(a, 0)
            out["portrait"].append(idx // 4 if idx is not None else None)
        elif w == 0x4EB9 and be32(rom, a + 2) == 0x38EB8:
            # The in-game runner takes a descriptor in a0 and copies all 64
            # colours from +0xC in one go, so this fills every line at once.
            desc = r.arg_long(a, LEA[0])
            if desc is not None and desc + 0x10 <= len(rom):
                out["palette"].append({"line": "all", "src": _hex(be32(rom, desc + 0xC)),
                                       "via": f"descriptor 0x{desc:06X}"})
        elif w == 0x4EB8 and be16(rom, a + 2) == 0x4304:
            out["vdp_regs"].append(_hex(r.arg_long(a, LEA[0])))
        elif w == LEA[3] and be16(rom, a + 6) == 0x4A93:
            out["desc_list"].append(_hex(be32(rom, a + 2)))

        # one level of call following, which is where scene 0 keeps its
        # screen setup (0x4B604 -> load_screen_gfx)
        if depth == 0:
            tgt = None
            if w == 0x6100:
                tgt = a + 2 + s16(be16(rom, a + 2))
            elif w == 0x4EB9:
                tgt = be32(rom, a + 2)
            if tgt is not None and 0x200 <= tgt < len(rom) and tgt not in seen:
                calls.append(tgt)
        a += 2

    for tgt in calls[:24]:
        seen.add(tgt)
        sub = scrape(rom, tgt, seen, depth + 1)
        for k in out:
            out[k].extend(sub[k])
    return out


def _hex(v): return None if v is None else f"0x{v:06X}"
def _hex16(v): return None if v is None else f"0x{v:04X}"
def _plus1(v): return None if v is None else v + 1


AREA_TABLE = 0x2C846


def frame_list(rom, at, cap=64):
    """`{u32 src, u16 words}` repeated, terminated by 0xFFFF.

    These are animation frames, not a stream: `sub_472A` walks the list six
    bytes at a time and DMAs one entry into a fixed VRAM slot, wrapping back to
    the start when it hits the terminator. Every frame in a list is the same
    size, which is what makes them interchangeable in that slot.
    """
    out, a = [], at
    while len(out) < cap:
        if a + 6 > len(rom):
            break
        src, words = be32(rom, a), be16(rom, a + 4)
        if not (0x200 <= src < len(rom)) or not 0 < words <= 0x800:
            break
        out.append({"src": f"0x{src:06X}", "bytes": words * 2})
        a += 6
    return out if (a + 2 <= len(rom) and be16(rom, a) == 0xFFFF) else []


def areas(rom, count=40):
    """FF0EEA indexes 0x2C846. Each entry is a list of 10-byte tile-animation
    records, which `sub_46C8` unpacks into 0x12-byte RAM slots at FFDAB6."""
    out = []
    for i in range(count):
        rec_at = be32(rom, AREA_TABLE + 4 * i)
        if not 0x200 <= rec_at < len(rom):
            break
        records, a = [], rec_at
        while len(records) < 16 and a + 10 <= len(rom):
            if be16(rom, a) == 0xFFFF:
                break
            frames = frame_list(rom, be32(rom, a + 4))
            sizes = {f["bytes"] for f in frames}
            records.append({"at": f"0x{a:06X}",
                            "counter": be16(rom, a),        # initial, staggers the groups
                            "period": be16(rom, a + 2),     # frames between advances
                            "frames_at": f"0x{be32(rom, a + 4):06X}",
                            "vram": f"0x{be16(rom, a + 8):04X}",   # offset, + FFCE6C
                            "frames": frames,
                            "frame_bytes": sizes.pop() if len(sizes) == 1 else None})
            a += 10
        out.append({"index": i, "records_at": f"0x{rec_at:06X}", "records": records})
    return out


def resolve_lines(loads):
    """Best guess at the four live palette lines.

    A scene loads a line several times as it fades between states, so the last
    load wins — which matches what is on screen once the scene has settled. A
    `line: "all"` entry comes from the in-game descriptor, which writes all 64
    colours at once, so it fills whichever lines nothing else claimed.
    """
    lines = [None] * 4
    for p in loads:
        if p["line"] in (0, 1, 2, 3) and p["src"]:
            lines[p["line"]] = p["src"]
    for p in loads:
        if p["line"] == "all" and p["src"]:
            base = int(p["src"], 16)
            for n in range(4):
                if lines[n] is None:
                    lines[n] = f"0x{base + 0x20 * n:06X}"
    return lines


def dedupe(seq):
    out = []
    for x in seq:
        if x not in out:
            out.append(x)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("--json")
    ap.add_argument("--areas", action="store_true",
                    help="dump the per-area tile animations at 0x2C846")
    args = ap.parse_args()
    rom = Path(args.rom).read_bytes()

    if args.areas:
        found = areas(rom)
        groups = {r["at"]: r for a in found for r in a["records"]}
        odd = [r for r in groups.values() if r["frame_bytes"] is None]
        print(f"{len(found)} areas, {len(groups)} distinct animation groups, "
              f"{sum(len(r['frames']) for r in groups.values())} frames")
        if odd:
            print(f"  {len(odd)} group(s) with mixed frame sizes: "
                  + ", ".join(r["at"] for r in odd[:6]))
        for r in list(groups.values())[:12]:
            print(f"  {r['at']}  every {r['period']:3} frames, {len(r['frames']):3} frames"
                  f" of {r['frame_bytes']} bytes -> VRAM +{r['vram']}")
        if args.json:
            Path(args.json).with_name("areas.json").write_text(json.dumps(found, indent=1))
            print(f"-> {Path(args.json).with_name('areas.json')}")
        return

    scenes = []
    for i in range(SCENE_COUNT):
        entry = be32(rom, SCENE_TABLE + 4 * i)
        found = scrape(rom, entry, {entry})
        found = {k: dedupe(v) for k, v in found.items()}
        scenes.append({"index": i, "entry": f"0x{entry:06X}",
                       "palette_lines": resolve_lines(found["palette"]), **found})

        print(f"\n=== scene {i}: entry 0x{entry:06X} ===")
        if found["vdp_regs"]:
            print(f"  vdp regs   {', '.join(map(str, found['vdp_regs']))}")
        for u in found["upload"]:
            print(f"  upload     {u['src']} -> VRAM {u['vram']}, {u['tiles']} tiles")
        for t in found["tilemap"]:
            print(f"  tilemap    {t['src']} -> VDP {t['vdp']}, "
                  f"{t['cols']}x{t['rows']}, base {t['base']}")
        if found["unpack"]:
            print(f"  unpack     {', '.join(found['unpack'])}")
        for p in found["palette"]:
            print(f"  palette    line {p['line']} <- {p['src']}")
        if found["portrait"]:
            print(f"  portraits  {found['portrait']}")
        if found["desc_list"]:
            print(f"  desc lists {', '.join(found['desc_list'])}")

    if args.json:
        Path(args.json).write_text(json.dumps(scenes, indent=1))
        print(f"\n-> {args.json}")


if __name__ == "__main__":
    main()
