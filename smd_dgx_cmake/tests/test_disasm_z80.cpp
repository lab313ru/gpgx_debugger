// disasmZ80: our own Z80 decoder over a caller's buffer.
//
// The texts below are what disasmZ80 printed for these bytes after
// tools/z80_dasm_oracle.py had compared every opcode of every table with
// objdump (binutils-z80) and z80dasm and every disagreement was accounted for.
// They pin the spelling DisasmZ80.h promises, the undocumented forms, the
// relative targets and the rule that nothing past avail is read.

#include "test_harness.h"

#include "debugger/DisasmZ80.h"

#include <cstdio>
#include <vector>

using disasm::disasmZ80;

namespace {

struct Golden {
    std::vector<uint8_t> bytes;
    size_t               avail;   // what the call may read; anything after is bait
    uint16_t             addr;
    uint32_t             len;
    const char*          text;
};

const std::vector<Golden>& goldens()
{
    static const std::vector<Golden> g = {
        { { 0x00 },                            1, 0x4000, 1, "nop" },
        { { 0x01, 0x34, 0x12 },                3, 0x4000, 3, "ld      bc,$1234" },
        { { 0x02 },                            1, 0x4000, 1, "ld      (bc),a" },
        { { 0x0A },                            1, 0x4000, 1, "ld      a,(bc)" },
        { { 0x08 },                            1, 0x4000, 1, "ex      af,af'" },
        { { 0x10, 0xFE },                      2, 0x0100, 2, "djnz    $0100" },  // djnz to itself
        { { 0x18, 0x00 },                      2, 0x4000, 2, "jr      $4002" },
        { { 0x20, 0x21 },                      2, 0x0100, 2, "jr      nz,$0123" },
        { { 0x38, 0x80 },                      2, 0x0000, 2, "jr      c,$ff82" },  // backward past $0000
        { { 0x18, 0x7F },                      2, 0xFFF0, 2, "jr      $0071" },  // forward past $ffff
        { { 0x30, 0xFF },                      2, 0x8000, 2, "jr      nc,$8001" },
        { { 0x22, 0x34, 0x12 },                3, 0x4000, 3, "ld      ($1234),hl" },
        { { 0x2A, 0x34, 0x12 },                3, 0x4000, 3, "ld      hl,($1234)" },
        { { 0x32, 0x34, 0x12 },                3, 0x4000, 3, "ld      ($1234),a" },
        { { 0x3A, 0x34, 0x12 },                3, 0x4000, 3, "ld      a,($1234)" },
        { { 0x36, 0x12 },                      2, 0x4000, 2, "ld      (hl),$12" },
        { { 0x3E, 0x12 },                      2, 0x4000, 2, "ld      a,$12" },
        { { 0x2B },                            1, 0x4000, 1, "dec     hl" },
        { { 0x3C },                            1, 0x4000, 1, "inc     a" },
        { { 0x27 },                            1, 0x4000, 1, "daa" },
        { { 0x3F },                            1, 0x4000, 1, "ccf" },
        { { 0x76 },                            1, 0x4000, 1, "halt" },
        { { 0x41 },                            1, 0x4000, 1, "ld      b,c" },
        { { 0x7E },                            1, 0x4000, 1, "ld      a,(hl)" },
        { { 0x70 },                            1, 0x4000, 1, "ld      (hl),b" },
        { { 0x86 },                            1, 0x4000, 1, "add     a,(hl)" },
        { { 0x96 },                            1, 0x4000, 1, "sub     (hl)" },
        { { 0x9F },                            1, 0x4000, 1, "sbc     a,a" },
        { { 0xA8 },                            1, 0x4000, 1, "xor     b" },
        { { 0xFE, 0x12 },                      2, 0x4000, 2, "cp      $12" },
        { { 0xCE, 0x80 },                      2, 0x4000, 2, "adc     a,$80" },
        { { 0xC0 },                            1, 0x4000, 1, "ret     nz" },
        { { 0xC9 },                            1, 0x4000, 1, "ret" },
        { { 0xC3, 0x34, 0x12 },                3, 0x4000, 3, "jp      $1234" },
        { { 0xCA, 0x34, 0x12 },                3, 0x4000, 3, "jp      z,$1234" },
        { { 0xCD, 0x34, 0x12 },                3, 0x4000, 3, "call    $1234" },
        { { 0xFC, 0x34, 0x12 },                3, 0x4000, 3, "call    m,$1234" },
        { { 0xF5 },                            1, 0x4000, 1, "push    af" },
        { { 0xE1 },                            1, 0x4000, 1, "pop     hl" },
        { { 0xE3 },                            1, 0x4000, 1, "ex      (sp),hl" },
        { { 0xE9 },                            1, 0x4000, 1, "jp      (hl)" },
        { { 0xEB },                            1, 0x4000, 1, "ex      de,hl" },
        { { 0xD9 },                            1, 0x4000, 1, "exx" },
        { { 0xF9 },                            1, 0x4000, 1, "ld      sp,hl" },
        { { 0xD3, 0x12 },                      2, 0x4000, 2, "out     ($12),a" },
        { { 0xDB, 0x12 },                      2, 0x4000, 2, "in      a,($12)" },
        { { 0xFF },                            1, 0x4000, 1, "rst     $38" },
        { { 0xC7 },                            1, 0x4000, 1, "rst     $00" },
        { { 0xF3 },                            1, 0x4000, 1, "di" },
        { { 0xCB, 0x00 },                      2, 0x4000, 2, "rlc     b" },
        { { 0xCB, 0x1E },                      2, 0x4000, 2, "rr      (hl)" },
        { { 0xCB, 0x36 },                      2, 0x4000, 2, "sll     (hl)" },  // undocumented sll
        { { 0xCB, 0x37 },                      2, 0x4000, 2, "sll     a" },
        { { 0xCB, 0x3F },                      2, 0x4000, 2, "srl     a" },
        { { 0xCB, 0x7E },                      2, 0x4000, 2, "bit     7,(hl)" },
        { { 0xCB, 0x87 },                      2, 0x4000, 2, "res     0,a" },
        { { 0xCB, 0xFF },                      2, 0x4000, 2, "set     7,a" },
        { { 0xED, 0x40 },                      2, 0x4000, 2, "in      b,(c)" },
        { { 0xED, 0x78 },                      2, 0x4000, 2, "in      a,(c)" },
        { { 0xED, 0x70 },                      2, 0x4000, 2, "in      (c)" },  // flags only
        { { 0xED, 0x71 },                      2, 0x4000, 2, "out     (c),0" },
        { { 0xED, 0x79 },                      2, 0x4000, 2, "out     (c),a" },
        { { 0xED, 0x42 },                      2, 0x4000, 2, "sbc     hl,bc" },
        { { 0xED, 0x7A },                      2, 0x4000, 2, "adc     hl,sp" },
        { { 0xED, 0x43, 0x34, 0x12 },          4, 0x4000, 4, "ld      ($1234),bc" },
        { { 0xED, 0x7B, 0x34, 0x12 },          4, 0x4000, 4, "ld      sp,($1234)" },
        { { 0xED, 0x63, 0x34, 0x12 },          4, 0x4000, 4, "ld      ($1234),hl" },  // same as 22 nn
        { { 0xED, 0x44 },                      2, 0x4000, 2, "neg" },
        { { 0xED, 0x7C },                      2, 0x4000, 2, "neg" },  // mirror
        { { 0xED, 0x45 },                      2, 0x4000, 2, "retn" },
        { { 0xED, 0x4D },                      2, 0x4000, 2, "reti" },
        { { 0xED, 0x55 },                      2, 0x4000, 2, "retn" },  // mirror
        { { 0xED, 0x46 },                      2, 0x4000, 2, "im      0" },
        { { 0xED, 0x56 },                      2, 0x4000, 2, "im      1" },
        { { 0xED, 0x5E },                      2, 0x4000, 2, "im      2" },
        { { 0xED, 0x4E },                      2, 0x4000, 2, "im      0" },  // mirror, mode 0
        { { 0xED, 0x76 },                      2, 0x4000, 2, "im      1" },  // mirror
        { { 0xED, 0x47 },                      2, 0x4000, 2, "ld      i,a" },
        { { 0xED, 0x5F },                      2, 0x4000, 2, "ld      a,r" },
        { { 0xED, 0x67 },                      2, 0x4000, 2, "rrd" },
        { { 0xED, 0x6F },                      2, 0x4000, 2, "rld" },
        { { 0xED, 0xB0 },                      2, 0x4000, 2, "ldir" },
        { { 0xED, 0xA3 },                      2, 0x4000, 2, "outi" },
        { { 0xED, 0xBB },                      2, 0x4000, 2, "otdr" },
        { { 0xED, 0xA9 },                      2, 0x4000, 2, "cpd" },
        { { 0xED, 0x00 },                      2, 0x4000, 2, "db      $ed,$00" },  // no operation
        { { 0xED, 0x77 },                      2, 0x4000, 2, "db      $ed,$77" },  // no operation
        { { 0xED, 0xFF },                      2, 0x4000, 2, "db      $ed,$ff" },  // no operation
        { { 0xED, 0xB4 },                      2, 0x4000, 2, "db      $ed,$b4" },  // no operation
        { { 0xDD, 0x21, 0x34, 0x12 },          4, 0x4000, 4, "ld      ix,$1234" },
        { { 0xFD, 0x09 },                      2, 0x4000, 2, "add     iy,bc" },
        { { 0xDD, 0x29 },                      2, 0x4000, 2, "add     ix,ix" },
        { { 0xDD, 0x23 },                      2, 0x4000, 2, "inc     ix" },
        { { 0xDD, 0x34, 0x05 },                3, 0x4000, 3, "inc     (ix+$05)" },
        { { 0xDD, 0x36, 0x80, 0x12 },          4, 0x4000, 4, "ld      (ix-$80),$12" },  // displacement, then the immediate
        { { 0xFD, 0x46, 0x00 },                3, 0x4000, 3, "ld      b,(iy+$00)" },
        { { 0xDD, 0x66, 0xFF },                3, 0x4000, 3, "ld      h,(ix-$01)" },  // h stays h next to (ix+d)
        { { 0xDD, 0x75, 0x7F },                3, 0x4000, 3, "ld      (ix+$7f),l" },
        { { 0xFD, 0xBE, 0x80 },                3, 0x4000, 3, "cp      (iy-$80)" },
        { { 0xDD, 0x26, 0x12 },                3, 0x4000, 3, "ld      ixh,$12" },
        { { 0xFD, 0x85 },                      2, 0x4000, 2, "add     a,iyl" },
        { { 0xDD, 0x6C },                      2, 0x4000, 2, "ld      ixl,ixh" },
        { { 0xFD, 0x7C },                      2, 0x4000, 2, "ld      a,iyh" },
        { { 0xFD, 0xE5 },                      2, 0x4000, 2, "push    iy" },
        { { 0xDD, 0xE1 },                      2, 0x4000, 2, "pop     ix" },
        { { 0xDD, 0xE3 },                      2, 0x4000, 2, "ex      (sp),ix" },
        { { 0xFD, 0xE9 },                      2, 0x4000, 2, "jp      (iy)" },
        { { 0xDD, 0xF9 },                      2, 0x4000, 2, "ld      sp,ix" },
        { { 0xDD, 0x22, 0x34, 0x12 },          4, 0x4000, 4, "ld      ($1234),ix" },
        { { 0xFD, 0x2A, 0x34, 0x12 },          4, 0x4000, 4, "ld      iy,($1234)" },
        { { 0xDD, 0x00 },                      2, 0x4000, 1, "db      $dd" },
        { { 0xDD, 0xEB },                      2, 0x4000, 1, "db      $dd" },  // ex de,hl is not renamed
        { { 0xDD, 0x76 },                      2, 0x4000, 1, "db      $dd" },
        { { 0xDD, 0x18, 0xFE },                3, 0x4000, 1, "db      $dd" },
        { { 0xDD, 0xDD, 0x21, 0x34, 0x12 },    5, 0x4000, 1, "db      $dd" },
        { { 0xFD, 0xED, 0x44 },                3, 0x4000, 1, "db      $fd" },
        { { 0xDD, 0xFD, 0x2A },                3, 0x4000, 1, "db      $dd" },
        { { 0xDD, 0xCB, 0x05, 0x06 },          4, 0x4000, 4, "rlc     (ix+$05)" },
        { { 0xDD, 0xCB, 0x05, 0x00 },          4, 0x4000, 4, "rlc     (ix+$05),b" },  // copy into b
        { { 0xFD, 0xCB, 0x80, 0x3E },          4, 0x4000, 4, "srl     (iy-$80)" },
        { { 0xDD, 0xCB, 0x05, 0x36 },          4, 0x4000, 4, "sll     (ix+$05)" },
        { { 0xDD, 0xCB, 0x05, 0x80 },          4, 0x4000, 4, "res     0,(ix+$05),b" },
        { { 0xDD, 0xCB, 0x05, 0x46 },          4, 0x4000, 4, "bit     0,(ix+$05)" },
        { { 0xDD, 0xCB, 0x05, 0x40 },          4, 0x4000, 4, "bit     0,(ix+$05)" },  // bit: no copy
        { { 0xFD, 0xCB, 0xFF, 0xFE },          4, 0x4000, 4, "set     7,(iy-$01)" },
        { { 0xFD, 0xCB, 0xFF, 0xFF },          4, 0x4000, 4, "set     7,(iy-$01),a" },
        { { 0xDD, 0xCB, 0x00, 0x16 },          4, 0x4000, 4, "rl      (ix+$00)" },
        { { 0x01, 0x34, 0x12 },                2, 0x4000, 1, "db      $01" },
        { { 0xC3, 0x34, 0x12 },                1, 0x4000, 1, "db      $c3" },
        { { 0xED, 0x43, 0x34, 0x12 },          3, 0x4000, 1, "db      $ed" },
        { { 0xED, 0x44 },                      1, 0x4000, 1, "db      $ed" },
        { { 0xDD, 0xCB, 0x05, 0x06 },          3, 0x4000, 1, "db      $dd" },
        { { 0xDD, 0x21, 0x34, 0x12 },          3, 0x4000, 1, "db      $dd" },
        { { 0xDD, 0x21 },                      1, 0x4000, 1, "db      $dd" },
        { { 0xCB, 0x00 },                      1, 0x4000, 1, "db      $cb" },
        { { 0xDD, 0x34, 0x05 },                2, 0x4000, 1, "db      $dd" },
        { { 0x10, 0xFE },                      1, 0x0100, 1, "db      $10" },
        { { 0x00, 0xC3, 0x34, 0x12 },          4, 0x4000, 1, "nop" },  // only the instruction is read
        { { 0x00 },                            0, 0x4000, 0, "" },
    };
    return g;
}

} // namespace

