#pragma once
#include <cstddef>
#include <cstdint>

#include "DisasmLine.h"

// One 68000 instruction to text. No Qt and no emulator: the caller passes the
// bytes it has already read (logical, big-endian order, as readMemory returns
// them), so the same call serves the live debugger, a ROM image and a test.
namespace disasm {

// Decodes the instruction at bytes[0], which sits at 68000 address addr; addr
// is only used for PC-relative operands and branch targets.
//
// Reads no more than avail bytes. An instruction longer than that comes back
// as "dc.w $xxxx" with len 2, never with operands made up past the buffer. A
// line-A or line-F word comes back as "dc.w $xxxx" too: on a 68000 both are
// traps, not instructions. With fewer than 2 bytes there is no opcode word:
// one byte gives "dc.b $xx" with len 1, none gives an empty line with len 0.
//
// Safe to call from several threads; the calls are serialised inside.
DisasmLine disasm68k(const uint8_t* bytes, size_t avail, uint32_t addr);

} // namespace disasm
