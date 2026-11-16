#pragma once
#include <cstddef>
#include <cstdint>

#include "DisasmLine.h"

// One Z80 instruction to text. No Qt and no emulator, like disasm68k: the
// caller passes the bytes it has already read from the Z80's address space.
//
// Spelling, fixed:
//   - lowercase; the mnemonic is padded to 8 columns, then the operands:
//     "ld      a,$12", "ex      af,af'", "nop".
//   - numbers are $ and lowercase hex: 8-bit values with 2 digits ("$12"),
//     16-bit values and addresses with 4 ("$1234"). Bit numbers and the
//     interrupt mode are plain digits ("bit     7,(hl)", "im      1").
//   - jr and djnz print the absolute target, wrapping at 16 bits:
//     "jr      nz,$0123", "djnz    $0100".
//   - the index displacement is signed: "(ix+$05)", "(iy-$80)", "(ix+$00)".
//   - "rst     $38", "in      a,($12)", "out     (c),a".
//
// Undocumented instructions are decoded as what the Z80 executes:
//   - DD/FD on H, L: "ld      ixh,$12", "add     a,iyl".
//   - CB30-CB37: "sll     b".
//   - DDCB/FDCB with a register other than (hl) also copies the result into
//     that register: "rlc     (ix+$05),b", "res     0,(ix+$05),b". bit has
//     no such copy: every DDCB 4x-7x is "bit     n,(ix+d)".
//   - ED70 is "in      (c)" (flags only), ED71 is "out     (c),0".
//   - ED mirrors of neg, retn and im are spelled as those instructions; ED4E
//     and ED6E set interrupt mode 0 and are "im      0".
//   - an ED opcode that does nothing is "db      $ed,$xx" with len 2.
//   - a DD or FD in front of an opcode that does not touch HL, H, L or (HL),
//     or in front of another DD, FD or ED, only delays the next opcode: it is
//     "db      $dd" (or $fd) with len 1, and the next line is the instruction.
//
// Reads no more than avail bytes. An instruction longer than that comes back
// as "db      $xx" (its first byte) with len 1, never with operands made up
// past the buffer. With avail 0 the text is empty and len is 0.
//
// No state: safe to call from several threads.
namespace disasm {

// Decodes the instruction at bytes[0], which sits at Z80 address addr; addr
// is only used for relative jump targets.
DisasmLine disasmZ80(const uint8_t* bytes, size_t avail, uint16_t addr);

} // namespace disasm
