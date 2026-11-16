#include "DisasmListing.h"

#include "Disasm68k.h"
#include "IDebugBackend.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace disasm {
namespace {

constexpr uint32_t kBusEnd     = 0x1000000;   // the 68000 drives 24 address lines
constexpr uint32_t kCartEnd    = 0x400000;    // cartridge space; ROM past it is not on the bus
constexpr uint32_t kRamStart   = 0xE00000;    // $E00000-$FFFFFF: the 64K work RAM, mirrored
constexpr uint32_t kRamSize    = 0x10000;
constexpr uint32_t kMaxInsn    = 10;          // longest 68000 instruction, in bytes
constexpr uint32_t kResyncBack = 0x40;

// GpgxBackend's region ids (a published contract, see its region table).
constexpr int kRegionRom = 0;
constexpr int kRegionRam = 1;

// The bus bytes [lo, hi), with a mark on each one that was read.
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
};

Window fetch(IDebugBackend& be, uint32_t lo, uint32_t hi)
{
    Window w;
    w.lo = lo;
    w.hi = hi;
    w.bytes.assign(hi - lo, 0);
    w.ok.assign(hi - lo, 0);

    uint32_t romEnd = 0;
    for (const MemRegion& r : be.getMemRegions())
        if (r.id == kRegionRom) romEnd = std::min(r.size, kCartEnd);

    for (uint32_t a = lo; a < hi; ) {
        int region;
        uint32_t off, end;
        if (a < romEnd) {
            region = kRegionRom;
            off = a;
            end = romEnd;
        } else if (a >= kRamStart) {
            // One read per 64K: a mirror boundary wraps the offset back to 0.
            region = kRegionRam;
            off = a & (kRamSize - 1);
            end = (a | (kRamSize - 1)) + 1;
        } else {
            a = std::min(hi, kRamStart);
            continue;
        }
        end = std::min(end, hi);

        const auto got = be.readRegion(region, off, end - a);
        const size_t n = std::min<size_t>(got.size(), end - a);
        std::copy_n(got.begin(), n, w.bytes.begin() + (a - lo));
        std::fill_n(w.ok.begin() + (a - lo), n, uint8_t(1));
        a = end;
    }
    return w;
}

DisasmLine decodeAt(const Window& w, uint32_t a)
{
    const size_t n = w.run(a);
    if (n == 0) return { a, 2, kNotReadable };
    return disasm68k(&w.bytes[a - w.lo], n, a);
}

// The lines before centre: the stream of lines from start, up to stop. When
// a stream lands exactly on centre, stop is centre; otherwise the words
// [stop, centre) are left for one-word lines.
struct Lead {
    uint32_t start;
    uint32_t stop;
};

// The start in [w.lo, centre) whose stream of lines lands exactly on centre
// with the most lines. When no start lands (every stream steps over centre),
// the stream with the most lines before the one that steps over, stopping at
// that line, so the lines above centre never run out and scrolling up always
// moves. decoded receives the line at every even address tried, indexed by
// (addr - w.lo) / 2.
Lead resync(const Window& w, uint32_t centre, std::vector<DisasmLine>& decoded)
{
    const uint32_t n = (centre - w.lo) / 2;
    decoded.assign(n, {});

    // lines[i]: how many lines the stream from w.lo + 2i takes to reach
    // centre, or -1 when it steps over it. For one that steps over, short_[i]
    // counts its lines before the one that does, and stops[i] is where that
    // line starts. Filled from centre down, so the line after each start has
    // been counted already. A line that ends on an odd address (a dc.b where
    // a readable run ends) counts as stepping over.
    std::vector<int> lines(n, -1);
    std::vector<int> short_(n, 0);
    std::vector<uint32_t> stops(n, centre);
    Lead best{ centre, centre };
    Lead nearest{ centre, centre };
    int most = 0;
    int mostShort = -1;
    for (uint32_t i = n; i-- > 0; ) {
        const uint32_t a = w.lo + 2 * i;
        decoded[i] = decodeAt(w, a);
        const uint32_t next = a + decoded[i].len;
        const uint32_t j = (next - w.lo) / 2;
        int count = -1;
        if (next == centre) {
            count = 1;
        } else if (next < centre && (next - w.lo) % 2 == 0 && lines[j] >= 0) {
            count = lines[j] + 1;
        } else if (next < centre && (next - w.lo) % 2 == 0) {
            short_[i] = short_[j] + 1;
            stops[i] = stops[j];
        } else {
            stops[i] = a;
        }
        lines[i] = count;
        // Strictly more: of two streams as long, the one starting nearer wins.
        if (count > most) {
            most = count;
            best = { a, centre };
        }
        if (count < 0 && short_[i] > mostShort) {
            mostShort = short_[i];
            nearest = { a, stops[i] };
        }
    }
    return most > 0 ? best : nearest;
}

// A word left between a stream that does not land and centre.
DisasmLine wordLine(const Window& w, uint32_t a)
{
    if (!w.readable(a) || !w.readable(a + 1)) return { a, 2, kNotReadable };
    char t[16];
    std::snprintf(t, sizeof t, "dc.w $%04x", (w.bytes[a - w.lo] << 8) | w.bytes[a + 1 - w.lo]);
    return { a, 2, t };
}

std::string hexWords(const Window& w, uint32_t addr, uint32_t len)
{
    std::string s;
    for (uint32_t i = 0; i < len; i += 2) {
        char b[8];
        const uint8_t* p = &w.bytes[addr + i - w.lo];
        if (i + 1 < len) std::snprintf(b, sizeof b, "%02X%02X", p[0], p[1]);
        else             std::snprintf(b, sizeof b, "%02X", p[0]);
        if (!s.empty()) s += ' ';
        s += b;
    }
    return s;
}

