#include "GpgxBackend.h"

extern "C" {
#include <shared.h>           // uint8/uint16/uint32/int16 macros — must be first
#include <m68k.h>             // m68ki_cpu_core m68k, m68k_get_reg, cpu_memory_map
#include <z80.h>              // Z80_Regs Z80
#include <vdp_ctrl.h>         // reg[], vram[], cram[], vsram[], sat[], status
#include <mem68k.h>
#include <genesis.h>          // work_ram[], zram[], cart macro
#include <input_hw/input.h>   // t_input input, MAX_DEVICES
#include <system.h>           // vdp_pal, MCYCLES_PER_LINE
#include <sound/sound.h>      // fm_debug_regs
#include <sound/psg.h>        // psg_debug_regs
#include <state.h>            // state_save/state_load, STATE_SIZE
#include <loadrom.h>          // rominfo: name, serial, calculated checksum
#include <debug/cpuhook.h>
}

#include <fstream>

#include <gx/gx.hpp>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>
#include <iterator>

GpgxBackend* g_gpgxBackend = nullptr;

static void cpuHookShim(hook_type_t type, int width, unsigned int addr, unsigned int value)
{
    if (g_gpgxBackend)
        g_gpgxBackend->onCpuHook(static_cast<int>(type), width, addr, value);
}

GpgxBackend::GpgxBackend()  { g_gpgxBackend = this; set_cpu_hook(cpuHookShim); }

GpgxBackend::~GpgxBackend()
{
    // Only tear down the hook if it is still ours. A second backend claims the
    // global on construction, and an unconditional clear here would silently
    // kill breakpoints and stepping for whoever is actually running.
    if (g_gpgxBackend == this) {
        set_cpu_hook(nullptr);
        g_gpgxBackend = nullptr;
    }
}

SessionInfo GpgxBackend::sessionInfo() const
{
    SessionInfo s;
    if (!running_.load()) return s;
    s.crc = rominfo.realchecksum;
    // The header fields are fixed-width and space-padded, not terminated.
    auto trimmed = [](const char* p, size_t n) {
        std::string v(p, ::strnlen(p, n));
        while (!v.empty() && (unsigned char)v.back() <= 0x20) v.pop_back();
        return v;
    };
    s.serial = trimmed(rominfo.product, sizeof rominfo.product);
    s.name   = trimmed(rominfo.domestic, sizeof rominfo.domestic);
    return s;
}

// ---------------------------------------------------------------------------
// CPU state
// ---------------------------------------------------------------------------
M68kRegs GpgxBackend::getM68kRegs()
{
    M68kRegs r{};
    for (int i = 0; i < 8; ++i) r.d[i] = m68k_get_reg(static_cast<m68k_register_t>(M68K_REG_D0 + i));
    for (int i = 0; i < 8; ++i) r.a[i] = m68k_get_reg(static_cast<m68k_register_t>(M68K_REG_A0 + i));
    r.pc  = m68k_get_reg(M68K_REG_PC);
    r.sr  = m68k_get_reg(M68K_REG_SR);
    r.usp = m68k_get_reg(M68K_REG_USP);
    r.isp = m68k_get_reg(M68K_REG_ISP);
    return r;
}

void GpgxBackend::setM68kRegs(const M68kRegs& r)
{
    for (int i = 0; i < 8; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_D0 + i), r.d[i]);
    for (int i = 0; i < 8; ++i) m68k_set_reg(static_cast<m68k_register_t>(M68K_REG_A0 + i), r.a[i]);
    m68k_set_reg(M68K_REG_PC,  r.pc);
    m68k_set_reg(M68K_REG_SR,  r.sr);
    m68k_set_reg(M68K_REG_USP, r.usp);
    m68k_set_reg(M68K_REG_ISP, r.isp);
}

Z80Regs GpgxBackend::getZ80Regs()
{
    Z80Regs r{};
    r.af  = Z80.af.w.l;  r.bc  = Z80.bc.w.l;  r.de  = Z80.de.w.l;  r.hl  = Z80.hl.w.l;
    r.af2 = Z80.af2.w.l; r.bc2 = Z80.bc2.w.l; r.de2 = Z80.de2.w.l; r.hl2 = Z80.hl2.w.l;
    r.ix  = Z80.ix.w.l;  r.iy  = Z80.iy.w.l;
    r.sp  = Z80.sp.w.l;  r.pc  = Z80.pc.w.l;
    r.i   = Z80.i; r.r = Z80.r; r.im = Z80.im;
    r.iff1 = Z80.iff1; r.iff2 = Z80.iff2; r.halt = Z80.halt;
    r.bank = ::zbank;
    return r;
}

