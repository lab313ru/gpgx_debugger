// listAroundZ80: the Z80 half of DisasmListing.h.

#include "DisasmListing.h"

#include "DisasmZ80.h"
#include "IDebugBackend.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace disasm {
namespace {

constexpr uint32_t kSpaceEnd    = 0x10000;    // the Z80 drives 16 address lines
constexpr uint32_t kRamEnd      = 0x4000;     // $0000-$3FFF: sound RAM, mirrored
constexpr uint32_t kRamSize     = 0x2000;
constexpr uint32_t kWindowStart = 0x8000;     // $8000-$FFFF: the bank window
constexpr uint32_t kWindowMask  = 0x7FFF;
constexpr uint32_t kBusMask     = 0xFFFFFF;   // the 68000 bus behind the window
constexpr uint32_t kCartEnd     = 0x400000;   // pages $00-$3F: the cartridge ROM
constexpr uint32_t kMaxInsn     = 4;          // longest Z80 instruction, in bytes
constexpr uint32_t kResyncBack  = 0x20;

// GpgxBackend's region ids (a published contract, see its region table).
constexpr int kRegionRom    = 0;
constexpr int kRegionZ80Ram = 2;

// The Z80 bytes [lo, hi), with a mark on each one that was read.
struct Window {
    uint32_t lo = 0;
    uint32_t hi = 0;
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> ok;

    bool readable(uint32_t a) const { return a >= lo && a < hi && ok[a - lo]; }

    // Readable bytes from a on, up to the first one that is not.
    size_t run(uint32_t a) const
    {
        size_t n = 0;
        while (a + n < hi && ok[a + n - lo]) ++n;
        return n;
    }

    void put(IDebugBackend& be, int region, uint32_t off, uint32_t a, uint32_t end)
    {
        const auto got = be.readRegion(region, off, end - a);
        const size_t n = std::min<size_t>(got.size(), end - a);
        std::copy_n(got.begin(), n, bytes.begin() + (a - lo));
        std::fill_n(ok.begin() + (a - lo), n, uint8_t(1));
    }
};

Window fetch(IDebugBackend& be, uint32_t lo, uint32_t hi, uint32_t bank)
{
    Window w;
    w.lo = lo;
    w.hi = hi;
    w.bytes.assign(hi - lo, 0);
    w.ok.assign(hi - lo, 0);

    uint32_t romEnd = 0;
    for (const MemRegion& r : be.getMemRegions())
        if (r.id == kRegionRom) romEnd = std::min(r.size, kCartEnd);

    // Sound RAM: one read per 8K, since a mirror boundary wraps the offset.
    for (uint32_t a = lo; a < std::min(hi, kRamEnd); ) {
        const uint32_t end = std::min((a | (kRamSize - 1)) + 1, hi);
        w.put(be, kRegionZ80Ram, a & (kRamSize - 1), a, end);
        a = end;
    }

    // The window is one 32K-aligned block of the 68000 bus, so the part of
    // it below the ROM's end is a single run.
    const uint32_t from = std::max(lo, kWindowStart);
    if (from < hi) {
        const uint32_t base = (bank & kBusMask) & ~kWindowMask;
        const uint32_t b = base | (from & kWindowMask);
        if (b < romEnd) {
            const uint32_t end = uint32_t(std::min<uint64_t>(hi, uint64_t(from) + (romEnd - b)));
            w.put(be, kRegionRom, b, from, end);
        }
    }
    return w;
}

DisasmLine decodeAt(const Window& w, uint32_t a)
{
    const size_t n = w.run(a);
    if (n == 0) return { a, 1, kNotReadable };
    return disasmZ80(&w.bytes[a - w.lo], n, uint16_t(a));
}

// The start in [w.lo, centre) whose stream of lines lands exactly on centre
// with the most lines, or centre itself when no start lands. decoded receives
// the line at every address tried, indexed by addr - w.lo.
uint32_t resync(const Window& w, uint32_t centre, std::vector<DisasmLine>& decoded)
{
    const uint32_t n = centre - w.lo;
    decoded.assign(n, {});

    // lines[i]: how many lines the stream from w.lo + i takes to reach
    // centre, or -1 when it steps over it. Filled from centre down, so the
    // line after each start has been counted already.
    std::vector<int> lines(n, -1);
    uint32_t best = centre;
    int most = 0;
    for (uint32_t i = n; i-- > 0; ) {
        const uint32_t a = w.lo + i;
        decoded[i] = decodeAt(w, a);
        const uint32_t next = a + decoded[i].len;
        int count = -1;
        if (next == centre)
            count = 1;
        else if (next < centre && lines[next - w.lo] >= 0)
            count = lines[next - w.lo] + 1;
        lines[i] = count;
        // Strictly more: of two streams as long, the one starting nearer wins.
        if (count > most) {
            most = count;
            best = a;
        }
    }
    return best;
}

std::string hexBytes(const Window& w, uint32_t addr, uint32_t len)
{
    std::string s;
    for (uint32_t i = 0; i < len; ++i) {
        char b[4];
        std::snprintf(b, sizeof b, "%02X", w.bytes[addr + i - w.lo]);
        if (!s.empty()) s += ' ';
        s += b;
    }
    return s;
}

} // namespace