bool isCondition(const std::string& cc)
{
    static const char* const kCc[] = { "t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
                                       "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le" };
    return std::find(std::begin(kCc), std::end(kCc), cc) != std::end(kCc);
}

// Musashi's spellings: bra, bsr, b<cc> and dbra, db<cc>, each with the
// target as the last operand, "$" and bare hex.
bool isRelativeBranch(const std::string& mn)
{
    if (mn == "bra" || mn == "bsr" || mn == "dbra") return true;
    if (mn.size() == 3 && mn[0] == 'b') return isCondition(mn.substr(1));
    if (mn.size() >= 3 && mn.compare(0, 2, "db") == 0) return isCondition(mn.substr(2));
    return false;
}

// Hex digits at s[pos] on; false when there are none.
bool parseHex(const std::string& s, size_t pos, uint32_t& v, size_t& end)
{
    if (pos >= s.size()) return false;
    const char* begin = s.c_str() + pos;
    char* stop = nullptr;
    const unsigned long long x = std::strtoull(begin, &stop, 16);
    if (stop == begin || !std::isxdigit((unsigned char)*begin)) return false;
    v = uint32_t(x);
    end = pos + size_t(stop - begin);
    return true;
}

} // namespace

std::optional<uint32_t> flowTarget(const std::string& text)
{
    const size_t sp = text.find(' ');
    if (sp == std::string::npos) return std::nullopt;
    const std::string mn = text.substr(0, sp);
    const size_t opStart = text.find_first_not_of(' ', sp);
    if (opStart == std::string::npos) return std::nullopt;

    uint32_t v = 0;
    size_t end = 0;
    if (isRelativeBranch(mn)) {
        const size_t dollar = text.rfind('$');
        if (dollar == std::string::npos || dollar < opStart) return std::nullopt;
        if (!parseHex(text, dollar + 1, v, end) || end != text.size()) return std::nullopt;
        return v & (kBusEnd - 1);
    }

    if (mn != "jmp" && mn != "jsr") return std::nullopt;

    // (d16,PC): Musashi appends the address it works out, "; ($1012)".
    const size_t helper = text.find("; ($", opStart);
    if (helper != std::string::npos) {
        if (text.compare(opStart, 1, "(") != 0 || text.find(",PC)", opStart) == std::string::npos)
            return std::nullopt;
        if (!parseHex(text, helper + 4, v, end) || text.compare(end, 1, ")") != 0)
            return std::nullopt;
        return v & (kBusEnd - 1);
    }

    // Absolute: "$f54.w" is sign-extended by the CPU, "$300.l" is taken whole.
    if (text[opStart] != '$' || !parseHex(text, opStart + 1, v, end)) return std::nullopt;
    if (text.compare(end, std::string::npos, ".w") == 0)
        return uint32_t(int32_t(int16_t(v & 0xFFFF))) & (kBusEnd - 1);
    if (text.compare(end, std::string::npos, ".l") == 0)
        return v & (kBusEnd - 1);
    return std::nullopt;
}

std::vector<ListingLine> listAround(IDebugBackend& be, uint32_t centre, int before, int after)
{
    centre &= (kBusEnd - 1) & ~1u;
    before = std::max(before, 0);
    after  = std::max(after, 0);

    // Enough behind centre for `before` lines of the longest instruction, and
    // after it for the centre line, `after` lines and one more instruction.
    const uint64_t back = std::min<uint64_t>(centre, std::max<uint64_t>(kResyncBack, uint64_t(before) * kMaxInsn));
    const uint64_t ahead = (uint64_t(after) + 2) * kMaxInsn;
    const uint32_t lo = centre - uint32_t(back);
    const uint32_t hi = uint32_t(std::min<uint64_t>(kBusEnd, centre + ahead));
    const Window w = fetch(be, lo, hi);

    std::vector<DisasmLine> decoded;
    std::vector<DisasmLine> lines;
    const Lead lead = resync(w, centre, decoded);
    for (uint32_t a = lead.start; a < lead.stop; ) {
        lines.push_back(decoded[(a - lo) / 2]);
        a += lines.back().len;
    }
    for (uint32_t a = lead.stop; a < centre; a += 2)
        lines.push_back(wordLine(w, a));
    if (lines.size() > size_t(before))
        lines.erase(lines.begin(), lines.end() - before);

    const size_t head = lines.size();
    for (uint32_t a = centre; a < hi && lines.size() - head < size_t(after) + 1; ) {
        lines.push_back(decodeAt(w, a));
        if (lines.back().len == 0) break;
        a += lines.back().len;
    }

    const uint32_t pc = be.getM68kRegs().pc & (kBusEnd - 1);
    const std::vector<Breakpoint> bps = be.getBreakpoints();
    // The backend matches an execution breakpoint against the opcode word, so
    // one on a + 1 stops there too.
    const auto breaksAt = [&](uint32_t a) {
        for (const Breakpoint& bp : bps)
            if (bp.type == BpType::PC && bp.enabled && bp.cpu == Cpu::M68K && !bp.is_vdp
                && bp.start <= a + 1 && a <= bp.end)
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
            l.hex = hexWords(w, d.addr, d.len);
            l.target = flowTarget(d.text);
        }
        l.isPc = (d.addr == pc);
        l.hasBreakpoint = breaksAt(d.addr);
        out.push_back(std::move(l));
    }
    return out;
}

} // namespace disasm