void GpgxBackend::setZ80Regs(const Z80Regs& r)
{
    Z80.af.w.l  = r.af;  Z80.bc.w.l  = r.bc;  Z80.de.w.l  = r.de;  Z80.hl.w.l  = r.hl;
    Z80.af2.w.l = r.af2; Z80.bc2.w.l = r.bc2; Z80.de2.w.l = r.de2; Z80.hl2.w.l = r.hl2;
    Z80.ix.w.l  = r.ix;  Z80.iy.w.l  = r.iy;  Z80.sp.w.l  = r.sp;  Z80.pc.w.l  = r.pc;
    Z80.i = r.i; Z80.r = r.r; Z80.im = r.im;
    Z80.iff1 = r.iff1; Z80.iff2 = r.iff2; Z80.halt = r.halt;
}

VdpState GpgxBackend::getVdpState()
{
    VdpState v{};
    std::memcpy(v.reg, reg, sizeof(v.reg));
    v.status   = ::status;
    // reg 19 is the LOW half of the length and reg 20 the high one — the same
    // order the core itself uses (vdp_ctrl.c: (reg[20] << 8) | reg[19]).
    // Swapping them turns a 0x140-word transfer into a nonsensical 0x4001.
    v.dma_len  = ((reg[20] & 0xFF) << 8) | (reg[19] & 0xFF);
    // Registers 21-23 hold a *word* address; report the byte address, which is
    // what every caller actually wants to compare against a memory map.
    v.dma_src  = (uint32_t((reg[21] & 0xFF) | ((reg[22] & 0xFF) << 8)
                           | ((reg[23] & 0x7F) << 16)) << 1) & 0xFFFFFF;
    v.dma_type = ::dma_type;
    v.vram  = ::vram;
    v.cram  = ::cram;
    v.vsram = ::vsram;
    v.sat   = ::sat;
    unsigned int a = 0, c = 0;
    vdp_debug_get_access(&a, &c);
    v.vdp_addr = uint16_t(a);
    v.vdp_code = uint8_t(c);
    return v;
}

void GpgxBackend::setVdpReg(int idx, uint8_t value)
{
    if (idx >= 0 && idx < 0x20) reg[idx] = value;
}

SoundState GpgxBackend::getSoundState()
{
    SoundState s{};
    std::memcpy(s.fm, fm_debug_regs, sizeof(s.fm));
    const int* p = psg_debug_regs();
    for (int i = 0; i < 8; ++i) s.psg[i] = p[i];
    return s;
}

// ---------------------------------------------------------------------------
// Memory
// ---------------------------------------------------------------------------
// map->base regions (ROM/RAM) are stored word-swapped on LSB_FIRST hosts;
// XOR 1 converts to logical Genesis byte order. read8/write8 handlers take
// real bus addresses and need no correction.
std::vector<uint8_t> GpgxBackend::readMemory(uint32_t addr, uint32_t size)
{
    std::vector<uint8_t> buf(size, 0xFF);
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t a = (addr + i) & 0xFFFFFF;
        const cpu_memory_map* map = &m68k.memory_map[(a >> 16) & 0xFF];
        if (map->base)  buf[i] = map->base[(a & 0xFFFF) ^ 1];
        else if (map->read8) buf[i] = static_cast<uint8_t>(map->read8(a));
    }
    return buf;
}

bool GpgxBackend::writeMemory(uint32_t addr, const uint8_t* data, uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t a = (addr + i) & 0xFFFFFF;
        cpu_memory_map* map = &m68k.memory_map[(a >> 16) & 0xFF];
        if (map->base) map->base[(a & 0xFFFF) ^ 1] = data[i];
        else if (map->write8) map->write8(a, data[i]);
        else return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Save states — see the threading note on IDebugBackend
// ---------------------------------------------------------------------------
bool GpgxBackend::saveState(const char* path)
{
    if (!path || !*path) return false;
    std::vector<uint8_t> buf(STATE_SIZE);
    const int len = state_save(buf.data());
    if (len <= 0) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(buf.data()), len);
    return static_cast<bool>(f);
}