TEST(disasm_z80_golden_table)
{
    for (const auto& g : goldens()) {
        const auto line = disasmZ80(g.bytes.data(), g.avail, g.addr);
        const std::string where = t::hexBytes(g.bytes) + " avail " + std::to_string(g.avail)
                                  + " at " + t::hex(g.addr);
        if (line.text != g.text || line.len != g.len || line.addr != g.addr)
            t::fail(__FILE__, __LINE__,
                    where + ": got \"" + line.text + "\" len " + std::to_string(line.len)
                        + " at " + t::hex(line.addr) + ", want \"" + g.text + "\" len "
                        + std::to_string(g.len));
    }
}

// Every opcode of every table, each with operand bytes after it: the length
// is 1 to 4, a buffer of exactly that length decodes the same, and one byte
// less gives "db" of the first byte, never a line made up past the buffer.
TEST(disasm_z80_every_opcode_respects_avail)
{
    std::vector<std::vector<uint8_t>> seqs;
    for (int op = 0; op < 256; ++op) {
        seqs.push_back({ (uint8_t)op, 0x12, 0x34, 0x56 });
        seqs.push_back({ 0xCB, (uint8_t)op, 0x12, 0x34 });
        seqs.push_back({ 0xED, (uint8_t)op, 0x34, 0x12 });
        for (uint8_t pre : { 0xDD, 0xFD }) {
            seqs.push_back({ pre, (uint8_t)op, 0x80, 0x12 });
            seqs.push_back({ pre, 0xCB, 0x80, (uint8_t)op });
        }
    }

    int checked = 0;
    for (const auto& s : seqs) {
        const auto full = disasmZ80(s.data(), s.size(), 0x4000);
        const std::string where = t::hexBytes(s);
        if (full.len < 1 || full.len > 4 || full.text.empty()) {
            t::fail(__FILE__, __LINE__, where + ": len " + std::to_string(full.len)
                                            + " text \"" + full.text + "\"");
            continue;
        }
        const auto exact = disasmZ80(s.data(), full.len, 0x4000);
        if (exact.len != full.len || exact.text != full.text)
            t::fail(__FILE__, __LINE__, where + ": \"" + exact.text + "\" with avail "
                                            + std::to_string(full.len) + ", \"" + full.text
                                            + "\" with more");
        if (full.len > 1) {
            char want[16];
            std::snprintf(want, sizeof want, "db      $%02x", s[0]);
            const auto cut = disasmZ80(s.data(), full.len - 1, 0x4000);
            if (cut.len != 1 || cut.text != want)
                t::fail(__FILE__, __LINE__, where + ": got \"" + cut.text + "\" len "
                                                + std::to_string(cut.len) + " with avail "
                                                + std::to_string(full.len - 1));
        }
        ++checked;
    }
    CHECK_EQ(checked, (int)seqs.size());
}

