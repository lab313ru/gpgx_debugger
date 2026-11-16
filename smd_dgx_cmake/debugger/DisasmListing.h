#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class IDebugBackend;

// What a 68000 or Z80 disassembly window shows around an address. No Qt: the
// window turns these lines into rows, and a test reads them as they are.
namespace disasm {

// Text of a line at an address the listing does not read (see listAround and
// listAroundZ80).
inline constexpr char kNotReadable[] = "?? (not readable here)";

struct ListingLine {
    uint32_t    addr = 0;
    uint32_t    len  = 0;        // bytes; an unreadable line covers one word (68000) or one byte (Z80)
    std::string hex;             // 68000: the bytes as words, "4EB9 0000 0300"; Z80: one
                                 // group per byte, "DD CB 05 06"; empty when unreadable
    std::string text;            // disasm68k's or disasmZ80's text, or kNotReadable
    bool        readable      = false;
    bool        isPc          = false;   // that CPU's PC is at addr
    bool        hasBreakpoint = false;   // an enabled execution breakpoint of that CPU stops at addr

    // Where a jump or call goes, when its operand names the place.
    // 68000: a branch, dbcc, jmp or jsr with a PC-relative label or an
    // absolute $xxxx.w / $xxxxxxxx.l. Z80: jp, jr, djnz, call and rst, with or
    // without a condition. Unset for operands that depend on a register, such
    // as (A0), (d8,PC,D0.w) or jp (hl), and for every other instruction.
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

// The same for the Z80: the line at centre, up to `before` lines leading up
// to it and up to `after` following it, in address order, in the Z80's own
// 16-bit space. Lines never wrap past $0000 or $FFFF.
//
// Nothing goes through readMemory or the core's read handlers. What the Z80
// sees, and what of it is read:
//   $0000-$3FFF  its 8K of sound RAM, mirrored once: readRegion region 2 at
//                a & $1FFF. A mirror lists as itself.
//   $4000-$7FFF  YM2612, the bank register, VDP and PSG. A read there is an
//                access (the YM2612 status, the VDP ports), so never: each
//                byte is a kNotReadable line.
//   $8000-$FFFF  the bank window onto 68000 address (bank | (a & $7FFF)),
//                bank from getZ80Regs. memz80.c serves a window read from
//                the 68000 page's memory only where zbank_memory_map has no
//                read handler for it; every other page runs a handler. With
//                the cartridge as loaded (md_cart.c), that is ROM, pages
//                $00-$3F: read through region 0, and only below the ROM's
//                size (past it the core gives a mirror or padding, which
//                the listing does not guess at). Everything else is a
//                handler: $40-$7F reads $FF, $80-$DF locks up the machine
//                or is the VDP, $A1 is I/O, and $E0-$FF, the 68000's work
//                RAM, reads $FF too: the Z80 can write that RAM but not read
//                it. So region 1 is never read here, and none of those lines
//                is readable. A mapper that swaps ROM banks or maps SRAM
//                over ROM is not followed, as in listAround.
//
// Lines before centre are found by resync, as listAround does: from every
// start between centre-1 and centre-$20 (further back when `before` asks for
// more lines than that can hold), decode forward, and keep the start whose
// stream lands exactly on centre with the most lines; when none lands there
// are no lines before it. The bytes are read once, with room after the last
// line, so no instruction is cut short by the fetch.
//
// isPc is the Z80 PC; hasBreakpoint is an enabled, non-VDP, PC breakpoint of
// the Z80 whose range holds addr. Both come from the backend at the call.
std::vector<ListingLine> listAroundZ80(IDebugBackend& be, uint16_t centre, int before, int after);

// The target of jp nn, jp cc,nn, jr, jr cc, djnz, call, call cc and rst in
// disasmZ80's text; unset for jp (hl), (ix), (iy) and everything else.
std::optional<uint32_t> flowTargetZ80(const std::string& text);

} // namespace disasm
