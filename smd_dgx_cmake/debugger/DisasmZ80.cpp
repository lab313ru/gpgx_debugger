#include "DisasmZ80.h"

#include <cstdio>
#include <initializer_list>
#include <string>

// Decoded with the field method of "Decoding Z80 Opcodes" (Cristian Dinu): an
// opcode byte is split into x = bits 7-6, y = bits 5-3, z = bits 2-0, and y
// again into p = bits 5-4, q = bit 3. Each table below is indexed by one of
// those fields.

namespace disasm {
namespace {

const char* const kReg[8]   = { "b", "c", "d", "e", "h", "l", "(hl)", "a" };
const char* const kPair[4]  = { "bc", "de", "hl", "sp" };   // rp
const char* const kPair2[4] = { "bc", "de", "hl", "af" };   // rp2: push, pop
const char* const kCond[8]  = { "nz", "z", "nc", "c", "po", "pe", "p", "m" };
const char* const kAlu[8]   = { "add", "adc", "sub", "sbc", "and", "xor", "or", "cp" };
const char* const kRot[8]   = { "rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl" };
const char* const kIm[8]    = { "0", "0", "1", "2", "0", "0", "1", "2" };
const char* const kBlock[4][4] = {
    { "ldi",  "cpi",  "ini",  "outi" },
    { "ldd",  "cpd",  "ind",  "outd" },
    { "ldir", "cpir", "inir", "otir" },
    { "lddr", "cpdr", "indr", "otdr" },
};

// add, adc and sbc name the accumulator; sub, and, xor, or and cp do not.
bool aluNamesA(int y) { return y == 0 || y == 1 || y == 3; }

// The bytes of one instruction, read front to back. A read past avail marks
// the whole decode as short; the caller then discards what was built.
struct Cursor {
    const uint8_t* bytes;
    size_t         avail;
    size_t         pos   = 0;
    bool           short_ = false;

