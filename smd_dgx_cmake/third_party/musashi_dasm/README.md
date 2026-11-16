# Musashi disassembler

The 68000 disassembler from Musashi by Karl Stenerud, used by
`debugger/Disasm68k.cpp`.

- Upstream: https://github.com/kstenerud/Musashi
- Commit: `313ebf1bd9f4d0d93341eb5ce21fd8a119e9dbdd` (2026-03-08)
- Files: `m68kdasm.c`, `m68k.h`, `m68kconf.h`, byte-identical to that commit
- Licence: MIT. `LICENSE.txt` is the "LICENSE AND COPYRIGHT" section of the
  upstream `readme.txt`; the same text heads `m68kdasm.c` and `m68k.h`.

Only `m68k_disassemble` is called. It reads memory through
`m68k_read_disassembler_8/16/32`, which `Disasm68k.cpp` implements over the
caller's buffer.

This is not the Musashi in `core/m68k`: that one is version 3.3, under a
non-commercial licence, and ships no disassembler. Its `m68k.h` has the same
name as the one here and is on the include path of everything that links the
emulator core, so this directory is never added to an include path that
reaches other targets: `m68kdasm.c` finds its headers beside itself, and the
wrapper names them as `musashi_dasm/m68k.h`.

To update, fetch the same three files at a newer commit and change the hash
above.
