#pragma once
#include <cstdint>
#include <string>

namespace disasm {

// One decoded instruction, whatever the CPU.
struct DisasmLine {
    uint32_t    addr;   // address of the first byte, as passed in
    uint32_t    len;    // bytes the instruction occupies
    std::string text;   // the decoder's spelling, e.g. "jsr     $f54.w"
};

} // namespace disasm