// disasmZ80 spells every target as the last operand, "$" and four hex
// digits ("$38" for rst), after the condition and a comma when there is one.
std::optional<uint32_t> flowTargetZ80(const std::string& text)
{
    const size_t sp = text.find(' ');
    if (sp == std::string::npos) return std::nullopt;
    const std::string mn = text.substr(0, sp);
    if (mn != "jp" && mn != "jr" && mn != "djnz" && mn != "call" && mn != "rst")
        return std::nullopt;

    const size_t opStart = text.find_first_not_of(' ', sp);
    if (opStart == std::string::npos) return std::nullopt;
    const size_t comma = text.find(',', opStart);
    const size_t at = (comma == std::string::npos) ? opStart : comma + 1;
    if (text.compare(at, 1, "$") != 0) return std::nullopt;   // jp (hl), (ix), (iy)

    const char* begin = text.c_str() + at + 1;
    char* stop = nullptr;
    const unsigned long v = std::strtoul(begin, &stop, 16);
    if (stop == begin || !std::isxdigit((unsigned char)*begin) || *stop != '\0')
        return std::nullopt;
    return uint32_t(v) & (kSpaceEnd - 1);
}

std::vector<ListingLine> listAroundZ80(IDebugBackend& be, uint16_t centre, int before, int after)
{
    before = std::max(before, 0);
    after  = std::max(after, 0);

    const Z80Regs z = be.getZ80Regs();

    // Enough behind centre for `before` lines of the longest instruction, and
    // after it for the centre line, `after` lines and one more instruction.
    const uint64_t back = std::min<uint64_t>(centre, std::max<uint64_t>(kResyncBack, uint64_t(before) * kMaxInsn));
    const uint64_t ahead = (uint64_t(after) + 2) * kMaxInsn;
    const uint32_t lo = centre - uint32_t(back);
    const uint32_t hi = uint32_t(std::min<uint64_t>(kSpaceEnd, centre + ahead));
    const Window w = fetch(be, lo, hi, z.bank);

    std::vector<DisasmLine> decoded;
    std::vector<DisasmLine> lines;
    for (uint32_t a = resync(w, centre, decoded); a < centre; ) {
        lines.push_back(decoded[a - lo]);
        a += lines.back().len;
    }
    if (lines.size() > size_t(before))
        lines.erase(lines.begin(), lines.end() - before);

    const size_t head = lines.size();
    for (uint32_t a = centre; a < hi && lines.size() - head < size_t(after) + 1; ) {
        lines.push_back(decodeAt(w, a));
        if (lines.back().len == 0) break;
        a += lines.back().len;
    }

    const std::vector<Breakpoint> bps = be.getBreakpoints();
    const auto breaksAt = [&](uint32_t a) {
        for (const Breakpoint& bp : bps)
            if (bp.type == BpType::PC && bp.enabled && bp.cpu == Cpu::Z80 && !bp.is_vdp
                && bp.start <= a && a <= bp.end)
                return true;
        return false;
    };

    std::vector<ListingLine> out;
    out.reserve(lines.size());
    for (const DisasmLine& d : lines) {
        ListingLine l;
        l.addr = d.addr;
        l.len = d.len;
        l.readable = w.readable(d.addr);
        l.text = d.text;
        if (l.readable) {
            l.hex = hexBytes(w, d.addr, d.len);
            l.target = flowTargetZ80(d.text);
        }
        l.isPc = (d.addr == z.pc);
        l.hasBreakpoint = breaksAt(d.addr);
        out.push_back(std::move(l));
    }
    return out;
}

} // namespace disasm
