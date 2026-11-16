#!/usr/bin/env python3
"""Checks disasm::disasmZ80 against two foreign Z80 disassemblers.

Runs on Linux (WSL on Windows: wsl -d Ubuntu -- python3 tools/z80_dasm_oracle.py)
and needs g++, binutils-z80 (z80-unknown-coff-objdump) and z80dasm:
    apt-get install -y g++ binutils-z80 z80dasm

Every opcode of every table goes in: 00-FF, CB xx, ED xx, DD xx, FD xx and
DD CB d xx / FD CB d xx, the indexed ones with d = $05, $00, $7f, $80, $ff,
plus jr/djnz with forward and backward offsets at both ends of the 64K space.
Each case gets a 16-byte slot in a binary file, zero-padded; every byte an
oracle could read past a case is a one-byte instruction or a nop, so after
any disagreement both oracles are back in step at the next slot.

The three listings are normalised to one spelling (case, hex notation,
spaces, the names the decoders give the same undocumented instruction) and
compared slot by slot, length first. A disagreement is either explained by a
rule in EXPLAINED, which says why the oracle is the one that is off, or
printed. The exit code is 1 when any length disagreement is unexplained.

Usage: z80_dasm_oracle.py [--show-explained] [--keep DIR]
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEBUGGER = os.path.join(ROOT, "smd_dgx_cmake", "debugger")
DUMP_SRC = os.path.join(ROOT, "tools", "z80_dasm_dump.cpp")
OBJDUMP = "z80-unknown-coff-objdump"
Z80DASM = "z80dasm"

SLOT = 16
CHUNK_SLOTS = 1024
MAIN_BASE = 0x4000
D_VALUES = (0x05, 0x00, 0x7F, 0x80, 0xFF)
REL_OPS = (0x10, 0x18, 0x20, 0x28, 0x30, 0x38)
REL_OFFSETS = (0x00, 0x05, 0x7F, 0x80, 0xFE, 0xFF)


# ---- cases -----------------------------------------------------------------

def main_cases():
    """Every opcode of every table, with fixed operand bytes after it."""
    out = []
    for op in range(256):
        out.append(bytes([op, 0x12, 0x34]))
    for op in range(256):
        out.append(bytes([0xCB, op]))
    for op in range(256):
        out.append(bytes([0xED, op, 0x34, 0x12]))
    for pre in (0xDD, 0xFD):
        for op in range(256):
            for d in D_VALUES:
                out.append(bytes([pre, op, d, 0x12]))
    for pre in (0xDD, 0xFD):
        for op in range(256):
            for d in D_VALUES:
                out.append(bytes([pre, 0xCB, d, op]))
    return out


def chunks():
    """(base address, [case bytes]) per binary file; each case at base + i*SLOT."""
    out = []
    cases = main_cases()
    for i in range(0, len(cases), CHUNK_SLOTS):
        out.append((MAIN_BASE, cases[i:i + CHUNK_SLOTS]))
    # Relative jumps where the target wraps: from the bottom of the address
    # space backwards, from the top forwards. The top chunk ends at $10000.
    for op in REL_OPS:
        rel = [bytes([op, d]) for d in REL_OFFSETS]
        out.append((0x0000, rel))
        out.append((0x10000 - SLOT * len(rel), rel))
    return out


def slot_bytes(case):
    return case + bytes(SLOT - len(case))


# ---- the three decoders ----------------------------------------------------

def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


def build_dump(work):
    exe = os.path.join(work, "z80_dasm_dump")
    run(["g++", "-std=c++20", "-O1", "-Wall", "-I", DEBUGGER, DUMP_SRC,
         os.path.join(DEBUGGER, "DisasmZ80.cpp"), "-o", exe])
    return exe


def ours(exe, base, cases):
    """{addr: (len, text)} for the first instruction of every slot."""
    lines = []
    for i, c in enumerate(cases):
        addr = base + i * SLOT
        lines.append("%04x %s" % (addr, slot_bytes(c)[:0x10000 - addr].hex()))
    out = run([exe], input="\n".join(lines) + "\n").splitlines()
    assert len(out) == len(cases)
    res = {}
    for i, l in enumerate(out):
        n, text = l.split("\t", 1)
        res[base + i * SLOT] = (int(n), text)
    return res


OBJ_LINE = re.compile(r"^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} )+)\s*(?:\t(.*))?$")


def objdump(path, base):
    out = run([OBJDUMP, "-D", "-z", "-b", "binary", "-m", "z80-full",
               "--adjust-vma=0x%x" % base, path])
    res = {}
    last = None
    for l in out.splitlines():
        m = OBJ_LINE.match(l)
        if not m:
            continue
        n = len(m.group(2).split())
        if m.group(3) is None and last is not None:   # bytes wrapped onto a second line
            res[last] = (res[last][0] + n, res[last][1])
            continue
        last = int(m.group(1), 16)
        res[last] = (n, m.group(3).strip())
    return res


Z80DASM_LINE = re.compile(r"\t+;([0-9a-f]{4})\t((?:[0-9a-f]{2} )+)")


def z80dasm(path, base):
    out = run([Z80DASM, "-a", "-t", "-u", "-g", "0x%x" % base, path])
    res = {}
    for l in out.splitlines():
        m = Z80DASM_LINE.search(l)
        if not m:
            continue
        text = l[:m.start()].split(";", 1)[0].strip()
        res[int(m.group(1), 16)] = (len(m.group(2).split()), text)
    return res


# ---- one spelling ----------------------------------------------------------

def _num(tok):
    return str(int(tok, 0))


def canon(text):
    """Lowercase, no spaces in operands, every number as plain decimal."""
    t = text.strip().lower()
    mn, _, ops = t.partition(" ")
    ops = ops.replace(" ", "").replace("\t", "")
    ops = re.sub(r"\$([0-9a-f]+)", lambda m: str(int(m.group(1), 16)), ops)
    ops = re.sub(r"0x([0-9a-f]+)", lambda m: str(int(m.group(1), 16)), ops)
    ops = re.sub(r"(?<![a-z0-9])([0-9][0-9a-f]*)h(?![a-z0-9])",
                 lambda m: str(int(m.group(1), 16)), ops)
    ops = re.sub(r"\((i[xy])\)", r"(\1+0)", ops)
    if mn == "defb":
        mn = "db"
    if mn == "sli":            # CB30-37: Young's "sll", also spelt "sli"/"sl1"
        mn = "sll"
    if mn == "in" and ops == "f,(c)":
        ops = "(c)"            # ED70 sets the flags only; objdump/z80dasm name it "in f,(c)"
    return (mn + " " + ops).strip()


def canon_z80dasm(text, addr):
    # Relative jumps are "$+N"/"$-N" with $ the address of the instruction.
    text = re.sub(r"\$([+-]\d+)", lambda m: "0x%x" % ((addr + int(m.group(1))) & 0xFFFF), text)
    # The undocumented register copy is spelt "rlc (ix+005h) & ld b,(ix+005h)".
    m = re.match(r"^(.*\S)\s*&\s*ld\s+([a-z]+),\((i[xy][+-][0-9a-f]+h)\)$", text)
    if m:
        text = "%s,%s" % (m.group(1), m.group(2))
    return canon(text)


def canon_objdump(text, addr):
    return canon(text)


# ---- why an oracle disagrees ----------------------------------------------
#
# Each rule: (name, oracle, predicate(case, ours, theirs) -> bool, reason).
# ours/theirs are (len, canonical text). Only disagreements reach them.

def _x(op): return op >> 6
def _y(op): return op >> 3 & 7
def _z(op): return op & 7


def _is_ddcb(c): return c[0] in (0xDD, 0xFD) and c[1] == 0xCB


EXPLAINED = []


def rule(name, oracle, reason):
    def deco(fn):
        EXPLAINED.append((name, oracle, fn, reason))
        return fn
    return deco


# Sources: the Zilog Z80 CPU User Manual (UM0080) for the documented set, and
# Sean Young, "The Undocumented Z80 Documented" (v0.91) for the rest. Both
# agree with how the core this debugger runs (MAME's Z80, in Genesis Plus GX)
# executes these opcodes.

def _db(text, first):
    """Whether the oracle left the bytes as data, starting with first."""
    return text == "db %d" % first or text.startswith("db %d," % first)


@rule("ed-mirror", "*",
      "ED 4C/54/5C/64/6C/74/7C run NEG, ED 55/5D/65/6D/75/7D run RETN, ED 4E/66/6E/76/7E "
      "run IM 0/0/0/1/2 (Young, ED table); the oracle leaves them as data")
def _ed_mirror(c, o, t):
    return (c[0] == 0xED and _x(c[1]) == 1 and _z(c[1]) in (4, 5, 6)
            and c[1] not in (0x44, 0x45, 0x4D, 0x46, 0x56, 0x5E)
            and o[0] == 2 and _db(t[1], 0xED))


@rule("ed-ld-hl-alias", "z80dasm",
      "ED 63 / ED 6B are LD (nn),HL / LD HL,(nn) again, 4 bytes (Young, ED table); "
      "z80dasm stops at the ED; objdump agrees with us")
def _ed_ld_hl(c, o, t):
    return c[0] == 0xED and c[1] in (0x63, 0x6B) and o[0] == 4 and t == (1, "db 237")


@rule("ed-nop-is-2-bytes", "z80dasm",
      "an undefined ED xx is one 2-byte NOP: xx is fetched as part of it and never runs on "
      "its own (Young; the core's illegal_2). z80dasm emits the ED alone and decodes xx "
      "as the next instruction; objdump agrees with us")
def _ed_nop(c, o, t):
    return c[0] == 0xED and o == (2, "db 237,%d" % c[1]) and t == (1, "db 237")


@rule("index-prefix-alone", "z80dasm",
      "DD/FD before an opcode that does not use HL/H/L/(HL), or before DD/FD/ED, only "
      "costs 4 T-states; the next opcode then runs unchanged (Young, DD/FD prefixes), so "
      "the prefix is a 1-byte line. z80dasm glues it to the next bytes as an 'illegal "
      "sequence' of fixed size; objdump agrees with us")
def _index_alone(c, o, t):
    return (c[0] in (0xDD, 0xFD) and o == (1, "db %d" % c[0])
            and t[1].startswith("db %d," % c[0]))


@rule("ddcb-bit-has-no-copy", "z80dasm",
      "BIT n,(IX+d) only sets flags: every DDCB/FDCB 40-7F is BIT n,(IX+d) and none writes "
      "a register (Young, DDCB table). z80dasm prints the register copy of RES/SET/shifts "
      "for BIT as well; objdump agrees with us")
def _ddcb_bit(c, o, t):
    return (_is_ddcb(c) and _x(c[3]) == 1 and o[0] == t[0] == 4
            and t[1] == "%s,%s" % (o[1], "bcdehl?a"[_z(c[3])]))


def explain(oracle, case, o, t):
    for name, who, fn, reason in EXPLAINED:
        if who in (oracle, "*") and fn(case, o, t):
            return name, reason
    return None


# ---- main ------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--show-explained", action="store_true")
    ap.add_argument("--keep", help="write the binaries and listings here")
    args = ap.parse_args()

    for tool in ("g++", OBJDUMP, Z80DASM):
        if not shutil.which(tool):
            sys.exit("missing %s: apt-get install -y g++ binutils-z80 z80dasm" % tool)

    work = args.keep or tempfile.mkdtemp(prefix="z80_oracle_")
    os.makedirs(work, exist_ok=True)
    exe = build_dump(work)

    total = 0
    agree = {"objdump": 0, "z80dasm": 0}
    explained = {}
    unexplained = []
    missing = []
    for ci, (base, cases) in enumerate(chunks()):
        path = os.path.join(work, "chunk%02d_%04x.bin" % (ci, base))
        blob = b"".join(slot_bytes(c) for c in cases)
        with open(path, "wb") as f:
            f.write(blob)
        mine = ours(exe, base, cases)
        theirs = {"objdump": (objdump(path, base), canon_objdump),
                  "z80dasm": (z80dasm(path, base), canon_z80dasm)}
        for i, case in enumerate(cases):
            addr = base + i * SLOT
            total += 1
            olen, otext = mine[addr]
            o = (olen, canon(otext))
            for name, (listing, norm) in theirs.items():
                if addr not in listing:
                    missing.append((name, addr, case))
                    continue
                tlen, ttext = listing[addr]
                t = (tlen, norm(ttext, addr))
                if o == t:
                    agree[name] += 1
                    continue
                why = explain(name, case, o, t)
                rec = (name, case, addr, (olen, otext), (tlen, ttext), o, t)
                if why:
                    explained.setdefault(why, []).append(rec)
                else:
                    unexplained.append(rec)

    print("%d cases" % total)
    for name, n in agree.items():
        print("  %-8s agrees on %d" % (name, n))
    for (rname, reason), recs in sorted(explained.items()):
        print("  explained, %-28s %4d  %s" % (rname, len(recs), reason))
        if args.show_explained:
            for r in recs[:6]:
                print("        %-7s %-10s ours %d %-26r theirs %d %r"
                      % (r[0], r[1].hex(), r[3][0], r[3][1], r[4][0], r[4][1]))
    if missing:
        print("\n%d slot(s) an oracle has no line for (out of step):" % len(missing))
        for name, addr, case in missing[:20]:
            print("  %-8s %04x %s" % (name, addr, case.hex()))

    lens = [r for r in unexplained if r[5][0] != r[6][0]]
    texts = [r for r in unexplained if r[5][0] == r[6][0]]
    for title, recs in (("LENGTH", lens), ("TEXT", texts)):
        print("\nunexplained %s disagreements: %d" % (title, len(recs)))
        for r in recs:
            print("  %-8s %04x %-10s ours %d %-28r theirs %d %r"
                  % (r[0], r[2], r[1].hex(), r[3][0], r[3][1], r[4][0], r[4][1]))
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    return 1 if lens or missing else 0


if __name__ == "__main__":
    sys.exit(main())
