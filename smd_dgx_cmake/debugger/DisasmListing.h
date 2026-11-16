#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class IDebugBackend;

// What a 68000 disassembly window shows around an address. No Qt: the window
// turns these lines into rows, and a test reads them as they are.
namespace disasm {

// Text of a line at an address the listing does not read (see listAround).
inline constexpr char kNotReadable[] = "?? (not readable here)";

struct ListingLine {
    uint32_t    addr = 0;
    uint32_t    len  = 0;        // bytes; an unreadable line covers one word
    std::string hex;             // the bytes as words, "4EB9 0000 0300"; empty when unreadable
    std::string text;            // disasm68k's text, or kNotReadable
    bool        readable      = false;
    bool        isPc          = false;   // the 68000 PC is at addr
    bool        hasBreakpoint = false;   // an enabled 68000 execution breakpoint touches the opcode word at addr

    // Where a branch, dbcc, jmp or jsr goes, when its operand names the
    // place: a PC-relative label or an absolute $xxxx.w / $xxxxxxxx.l.
    // Unset for operands that depend on a register, such as (A0) or
    // (d8,PC,D0.w), and for every other instruction.
    std::optional<uint32_t> target;
};

// The line at centre, up to `before` lines that lead up to it and up to
// `after` lines that follow it, in address order. centre is masked to the
// 24-bit bus and rounded down to an even address.
//
// Only two places are read, both through readRegion: the ROM image, from 0 to
// its size in region 0 (as loaded: a cartridge mapper that swaps banks or maps
// SRAM over ROM is not followed), and 68K work RAM, region 1, which the bus
// mirrors through all of $E00000-$FFFFFF; addresses stay as given, so a
// mirror lists as itself. Everything else on the bus goes to the core's read
// handlers, and there a read is an access with side effects: the VDP control
// port drops a half-written command, unmapped space halts the 68000. So it is
// never read: each word there is a kNotReadable line. Lines never wrap past
// either end of the bus.
//
// Instructions have no fixed length, so the lines before centre are found by
// resync: from every even start between centre-2 and centre-0x40 (further
// back when `before` asks for more lines than that can hold), decode forward,
// and keep the start whose stream lands exactly on centre with the most lines;
// when none lands, there are no lines before it. Each instruction is decoded
// from bytes read once for the whole listing, with room after the last line,
// so none is cut short by the fetch.
//
// isPc and hasBreakpoint come from the backend at the time of the call.
std::vector<ListingLine> listAround(IDebugBackend& be, uint32_t centre, int before, int after);

// The target of a branch, dbcc, jmp or jsr in disasm68k's text, masked to the
// 24-bit bus; see ListingLine::target.
std::optional<uint32_t> flowTarget(const std::string& text);

} // namespace disasm
