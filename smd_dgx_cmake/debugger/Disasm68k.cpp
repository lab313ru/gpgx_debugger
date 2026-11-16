#include "Disasm68k.h"

#include <cstdio>
#include <mutex>

// By path: core/m68k/m68k.h, the emulator's own Musashi, has the same name.
#include "musashi_dasm/m68k.h"

namespace {

// m68k_disassemble keeps its state in statics (the opcode table it builds on
// first use, the PC it advances, the string it formats into) and reads memory
// through the free functions below, so the buffer they read is global too and
// one mutex covers both.
std::mutex     g_mx;
const uint8_t* g_bytes = nullptr;
size_t         g_avail = 0;
uint32_t       g_base  = 0;

// In 68000 mode Musashi masks every address it reads to 24 bits.
constexpr uint32_t kAddrMask = 0x00FFFFFF;

unsigned int byteAt(unsigned int address)
{
    const uint32_t off = (address - g_base) & kAddrMask;
    return off < g_avail ? g_bytes[off] : 0;
}

disasm::DisasmLine dataWord(uint32_t addr, unsigned word)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "dc.w $%04x", word & 0xFFFF);
    return { addr, 2, buf };
}

} // namespace

extern "C" {

unsigned int m68k_read_disassembler_8(unsigned int address)
{
    return byteAt(address);
}

unsigned int m68k_read_disassembler_16(unsigned int address)
{
    return byteAt(address) << 8 | byteAt(address + 1);
}

unsigned int m68k_read_disassembler_32(unsigned int address)
{
    return m68k_read_disassembler_16(address) << 16 | m68k_read_disassembler_16(address + 2);
}

} // extern "C"

namespace disasm {

DisasmLine disasm68k(const uint8_t* bytes, size_t avail, uint32_t addr)
{
    if (avail == 0 || bytes == nullptr) return { addr, 0, {} };
    if (avail == 1) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "dc.b $%02x", bytes[0]);
        return { addr, 1, buf };
    }

    const unsigned word = (unsigned)bytes[0] << 8 | bytes[1];

    // Musashi decodes F000-F0FF as 68851 PMMU instructions whatever CPU type
    // it is given, and line A as "dc.w ...; opcode 1010". Neither exists on a
    // 68000, so both lines are settled here, in one spelling.
    const unsigned line = word >> 12;
    if (line == 0xA || line == 0xF) return dataWord(addr, word);

    char text[256];   // Musashi formats two 100-byte strings into it
    unsigned int len;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_bytes = bytes;
        g_avail = avail;
        g_base  = addr & kAddrMask;
        len = m68k_disassemble(text, addr, M68K_CPU_TYPE_68000);
        g_bytes = nullptr;
        g_avail = 0;
    }

    // The operands were read as zeros past the end of the buffer: the text
    // describes bytes the caller does not have.
    if (len > avail) return dataWord(addr, word);
    return { addr, len, text };
}

} // namespace disasm
