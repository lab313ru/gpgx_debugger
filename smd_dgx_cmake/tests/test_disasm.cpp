// disasm68k: Musashi's 68000 disassembler over a caller's buffer.
//
// The texts below are what Musashi printed for these bytes, each checked by
// hand against the opcode map. They pin three things at once: that the right
// Musashi is linked (the operand spelling is its own), that the wrapper feeds
// it the bytes in the right order and at the right address, and that it never
// decodes past what the caller handed over.

#include "test_harness.h"

#include "debugger/Disasm68k.h"

#include <atomic>
#include <thread>
#include <vector>

using disasm::disasm68k;

namespace {

struct Golden {
    std::vector<uint8_t> bytes;
    size_t               avail;   // what the call may read; anything after is bait
    uint32_t             addr;
    uint32_t             len;
    const char*          text;
};

const std::vector<Golden>& goldens()
{
    static const std::vector<Golden> g = {
        // MULU.W #imm,D0: the immediate is the extension word, $0240.
        { { 0xC0, 0xFC, 0x02, 0x40 },             4, 0x001000, 4, "mulu.w  #$240, D0" },
        { { 0x4E, 0xB8, 0x0F, 0x54 },             4, 0x001000, 4, "jsr     $f54.w" },
        { { 0x4E, 0xB9, 0x00, 0x00, 0x03, 0x00 }, 6, 0x001000, 6, "jsr     $300.l" },
        { { 0x4E, 0x75 },                         2, 0x001000, 2, "rts" },
        { { 0x4E, 0x73 },                         2, 0x001000, 2, "rte" },
        { { 0x4E, 0x71 },                         2, 0x001000, 2, "nop" },
        { { 0x20, 0x3C, 0x12, 0x34, 0x56, 0x78 }, 6, 0x001000, 6, "move.l  #$12345678, D0" },
        { { 0x33, 0xFC, 0x12, 0x34, 0x00, 0xC0, 0x00, 0x04 }, 8, 0x001000, 8, "move.w  #$1234, $c00004.l" },
        { { 0x48, 0xE7, 0xFF, 0xFE },             4, 0x001000, 4, "movem.l D0-D7/A0-A6, -(A7)" },
        { { 0x4C, 0xDF, 0x7F, 0xFF },             4, 0x001000, 4, "movem.l (A7)+, D0-D7/A0-A6" },

        // Targets are relative to the address passed in, not to the buffer.
        { { 0x60, 0xFE },                         2, 0x001000, 2, "bra     $1000" },     // to itself
        { { 0x60, 0xFE },                         2, 0xFF0100, 2, "bra     $ff0100" },
        { { 0x60, 0x00, 0x00, 0x00 },             4, 0x001000, 4, "bra     $1002" },
        { { 0x61, 0x00, 0x00, 0x10 },             4, 0x001000, 4, "bsr     $1012" },     // bsr.w, +$10 past the extension
        { { 0x61, 0x00, 0xFF, 0xFE },             4, 0x001000, 4, "bsr     $1000" },
        { { 0x41, 0xFA, 0x00, 0x10 },             4, 0x001000, 4, "lea     ($10,PC), A0; ($1012)" },

        // Not instructions on a 68000, in Musashi's own spelling.
        { { 0x4A, 0xFC },                         2, 0x001000, 2, "dc.w $4afc; ILLEGAL" },
        { { 0x4E, 0x7A, 0x00, 0x01 },             4, 0x001000, 2, "dc.w $4e7a; ILLEGAL" }, // movec, 68010+

        // The instruction is longer than the buffer: the bytes after avail are
        // there, and must not be used.
        { { 0x4E, 0xB9, 0x00, 0x00, 0x03, 0x00 }, 4, 0x001000, 2, "dc.w $4eb9" },
        { { 0xC0, 0xFC, 0x02, 0x40 },             3, 0x001000, 2, "dc.w $c0fc" },
        { { 0xC0, 0xFC, 0x02, 0x40 },             2, 0x001000, 2, "dc.w $c0fc" },
        // Only the instruction is read, not the whole buffer.
        { { 0x4E, 0x75, 0x4E, 0x71 },             4, 0x001000, 2, "rts" },

        // Line A and line F trap on a 68000. Upstream decodes F000-F0FF as
        // PMMU instructions in any mode ("pmove  D0, tc" for these bytes).
        { { 0xA0, 0x00 },                         2, 0x001000, 2, "dc.w $a000" },
        { { 0xA9, 0x34 },                         2, 0x001000, 2, "dc.w $a934" },
        { { 0xF0, 0x00, 0x40, 0x00 },             4, 0x001000, 2, "dc.w $f000" },
        { { 0xF0, 0x10, 0x40, 0x00 },             4, 0x001000, 2, "dc.w $f010" },
        { { 0xF2, 0x00, 0x00, 0x00 },             4, 0x001000, 2, "dc.w $f200" },
        { { 0xFF, 0xFF },                         2, 0x001000, 2, "dc.w $ffff" },

        // No opcode word at all.
        { { 0x4E },                               1, 0x001000, 1, "dc.b $4e" },
        { { 0x4E, 0x75 },                         0, 0x001000, 0, "" },
    };
    return g;
}

} // namespace