// How a disassembly window walks Z80 memory: by len. A lone DD is a line of
// its own and the instruction after it decodes as if it were not there.
TEST(disasm_z80_walks_a_block)
{
    const uint8_t code[] = {
        0xF3,                         // 0000 di
        0x31, 0x00, 0x20,             // 0001 ld sp,$2000
        0xDD, 0xEB,                   // 0004 db $dd, then ex de,hl
        0xDD, 0x36, 0xFE, 0x99,       // 0006 ld (ix-$02),$99
        0xFD, 0xCB, 0x01, 0xC6,       // 000a set 0,(iy+$01)
        0xED, 0xB0,                   // 000e ldir
        0x10, 0xEE,                   // 0010 djnz $0000
        0xC3,                         // 0012 jp, cut off
    };
    const struct { uint16_t addr; uint32_t len; const char* text; } want[] = {
        { 0x0000, 1, "di" },
        { 0x0001, 3, "ld      sp,$2000" },
        { 0x0004, 1, "db      $dd" },
        { 0x0005, 1, "ex      de,hl" },
        { 0x0006, 4, "ld      (ix-$02),$99" },
        { 0x000A, 4, "set     0,(iy+$01)" },
        { 0x000E, 2, "ldir" },
        { 0x0010, 2, "djnz    $0000" },
        { 0x0012, 1, "db      $c3" },
    };

    size_t off = 0;
    for (const auto& w : want) {
        REQUIRE(off < sizeof code);
        const auto line = disasmZ80(code + off, sizeof code - off, (uint16_t)off);
        CHECK_EQ(line.addr, w.addr);
        CHECK_EQ(line.len, w.len);
        CHECK_STR(line.text, w.text);
        if (line.len == 0) return;
        off += line.len;
    }
    CHECK_EQ(off, sizeof code);
}

TEST(disasm_z80_null_or_empty_buffer)
{
    const auto a = disasmZ80(nullptr, 4, 0x1234);
    CHECK_EQ(a.len, 0u);
    CHECK_STR(a.text, "");
    CHECK_EQ(a.addr, 0x1234u);
}