    uint8_t next()
    {
        if (pos >= avail) {
            short_ = true;
            return 0;
        }
        return bytes[pos++];
    }
    uint16_t next16()
    {
        const uint8_t lo = next();
        const uint8_t hi = next();
        return (uint16_t)(lo | hi << 8);
    }
};

std::string hex8(unsigned v)
{
    char b[8];
    std::snprintf(b, sizeof b, "$%02x", v & 0xFF);
    return b;
}

std::string hex16(unsigned v)
{
    char b[8];
    std::snprintf(b, sizeof b, "$%04x", v & 0xFFFF);
    return b;
}

std::string line(const char* mnemonic, const std::string& operands = {})
{
    std::string s = mnemonic;
    if (operands.empty()) return s;
    s.resize(8, ' ');
    return s + operands;
}

std::string dataBytes(std::initializer_list<uint8_t> v)
{
    std::string ops;
    for (uint8_t b : v) {
        if (!ops.empty()) ops += ',';
        ops += hex8(b);
    }
    return line("db", ops);
}

// "(ix+$05)", "(iy-$80)".
std::string indexed(const char* ix, uint8_t d)
{
    const int s = (int8_t)d;
    return std::string("(") + ix + (s < 0 ? "-" : "+") + hex8(s < 0 ? -s : s) + ")";
}

// Whether a DD or FD in front of this unprefixed opcode changes it, i.e. the
// opcode names HL, H, L or (HL). EX DE,HL and EXX are not changed: the index
// prefix renames HL only where the instruction encodes it as rp or r.
bool touchesHL(uint8_t op)
{
    const int x = op >> 6, y = op >> 3 & 7, z = op & 7, p = y >> 1, q = y & 1;
    switch (x) {
    case 0:
        switch (z) {
        case 1: return q == 1 || p == 2;               // add hl,rp; ld hl,nn
        case 2: return p == 2;                         // ld (nn),hl; ld hl,(nn)
        case 3: return p == 2;                         // inc hl; dec hl
        case 4: case 5: case 6: return y >= 4 && y <= 6;
        default: return false;
        }
    case 1:
        if (op == 0x76) return false;                  // halt
        return (y >= 4 && y <= 6) || (z >= 4 && z <= 6);
    case 2:
        return z >= 4 && z <= 6;
    default:
        return op == 0xE1 || op == 0xE3 || op == 0xE5 || op == 0xE9 || op == 0xF9;
    }
}

// One unprefixed opcode, or the opcode after DD/FD when ix is "ix"/"iy".
std::string decodeMain(Cursor& c, uint8_t op, const char* ix, uint16_t addr)
{
    const int x = op >> 6, y = op >> 3 & 7, z = op & 7, p = y >> 1, q = y & 1;

    // An indexed (hl) operand reads its displacement straight after the
    // opcode, before any immediate (DD 36 d n). In such an instruction H and L
    // keep their names: "ld h,(ix+d)".
    const bool mem = (x == 0 && z >= 4 && z <= 6 && y == 6)
                     || (x == 1 && (y == 6) != (z == 6))
                     || (x == 2 && z == 6);
    std::string memOperand = "(hl)";
    if (ix && mem) memOperand = indexed(ix, c.next());
    const bool renameHL = ix && !mem;

    const std::string hl = ix ? ix : "hl";
    auto reg = [&](int i) -> std::string {
        if (i == 6) return memOperand;
        if (renameHL && i == 4) return hl + "h";
        if (renameHL && i == 5) return hl + "l";
        return kReg[i];
    };
    auto pair = [&](int i) -> std::string { return i == 2 ? hl : kPair[i]; };
    auto pair2 = [&](int i) -> std::string { return i == 2 ? hl : kPair2[i]; };
    auto rel = [&]() {
        const uint8_t d = c.next();
        return hex16((unsigned)(addr + 2 + (int8_t)d));
    };

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            if (y == 0) return line("nop");
            if (y == 1) return line("ex", "af,af'");
            if (y == 2) return line("djnz", rel());
            if (y == 3) return line("jr", rel());
            return line("jr", std::string(kCond[y - 4]) + "," + rel());
        case 1:
            if (q == 0) return line("ld", pair(p) + "," + hex16(c.next16()));
            return line("add", hl + "," + pair(p));
        case 2: {
            static const char* const kInd[2] = { "(bc)", "(de)" };
            if (p < 2) return q == 0 ? line("ld", std::string(kInd[p]) + ",a")
                                     : line("ld", std::string("a,") + kInd[p]);
            const std::string nn = "(" + hex16(c.next16()) + ")";
            const std::string r = p == 2 ? hl : "a";
            return q == 0 ? line("ld", nn + "," + r) : line("ld", r + "," + nn);
        }
        case 3: return line(q == 0 ? "inc" : "dec", pair(p));
        case 4: return line("inc", reg(y));
        case 5: return line("dec", reg(y));
        case 6: {
            const std::string dst = reg(y);
            return line("ld", dst + "," + hex8(c.next()));
        }
        default: {
            static const char* const k[8] = { "rlca", "rrca", "rla", "rra", "daa", "cpl", "scf", "ccf" };
            return line(k[y]);
        }
        }
    case 1:
        if (op == 0x76) return line("halt");
        return line("ld", reg(y) + "," + reg(z));
    case 2:
        return line(kAlu[y], (aluNamesA(y) ? "a," : "") + reg(z));
    default:
        switch (z) {
        case 0: return line("ret", kCond[y]);
        case 1:
            if (q == 0) return line("pop", pair2(p));
            if (p == 0) return line("ret");
            if (p == 1) return line("exx");
            if (p == 2) return line("jp", "(" + hl + ")");
            return line("ld", "sp," + hl);
        case 2: return line("jp", std::string(kCond[y]) + "," + hex16(c.next16()));
        case 3:
            switch (y) {
            case 0: return line("jp", hex16(c.next16()));
            case 2: return line("out", "(" + hex8(c.next()) + "),a");
            case 3: return line("in", "a,(" + hex8(c.next()) + ")");
            case 4: return line("ex", "(sp)," + hl);
            case 5: return line("ex", "de,hl");
            case 6: return line("di");
            case 7: return line("ei");
            default: return {};   // CB: decoded by the caller
            }
        case 4: return line("call", std::string(kCond[y]) + "," + hex16(c.next16()));
        case 5:
            if (q == 0) return line("push", pair2(p));
            return line("call", hex16(c.next16()));   // p 1-3 are prefixes, decoded by the caller
        case 6: return line(kAlu[y], (aluNamesA(y) ? "a," : "") + hex8(c.next()));
        default: return line("rst", hex8(y * 8));
        }
    }
}