TEST(disasm_golden_table)
{
    for (const auto& g : goldens()) {
        const auto line = disasm68k(g.bytes.data(), g.avail, g.addr);
        const std::string where = t::hexBytes(g.bytes) + " avail " + std::to_string(g.avail);
        if (line.text != g.text || line.len != g.len || line.addr != g.addr)
            t::fail(__FILE__, __LINE__,
                    where + ": got \"" + line.text + "\" len " + std::to_string(line.len)
                        + " at " + t::hex(line.addr) + ", want \"" + g.text + "\" len "
                        + std::to_string(g.len) + " at " + t::hex(g.addr));
    }
}

// How the debugger window will use it: walk a block of memory by len.
TEST(disasm_walks_a_block)
{
    const uint8_t code[] = {
        0x4E, 0x71,                               // 000300 nop
        0x4E, 0xB9, 0x00, 0x00, 0x03, 0x00,       // 000302 jsr $300.l
        0x60, 0xFE,                               // 000308 bra $308
        0xF0, 0x00,                               // 00030a dc.w $f000
        0x4E, 0x75,                               // 00030c rts
        0x4E, 0xB8,                               // 00030e jsr abs.w, cut off
    };
    const struct { uint32_t addr; uint32_t len; const char* text; } want[] = {
        { 0x300, 2, "nop" },
        { 0x302, 6, "jsr     $300.l" },
        { 0x308, 2, "bra     $308" },
        { 0x30A, 2, "dc.w $f000" },
        { 0x30C, 2, "rts" },
        { 0x30E, 2, "dc.w $4eb8" },
    };

    size_t off = 0;
    for (const auto& w : want) {
        REQUIRE(off < sizeof code);
        const auto line = disasm68k(code + off, sizeof code - off, 0x300 + (uint32_t)off);
        CHECK_EQ(line.addr, w.addr);
        CHECK_EQ(line.len, w.len);
        CHECK_STR(line.text, w.text);
        if (line.len == 0) return;
        off += line.len;
    }
    CHECK_EQ(off, sizeof code);
}

// Musashi formats into statics and reads through globals; two windows
// disassembling at once must still each get their own bytes back.
TEST(disasm_concurrent_calls_do_not_mix)
{
    const uint8_t a[] = { 0x4E, 0xB9, 0x00, 0x00, 0x03, 0x00 };
    const uint8_t b[] = { 0xC0, 0xFC, 0x02, 0x40 };
    std::atomic<int> bad{ 0 };

    auto worker = [&](const uint8_t* bytes, size_t n, uint32_t addr, uint32_t len, const char* text) {
        for (int i = 0; i < 20000; ++i) {
            const auto line = disasm68k(bytes, n, addr);
            if (line.len != len || line.text != text) ++bad;
        }
    };
    std::thread t1(worker, a, sizeof a, 0x001000u, 6u, "jsr     $300.l");
    std::thread t2(worker, b, sizeof b, 0xFF8000u, 4u, "mulu.w  #$240, D0");
    t1.join();
    t2.join();
    CHECK_EQ(bad.load(), 0);
}
