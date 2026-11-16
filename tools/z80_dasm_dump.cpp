// Feeds disasm::disasmZ80 for tools/z80_dasm_oracle.py, which compiles it
// with g++ together with smd_dgx_cmake/debugger/DisasmZ80.cpp. Not part of
// the CMake build.
//
// stdin, one case per line: "<addr hex> <bytes hex>", e.g. "4000 dd21341200".
// stdout, one line per case: "<len>\t<text>".

#include "DisasmZ80.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

int main()
{
    std::string addrHex, bytesHex;
    while (std::cin >> addrHex >> bytesHex) {
        std::vector<uint8_t> bytes;
        for (size_t i = 0; i + 1 < bytesHex.size(); i += 2)
            bytes.push_back((uint8_t)std::stoul(bytesHex.substr(i, 2), nullptr, 16));
        const auto line = disasm::disasmZ80(bytes.data(), bytes.size(),
                                            (uint16_t)std::stoul(addrHex, nullptr, 16));
        std::printf("%u\t%s\n", line.len, line.text.c_str());
    }
    return 0;
}