bool GpgxBackend::loadState(const char* path)
{
    if (!path || !*path) return false;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize size = f.tellg();
    if (size <= 0 || size > static_cast<std::streamsize>(STATE_SIZE)) return false;
    f.seekg(0);
    std::vector<uint8_t> buf(STATE_SIZE, 0);
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) return false;
    return state_load(buf.data()) != 0;
}

bool GpgxBackend::runSafely(const std::function<void()>& fn)
{
    if (!fn) return false;

    // Preferred: hand it to the host's emulation thread.
    if (safeExec_) return safeExec_(fn);

    // Already paused — the emulation thread is parked in firePause(), so
    // nothing is touching the core.
    if (paused_.load()) { fn(); return true; }

    if (!running_.load()) { fn(); return true; }   // nothing running to race

    // No host queue: stop the world ourselves.
    pause();
    for (int i = 0; i < 500 && !paused_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!paused_.load()) return false;
    fn();
    resume();
    return true;
}

// ---------------------------------------------------------------------------
// Controller input
// ---------------------------------------------------------------------------
void GpgxBackend::setPad(int port, uint16_t buttons)
{
    if (port >= 0 && port < MAX_DEVICES)
        input.pad[port] = buttons;
}

uint16_t GpgxBackend::getPad(int port)
{
    return (port >= 0 && port < MAX_DEVICES) ? input.pad[port] : 0;
}

// ---------------------------------------------------------------------------
// Memory regions (hex editor / RAM tools) — parity with the Gens hex editor
// ---------------------------------------------------------------------------
namespace {
struct RegionDesc {
    int id; const char* name; uint32_t base; bool writable; uint8_t swap;
};
// ids are a stable contract with the views
constexpr RegionDesc kRegions[] = {
    { 0, "ROM",       0x000000, true,  1 },
    { 1, "RAM 68K",   0xFF0000, true,  1 },
    { 2, "RAM Z80",   0xA00000, true,  0 },
    { 3, "VRAM",      0x000000, true,  1 },
    // CRAM/VSRAM hold core-internal packed words, not Genesis bus bytes, and
    // the core reads them as native uint16 — expose them raw (see DebugState.h).
    { 4, "CRAM",      0x000000, true,  0 },
    { 5, "VSRAM",     0x000000, true,  0 },
    { 6, "Regs M68K", 0x000000, true,  0 },
    { 7, "Regs Z80",  0x000000, false, 0 },
    { 8, "Regs VDP",  0x000000, true,  0 },
};

uint8_t* regionArray(int id, uint32_t& size)
{
    switch (id) {
    case 0: size = cart.romsize;  return cart.rom;
    case 1: size = 0x10000;       return work_ram;
    case 2: size = 0x2000;        return zram;
    case 3: size = 0x10000;       return vram;
    case 4: size = 0x80;          return cram;
    case 5: size = 0x80;          return vsram;
    case 8: size = 0x20;          return reg;
    default: size = 0;            return nullptr;
    }
}

void putBE32(std::vector<uint8_t>& v, uint32_t off, uint32_t x)
{
    if (off + 3 < v.size()) {
        v[off] = x >> 24; v[off+1] = x >> 16; v[off+2] = x >> 8; v[off+3] = x;
    }
}
void putBE16(std::vector<uint8_t>& v, uint32_t off, uint16_t x)
{
    if (off + 1 < v.size()) { v[off] = x >> 8; v[off+1] = (uint8_t)x; }
}
} // namespace