// CB xx, or DD CB d xx / FD CB d xx when target is "(ix+d)" / "(iy+d)".
std::string decodeBits(uint8_t op, const std::string* target)
{
    const int x = op >> 6, y = op >> 3 & 7, z = op & 7;
    std::string operand = target ? *target : kReg[z];
    // Indexed rotates, shifts, res and set also write the result into the
    // register the opcode names, unless that is (hl).
    if (target && x != 1 && z != 6) operand += std::string(",") + kReg[z];
    if (x == 0) return line(kRot[y], operand);
    static const char* const k[4] = { "", "bit", "res", "set" };
    return line(k[x], std::to_string(y) + "," + operand);
}

std::string decodeED(Cursor& c)
{
    const uint8_t op = c.next();
    const int x = op >> 6, y = op >> 3 & 7, z = op & 7, p = y >> 1, q = y & 1;

    if (x == 1) {
        switch (z) {
        case 0: return y == 6 ? line("in", "(c)") : line("in", std::string(kReg[y]) + ",(c)");
        case 1: return line("out", std::string("(c),") + (y == 6 ? "0" : kReg[y]));
        case 2: return line(q == 0 ? "sbc" : "adc", std::string("hl,") + kPair[p]);
        case 3: {
            const std::string nn = "(" + hex16(c.next16()) + ")";
            return q == 0 ? line("ld", nn + "," + kPair[p]) : line("ld", std::string(kPair[p]) + "," + nn);
        }
        case 4: return line("neg");
        case 5: return line(y == 1 ? "reti" : "retn");
        case 6: return line("im", kIm[y]);
        default:
            switch (y) {
            case 0: return line("ld", "i,a");
            case 1: return line("ld", "r,a");
            case 2: return line("ld", "a,i");
            case 3: return line("ld", "a,r");
            case 4: return line("rrd");
            case 5: return line("rld");
            default: break;   // ED77, ED7F: no operation
            }
        }
    } else if (x == 2 && z <= 3 && y >= 4) {
        return line(kBlock[y - 4][z]);
    }
    return dataBytes({ 0xED, op });
}

} // namespace

DisasmLine disasmZ80(const uint8_t* bytes, size_t avail, uint16_t addr)
{
    if (bytes == nullptr || avail == 0) return { addr, 0, {} };

    Cursor c{ bytes, avail };
    const uint8_t op = c.next();
    std::string text;

    if (op == 0xCB) {
        text = decodeBits(c.next(), nullptr);
    } else if (op == 0xED) {
        text = decodeED(c);
    } else if (op == 0xDD || op == 0xFD) {
        const char* ix = op == 0xDD ? "ix" : "iy";
        const uint8_t next = avail > 1 ? bytes[1] : 0;
        if (avail > 1 && next == 0xCB) {
            c.next();
            const uint8_t d = c.next();
            const std::string target = indexed(ix, d);
            text = decodeBits(c.next(), &target);
        } else if (avail > 1 && next != 0xDD && next != 0xFD && next != 0xED && touchesHL(next)) {
            text = decodeMain(c, c.next(), ix, addr);
        } else {
            // Nothing for the prefix to change: it costs its own fetch and
            // the next opcode runs as if it were not there.
            text = dataBytes({ op });
        }
    } else {
        text = decodeMain(c, op, nullptr, addr);
    }

    if (c.short_) return { addr, 1, dataBytes({ op }) };
    return { addr, (uint32_t)c.pos, std::move(text) };
}

} // namespace disasm
