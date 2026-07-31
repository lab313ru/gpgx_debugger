#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct M68kRegs {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc, sr, usp, isp;
};

struct Z80Regs {
    uint16_t af, bc, de, hl;
    uint16_t af2, bc2, de2, hl2;
    uint16_t ix, iy, sp, pc;
    uint8_t  i, r, im, iff1, iff2, halt;
    // Base of the 68000-space window the Z80 sees at $8000..$FFFF. Not a CPU
    // register — it lives in the bus glue, shifted in one bit at a time
    // through $6000 — but a Z80 disassembly is unreadable without it: sample
    // banks and driver overlays all arrive through that window.
    uint32_t bank;
};

// Byte-order contract for the raw pointers below (LSB_FIRST build). The core
// accesses all of these as native uint16/uint32 words, so on a little-endian
// host the two bytes of every word are stored in reverse:
//   vram / sat  : the logical Genesis byte at address A lives at ptr[A ^ 1].
//   cram / vsram: read per entry as a native 16-bit word, lo | (hi << 8).
//
// CRAM entries are NOT in the 16-bit bus format 0000BBB0GGG0RRR0. The core
// packs them on write (vdp_ctrl.c, "Pack 16-bit bus data ... to 9-bit CRAM
// data") into 9 bits, BBBGGGRRR — red in the low bits. Decode with
// cram_to_rgb() below; decoding a packed entry as bus data scrambles the
// channels (which is exactly what it looks like: wrong colors everywhere).
struct VdpState {
    uint8_t  reg[0x20];
    uint16_t status;
    uint32_t dma_len;      // in words
    uint32_t dma_src;      // BYTE address (regs 21-23 hold a word address)
    // As the core assigns it (vdp_ctrl.c), not the order reg 23 suggests:
    uint8_t  dma_type;     // 0=bus->CRAM/VSRAM, 1=bus->VRAM, 2=VRAM fill, 3=VRAM copy
    uint16_t vdp_addr;     // pending access: address register
    uint8_t  vdp_code;     // pending access: code register (low nibble = target)
    const uint8_t* vram;   // 0x10000 bytes – points into gpgx globals
    const uint8_t* cram;   // 0x80 bytes (64 colors)
    const uint8_t* vsram;  // 0x80 bytes (40 x 11-bit)
    const uint8_t* sat;    // 0x400 bytes – internal sprite attribute table copy
};

// Raw sound chip registers (shadow copies maintained by the core under HOOK_CPU).
struct SoundState {
    uint8_t fm[2][0x100];  // YM2612 raw registers: part I (ch 1-3, globals), part II (ch 4-6)
    int     psg[8];        // SN76489: [0,2,4]=tone period 0-2, [1,3,5]=attenuation 0-2,
                           //          [6]=noise control, [7]=noise attenuation
};

// Who is on the other end of a control socket.
//
// Two emulators can be running at once, so an agent (or a Z80 database) that
// connects to a fixed port has no way to know whose game it is looking at.
// crc is the calculated ROM checksum, not the header's — a hacked or patched
// ROM must not pass for the original.
struct SessionInfo {
    unsigned    pid  = 0;
    unsigned short port = 0;
    unsigned short crc  = 0;      // rominfo.realchecksum
    std::string serial;           // rominfo.product
    std::string name;             // rominfo.domestic, trimmed
};

// A viewable/editable memory region (parity with the Gens hex editor).
// readRegion()/writeRegion() operate in LOGICAL byte order — the backend
// applies any host byte-swapping internally.
struct MemRegion {
    int         id;        // stable id, see GpgxBackend region table
    std::string name;      // "ROM", "RAM 68K", "RAM Z80", "VRAM", "CRAM", "VSRAM", "Regs ..."
    uint32_t    base;      // display base address (e.g. 0xFF0000 for 68K RAM)
    uint32_t    size;      // bytes
    bool        writable;
};

// CRAM entry (packed 9-bit BBBGGGRRR) <-> 8-bit per channel.
// 3 bits scale to 0..224 (v << 5), the same range the original Gens tools
// displayed, so screenshots stay comparable.
inline void cram_to_rgb(uint16_t packed, int& r, int& g, int& b)
{
    r = ((packed >> 0) & 7) << 5;
    g = ((packed >> 3) & 7) << 5;
    b = ((packed >> 6) & 7) << 5;
}

inline uint16_t rgb_to_cram(int r, int g, int b)
{
    return static_cast<uint16_t>((((r >> 5) & 7) << 0)
                               | (((g >> 5) & 7) << 3)
                               | (((b >> 5) & 7) << 6));
}

// Controller buttons, as a bitmask for one pad. Mirrors the core's INPUT_*
// values so UI code never has to include emulator headers.
enum PadButton : uint16_t {
    PAD_UP    = 0x0001,
    PAD_DOWN  = 0x0002,
    PAD_LEFT  = 0x0004,
    PAD_RIGHT = 0x0008,
    PAD_B     = 0x0010,
    PAD_C     = 0x0020,
    PAD_A     = 0x0040,
    PAD_START = 0x0080,
    PAD_Z     = 0x0100,
    PAD_Y     = 0x0200,
    PAD_X     = 0x0400,
    PAD_MODE  = 0x0800,
};

// VDP memories share one linear space for breakpoints, matching what the IDA
// plugin maps its pseudo-segments onto: VRAM at 0, CRAM at +0x10000,
// VSRAM at +0x20000. A breakpoint with is_vdp set is an address in here.
enum : uint32_t { VDP_BP_VRAM = 0x00000, VDP_BP_CRAM = 0x10000, VDP_BP_VSRAM = 0x20000 };

enum class BpType : uint8_t { PC = 1, Read = 2, Write = 3 };

// Which processor a breakpoint or a stop belongs to. One machine, two CPUs:
// without this the Z80 — which executes far more instructions per frame than
// the 68000 — swallows every step and matches every address.
enum class Cpu : uint8_t { M68K = 0, Z80 = 1 };

struct Breakpoint {
    int      id      = 0;
    BpType   type    = BpType::PC;
    Cpu      cpu     = Cpu::M68K;
    bool     is_vdp  = false;
    bool     enabled = true;
    uint32_t start   = 0;
    uint32_t end     = 0;
    std::string condition;
    uint32_t elang   = 0;   // which client language `condition` is written in
};