std::vector<MemRegion> GpgxBackend::getMemRegions()
{
    std::vector<MemRegion> out;
    for (const auto& r : kRegions) {
        MemRegion m;
        m.id = r.id; m.name = r.name; m.base = r.base; m.writable = r.writable;
        switch (r.id) {
        case 6: m.size = 64; break;               // D0-D7,A0-A7 as BE dwords
        case 7: m.size = 24; break;               // 12 BE word pairs
        default: { uint32_t s = 0; regionArray(r.id, s); m.size = s; }
        }
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<uint8_t> GpgxBackend::readRegion(int id, uint32_t off, uint32_t size)
{
    if (id == 6) { // M68K regs: D0..D7, A0..A7 as big-endian dwords
        std::vector<uint8_t> full(64, 0);
        M68kRegs r = getM68kRegs();
        for (int i = 0; i < 8; ++i) { putBE32(full, i*4, r.d[i]); putBE32(full, 32 + i*4, r.a[i]); }
        std::vector<uint8_t> out;
        for (uint32_t i = 0; i < size && off + i < full.size(); ++i) out.push_back(full[off + i]);
        return out;
    }
    if (id == 7) { // Z80 regs: AF,BC,DE,HL,AF',BC',DE',HL',IX,IY,SP,PC as BE words
        std::vector<uint8_t> full(24, 0);
        Z80Regs z = getZ80Regs();
        const uint16_t regs16[12] = { z.af, z.bc, z.de, z.hl, z.af2, z.bc2, z.de2, z.hl2, z.ix, z.iy, z.sp, z.pc };
        for (int i = 0; i < 12; ++i) putBE16(full, i*2, regs16[i]);
        std::vector<uint8_t> out;
        for (uint32_t i = 0; i < size && off + i < full.size(); ++i) out.push_back(full[off + i]);
        return out;
    }

    uint32_t rsize = 0;
    uint8_t* arr = regionArray(id, rsize);
    const uint8_t swap = (id >= 0 && id < (int)std::size(kRegions)) ? kRegions[id].swap : 0;
    std::vector<uint8_t> out;
    if (!arr) return out;
    out.reserve(size);
    for (uint32_t i = 0; i < size && off + i < rsize; ++i)
        out.push_back(arr[(off + i) ^ swap]);
    return out;
}

bool GpgxBackend::writeRegion(int id, uint32_t off, const uint8_t* data, uint32_t size)
{
    if (id == 7) return false;
    if (id == 6) { // poke M68K regs through the byte image
        std::vector<uint8_t> full = readRegion(6, 0, 64);
        if (full.size() < 64) return false;
        for (uint32_t i = 0; i < size && off + i < 64; ++i) full[off + i] = data[i];
        M68kRegs r = getM68kRegs();
        for (int i = 0; i < 8; ++i) {
            r.d[i] = (full[i*4] << 24) | (full[i*4+1] << 16) | (full[i*4+2] << 8) | full[i*4+3];
            r.a[i] = (full[32+i*4] << 24) | (full[32+i*4+1] << 16) | (full[32+i*4+2] << 8) | full[32+i*4+3];
        }
        setM68kRegs(r);
        return true;
    }

    uint32_t rsize = 0;
    uint8_t* arr = regionArray(id, rsize);
    if (!arr) return false;
    const uint8_t swap = (id >= 0 && id < (int)std::size(kRegions)) ? kRegions[id].swap : 0;
    if (!kRegions[id].writable) return false;
    for (uint32_t i = 0; i < size && off + i < rsize; ++i)
        arr[(off + i) ^ swap] = data[i];
    return true;
}

// The Z80's own 16-bit view. Only the parts that can be read without side
// effects are served: $4000 (YM2612) and $7F00 (VDP) are live hardware ports
// where a read changes state, so they stay 0xFF rather than being "helpfully"
// sampled behind the driver's back.
std::vector<uint8_t> GpgxBackend::readZ80Memory(uint16_t addr, uint16_t size)
{
    std::vector<uint8_t> buf(size, 0xFF);
    for (uint16_t i = 0; i < size; ++i) {
        const uint16_t a = (uint16_t)(addr + i);
        if (a < 0x4000) {
            buf[i] = ::zram[a & 0x1FFF];            // 8K, mirrored through $3FFF
        } else if (a >= 0x8000) {
            // Bank window into 68000 space. Same word-swap correction as
            // readMemory: the core stores ROM/RAM byte-swapped on LSB_FIRST.
            const uint32_t b = (::zbank | (a & 0x7FFF)) & 0xFFFFFF;
            const cpu_memory_map* map = &m68k.memory_map[(b >> 16) & 0xFF];
            if (map->base) buf[i] = map->base[(b & 0xFFFF) ^ 1];
        }
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Breakpoints
// ---------------------------------------------------------------------------
int GpgxBackend::addBreakpoint(const Breakpoint& bp)
{
    std::lock_guard<std::mutex> lk(bpMutex_);
    Breakpoint b = bp; b.id = nextBpId_++;
    breakpoints_.push_back(b);
    bpCount_.store(int(breakpoints_.size()), std::memory_order_relaxed);
    return b.id;
}
void GpgxBackend::removeBreakpoint(int id)
{
    std::lock_guard<std::mutex> lk(bpMutex_);
    breakpoints_.erase(std::remove_if(breakpoints_.begin(), breakpoints_.end(),
        [id](const Breakpoint& b){ return b.id == id; }), breakpoints_.end());
    bpCount_.store(int(breakpoints_.size()), std::memory_order_relaxed);
}
void GpgxBackend::clearBreakpoints() { std::lock_guard<std::mutex> lk(bpMutex_); breakpoints_.clear(); bpCount_.store(0, std::memory_order_relaxed); }
std::vector<Breakpoint> GpgxBackend::getBreakpoints() { std::lock_guard<std::mutex> lk(bpMutex_); return breakpoints_; }

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------
void GpgxBackend::pause()   { stepInto_.store(true); }
void GpgxBackend::resume()  { paused_.store(false); if (resumeCb_) resumeCb_(); }
void GpgxBackend::stepInto(Cpu cpu)
{
    paused_.store(false);
    if (cpu == Cpu::Z80) stepIntoZ80_.store(true);
    else                 stepInto_.store(true);
}

void GpgxBackend::stepOver(Cpu cpu)
{
    if (cpu == Cpu::Z80) { stepOverZ80(); return; }

    uint32_t pc = m68k_get_reg(M68K_REG_PC);
    const cpu_memory_map* map = &m68k.memory_map[(pc >> 16) & 0xFF];
    uint16_t opc = 0;
    // storage is word-swapped on LE hosts: logical hi byte lives at addr^1
    if (map->base) opc = (static_cast<uint16_t>(map->base[(pc & 0xFFFF) ^ 1]) << 8) | map->base[((pc+1) & 0xFFFF) ^ 1];
    if ((opc & 0xFFC0) == 0x4E80) {
        // jsr <ea>: how far to step depends on the addressing mode, since the
        // extension words are part of the instruction. Getting this wrong sets
        // the resume address inside the operand and the step never lands.
        const int mod = (opc >> 3) & 7;
        const int reg = opc & 7;
        int words = 1;                       // the opcode itself

        auto countExtension = [&] {
            // Brief format is one word. Bit 8 selects the 68020 full format,
            // which a 68000 never produces — handled anyway so a 68020 database
            // does not mis-step.
            const uint16_t ext = opcodeAt(pc + 2);
            words += 1;
            if (ext & 0x0100) {
                const int bd = (ext >> 4) & 3;   // base displacement
                const int od = ext & 3;          // outer displacement
                if (bd == 2) words += 1; else if (bd == 3) words += 2;
                if (od == 2) words += 1; else if (od == 3) words += 2;
            }
        };

        switch (mod) {
        case 2: break;                       // (An)
        case 5: words += 1; break;           // (d16,An)
        case 6: countExtension(); break;     // (d8,An,Xn)
        case 7:
            switch (reg) {
            case 0: words += 1; break;       // (xxx).W
            case 1: words += 2; break;       // (xxx).L
            case 2: words += 1; break;       // (d16,PC)
            case 3: countExtension(); break; // (d8,PC,Xn)
            default: break;
            }
            break;
        default: break;                      // not a valid jsr destination
        }
        stepOverAddr_.store(static_cast<int>(pc + words * 2));
    } else if ((opc & 0xFF00) == 0x6100) {
        int off = (opc & 0xFF) == 0 ? 2 : (opc & 0xFF) == 0xFF ? 3 : 1;
        stepOverAddr_.store(static_cast<int>(pc + off * 2));
    } else { stepInto_.store(true); return; }
    paused_.store(false); if (resumeCb_) resumeCb_();
}

bool GpgxBackend::loadRom(const char* path)
{
    gx::init();

    // gx::init() zeroes the bitmap descriptor but allocates no framebuffer —
    // supplying it is the host's job, and the VDP renderer writes through the
    // pointer on the very first frame. Own it here so every host (standalone
    // frontend, IDA plugin, anything future) is safe by construction.
    // Sized for the largest geometry gpgx can produce at 32bpp.
    static std::vector<uint8_t> framebuffer(720 * 576 * 4, 0);
    gx::bitmap_data() = framebuffer.data();

    { std::lock_guard<std::mutex> lk(callstackMutex_); callstack_.clear(); callstackZ80_.clear(); }
    abandoned_.store(false);        // a previous shutdown must not mute this run

    bool ok = gx::load_rom(path);
    if (ok) running_.store(true);
    return ok;
}

// ---------------------------------------------------------------------------
// CPU hook
// ---------------------------------------------------------------------------
void GpgxBackend::onCpuHook(int type, int width, uint32_t addr, uint32_t /*value*/)
{
    if (type & HOOK_M68K_E) {
        // codemap: record predecessor of every executed ROM/RAM address
        if (lastPc_ && addr && addr < MAXROMSIZE) {
            std::lock_guard<std::mutex> lk(codemapMutex_);
            codemap_[addr] = lastPc_;
        }
        lastPc_ = addr;
        trackCall(addr);
        bool brk = stepInto_.exchange(false);
        if (!brk) { int so = stepOverAddr_.load(); if (so >= 0 && (uint32_t)so == addr) { stepOverAddr_.store(-1); brk = true; } }
        int hit = -1;
        if (!brk) { hit = matchBreakpoint(type, addr, 2); brk = hit >= 0; }  // opcode word
        if (brk) firePause(addr, Cpu::M68K, hit);
    } else if (type & (HOOK_M68K_R | HOOK_M68K_W)) {
        const int hit = matchBreakpoint(type, addr, width);
        if (hit >= 0) firePause(lastPc_, Cpu::M68K, hit);
    } else if (type & HOOK_Z80_E) {
        onZ80Exec(addr & 0xFFFF);
    } else if (type & (HOOK_Z80_R | HOOK_Z80_W)) {
        // Report the Z80's own PC, not the 68000's: a sound driver writing to
        // its work RAM is a Z80 event, and stopping at a 68000 address would
        // point the user at code that has nothing to do with it.
        const int hit = matchBreakpoint(type, addr & 0xFFFF, width);
        if (hit >= 0) firePause(lastPcZ80_, Cpu::Z80, hit);
    } else if (type & (HOOK_VRAM_R | HOOK_VRAM_W | HOOK_CRAM_R | HOOK_CRAM_W |
                       HOOK_VSRAM_R | HOOK_VSRAM_W)) {
        // The core reports an offset within one VDP memory; breakpoints live in
        // the combined space, so bias it the same way the IDA plugin does.
        uint32_t linear = addr & 0xFFFF;
        if (type & (HOOK_CRAM_R  | HOOK_CRAM_W))  linear += VDP_BP_CRAM;
        else if (type & (HOOK_VSRAM_R | HOOK_VSRAM_W)) linear += VDP_BP_VSRAM;
        const int hit = matchBreakpoint(type, linear, width);
        if (hit >= 0) firePause(lastPc_, Cpu::M68K, hit);
    }
}

std::map<uint32_t, uint32_t> GpgxBackend::takeCodemap()
{
    std::lock_guard<std::mutex> lk(codemapMutex_);
    std::map<uint32_t, uint32_t> out;
    out.swap(codemap_);
    return out;
}

void GpgxBackend::abortRunControl()
{
    abandoned_.store(true);
    stepInto_.store(false);
    stepIntoZ80_.store(false);
    stepOverAddr_.store(-1);
    stepOverAddrZ80_.store(-1);
    resume();
}

void GpgxBackend::firePause(uint32_t pc, Cpu cpu, int bpId)
{
    if (abandoned_.load()) return;      // shutting down: do not park the thread
    lastBpId_.store(bpId);
    paused_.store(true);
    if (pauseCb_) pauseCb_(pc, cpu);
    // Block the emulation thread until resumed. While blocked, pump host
    // commands (resume/step/read) so run-control can proceed — nested-loop
    // model of the original Gens debugger.
    while (paused_.load() && !abandoned_.load()) {
        if (pausePump_) pausePump_();
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Returns the id of the breakpoint that matched, or -1. Which one matched is
// what an agent actually needs: it sets several and then has to know which of
// them it is now looking at.
int GpgxBackend::matchBreakpoint(int type, uint32_t addr, int width)
{
    if (bpCount_.load(std::memory_order_relaxed) == 0) return -1;   // hot path

    // A VDP access carries an address in the VDP linear space, so it may only
    // match breakpoints marked is_vdp — and a bus breakpoint must never be
    // triggered by one. Without this the two address spaces alias and VDP
    // breakpoints fire on unrelated RAM.
    const bool vdpAccess = (type & (HOOK_VRAM_R | HOOK_VRAM_W |
                                    HOOK_CRAM_R | HOOK_CRAM_W |
                                    HOOK_VSRAM_R | HOOK_VSRAM_W)) != 0;
    // Both CPUs live in one address space as far as a bare number goes, so a
    // breakpoint must name its processor or a Z80 address matches 68000 code.
    const Cpu cpu = (type & (HOOK_Z80_E | HOOK_Z80_R | HOOK_Z80_W)) ? Cpu::Z80 : Cpu::M68K;

    const bool isRead  = (type & (HOOK_M68K_R | HOOK_Z80_R
                                | HOOK_VRAM_R | HOOK_CRAM_R | HOOK_VSRAM_R)) != 0;
    const bool isWrite = (type & (HOOK_M68K_W | HOOK_Z80_W
                                | HOOK_VRAM_W | HOOK_CRAM_W | HOOK_VSRAM_W)) != 0;
    const bool isExec  = (type & (HOOK_M68K_E | HOOK_Z80_E)) != 0;

    // Collect matches under the lock, evaluate conditions outside it: the
    // evaluator calls into the client (IDA), which must not be done while
    // holding a lock the client's own thread may end up waiting on.
    std::vector<std::tuple<int, uint32_t, std::string>> pending;   // id + elang + expr
    {
        std::lock_guard<std::mutex> lk(bpMutex_);
        for (const auto& bp : breakpoints_) {
            if (!bp.enabled) continue;
            if (bp.cpu != cpu) continue;
            if (bp.is_vdp != vdpAccess) continue;
            const bool typeOk = (bp.type == BpType::PC    && isExec)
                             || (bp.type == BpType::Read  && isRead)
                             || (bp.type == BpType::Write && isWrite);
            if (!typeOk) continue;
            // Overlap, not containment: a longword write at FF4276 touches
            // FF4279, so a breakpoint on FF4278 has to see it. Comparing only
            // the start address silently misses every unaligned access.
            const uint32_t last = addr + uint32_t(width > 0 ? width - 1 : 0);
            if (last < bp.start || addr > bp.end) continue;

            if (bp.condition.empty()) return bp.id;         // unconditional: done
            pending.emplace_back(bp.id, bp.elang, bp.condition);
        }
    }

    if (pending.empty()) return -1;
    // Nobody can judge the conditions; err on stopping rather than skipping.
    if (!condEval_) return std::get<0>(pending.front());

    for (const auto& c : pending)
        if (condEval_(std::get<1>(c), std::get<2>(c))) return std::get<0>(c);
    return -1;
}


// ---------------------------------------------------------------------------
// Z80
//
// Deliberately parallel to the 68000 path rather than shared: the two CPUs run
// interleaved on one thread, and mixing their step flags, PCs or callstacks
// means whichever executes more instructions (always the Z80) wins every race.
// lastPcZ80_ is likewise separate — feeding Z80 addresses into the 68000
// codemap would have IDA turn sound-RAM offsets into 68000 code.
// ---------------------------------------------------------------------------
uint8_t GpgxBackend::z80ByteAt(uint16_t pc) const
{
    return (pc < 0x2000) ? zram[pc] : 0;   // table read: never touch IO handlers
}

// Is the condition in a CALL cc,nn / RET cc satisfied right now?
//
// The hook fires before the instruction executes, so the flags still hold the
// values the instruction is about to test. Checking them is the only way to
// know whether the call is actually taken — and it matters: a sound driver
// polling a flag with "call nz,handler" executes that opcode thousands of
// times a second and takes it almost never. Counting them all made the stack
// pile up until it hit the cap, which is exactly what a bogus 256-deep
// callstack of one repeated address looks like.
static bool z80CondTrue(uint8_t op, uint16_t af)
{
    const uint8_t f = uint8_t(af & 0xFF);
    switch ((op >> 3) & 7) {
    case 0: return (f & 0x40) == 0;   // NZ
    case 1: return (f & 0x40) != 0;   // Z
    case 2: return (f & 0x01) == 0;   // NC
    case 3: return (f & 0x01) != 0;   // C
    case 4: return (f & 0x04) == 0;   // PO
    case 5: return (f & 0x04) != 0;   // PE
    case 6: return (f & 0x80) == 0;   // P
    default: return (f & 0x80) != 0;  // M
    }
}

void GpgxBackend::trackZ80Call(uint16_t pc)
{
    const uint8_t op = z80ByteAt(pc);
    bool call = false, ret = false;
    switch (op) {
    case 0xCD:                                          // CALL nn
        call = true; break;
    case 0xDC: case 0xFC: case 0xD4: case 0xC4:         // CALL cc,nn
    case 0xF4: case 0xEC: case 0xE4: case 0xCC:
        call = z80CondTrue(op, Z80.af.w.l); break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:         // RST — always taken
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        call = true; break;
    case 0xC9:                                          // RET
        ret = true; break;
    case 0xD8: case 0xF8: case 0xD0: case 0xC0:         // RET cc
    case 0xF0: case 0xE8: case 0xE0: case 0xC8:
        ret = z80CondTrue(op, Z80.af.w.l); break;
    case 0xED:                                          // reti / retn
        if (z80ByteAt(uint16_t(pc + 1)) == 0x4D || z80ByteAt(uint16_t(pc + 1)) == 0x45)
            ret = true;
        break;
    default: break;
    }
    if (!call && !ret) return;

    std::lock_guard<std::mutex> lk(callstackMutex_);
    if (call) { if (callstackZ80_.size() < 256) callstackZ80_.push_back(pc); }
    else if (!callstackZ80_.empty()) callstackZ80_.pop_back();
}

void GpgxBackend::stepOverZ80()
{
    const uint16_t pc = Z80.pc.w.l;
    const uint8_t  op = z80ByteAt(pc);
    int len = 0;
    switch (op) {
    case 0xCD: case 0xDC: case 0xFC: case 0xD4: case 0xC4:
    case 0xF4: case 0xEC: case 0xE4: case 0xCC:
        len = 3; break;                                   // CALL nn
    case 0xC7: case 0xCF: case 0xD7: case 0xDF:
    case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        len = 1; break;                                   // RST n
    case 0xED:
        if (z80ByteAt(uint16_t(pc + 1)) == 0xB0) len = 2; // LDIR: stepping it is a loop
        break;
    default: break;
    }
    if (len == 0) { stepInto(Cpu::Z80); return; }         // nothing to step over

    stepOverAddrZ80_.store(int(uint16_t(pc + len)));
    paused_.store(false);
    if (resumeCb_) resumeCb_();
}

void GpgxBackend::onZ80Exec(uint32_t pc)
{
    // A HALT re-executes its own address; without this a breakpoint on it would
    // fire again the moment we resume, and never let go.
    const int skip = z80ResumeSkip_.load();
    if (skip >= 0) {
        if (uint32_t(skip) == pc) return;
        z80ResumeSkip_.store(-1);
    }

    lastPcZ80_ = pc;
    trackZ80Call(uint16_t(pc));

    bool brk = stepIntoZ80_.exchange(false);
    if (!brk) {
        const int so = stepOverAddrZ80_.load();
        if (so >= 0 && uint32_t(so) == pc) { stepOverAddrZ80_.store(-1); brk = true; }
    }
    int hit = -1;
    if (!brk) { hit = matchBreakpoint(HOOK_Z80_E, pc); brk = hit >= 0; }
    if (brk) {
        z80ResumeSkip_.store(int(pc));
        firePause(pc, Cpu::Z80, hit);
    }
}

// ---------------------------------------------------------------------------
// Callstack — the same heuristic the original Gens used: push the address of
// each jsr/bsr, pop on rts/rte. Conditional returns are popped unconditionally,
// so it can drift; it is a navigation aid, not ground truth.
// ---------------------------------------------------------------------------
uint16_t GpgxBackend::opcodeAt(uint32_t pc) const
{
    const cpu_memory_map* map = &m68k.memory_map[(pc >> 16) & 0xFF];
    if (!map->base) return 0;
    // storage is word-swapped on LE hosts: logical hi byte lives at addr^1
    return uint16_t((map->base[(pc & 0xFFFF) ^ 1] << 8) | map->base[((pc + 1) & 0xFFFF) ^ 1]);
}

void GpgxBackend::trackCall(uint32_t pc)
{
    const uint16_t opc = opcodeAt(pc);
    const bool isCall   = ((opc & 0xFFC0) == 0x4E80)      // jsr
                       || ((opc & 0xFF00) == 0x6100);     // bsr
    const bool isReturn = (opc == 0x4E75) || (opc == 0x4E73);  // rts / rte
    if (!isCall && !isReturn) return;

    std::lock_guard<std::mutex> lk(callstackMutex_);
    if (isCall) {
        if (callstack_.size() < 256) callstack_.push_back(pc);   // runaway guard
    } else if (!callstack_.empty()) {
        callstack_.pop_back();
    }
}

std::vector<uint32_t> GpgxBackend::getCallstack(Cpu cpu)
{
    std::lock_guard<std::mutex> lk(callstackMutex_);
    return (cpu == Cpu::Z80) ? callstackZ80_ : callstack_;
}
