// GpgxBackend against a synthetic ROM: the contracts the whole debug layer
// rests on, and regressions for the bugs that have actually happened here.

#include "fixture.h"

#include <algorithm>
#include <cstring>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using namespace synth;

// ---------------------------------------------------------------------------
// The byte-order contract (DebugState.h). Everything downstream — every view,
// every IDA read — assumes readMemory hands back LOGICAL order regardless of
// how the core stores it. Assert it against bytes we ourselves put in the ROM.
// ---------------------------------------------------------------------------
TEST(memory_reads_in_logical_byte_order)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const auto got = e.backend()->readMemory(kMarkerAddr, 18);
    REQUIRE(got.size() == 18);
    CHECK_STR(std::string(got.begin(), got.end()), markerText());

    // The header, at an odd-word boundary in the other direction.
    const auto hdr = e.backend()->readMemory(0x100, 16);
    CHECK_STR(std::string(hdr.begin(), hdr.end()), "SEGA MEGA DRIVE ");

    // A single odd address must be the odd byte, not its neighbour: this is
    // exactly what a missing ^1 gets wrong, and it reads plausibly either way.
    const auto one = e.backend()->readMemory(kMarkerAddr + 1, 1);
    REQUIRE(one.size() == 1);
    CHECK_EQ(one[0], uint8_t('M'));
}

TEST(rom_region_matches_flat_bus_read)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const auto flat   = e.backend()->readMemory(kMarkerAddr, 18);
    const auto region = e.backend()->readRegion(0, kMarkerAddr, 18);
    CHECK(flat == region);
}

TEST(work_ram_round_trips_through_both_paths)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const uint8_t pat[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    REQUIRE(e.backend()->writeMemory(0xFF1000, pat, 8));

    const auto viaBus    = e.backend()->readMemory(0xFF1000, 8);
    const auto viaRegion = e.backend()->readRegion(1, 0x1000, 8);
    CHECK(std::equal(pat, pat + 8, viaBus.begin()));
    CHECK(std::equal(pat, pat + 8, viaRegion.begin()));

    // ...and the region write must land where the bus read sees it.
    const uint8_t other[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    REQUIRE(e.backend()->writeRegion(1, 0x2000, other, 4));
    const auto back = e.backend()->readMemory(0xFF2000, 4);
    CHECK(std::equal(other, other + 4, back.begin()));
}

// ---------------------------------------------------------------------------
// Region table. A client indexes these by id, so the ids are a published
// contract; the names are shown to humans.
// ---------------------------------------------------------------------------
TEST(region_table_is_stable_and_bounded)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const auto regions = e.backend()->getMemRegions();
    REQUIRE(regions.size() == 9);

    static const char* kNames[9] = { "ROM", "RAM 68K", "RAM Z80", "VRAM", "CRAM",
                                     "VSRAM", "Regs M68K", "Regs Z80", "Regs VDP" };
    for (int i = 0; i < 9; ++i) {
        CHECK_EQ(regions[i].id, i);
        CHECK_STR(regions[i].name, kNames[i]);
        CHECK(regions[i].size > 0);
    }
    CHECK_EQ(regions[0].size, kRomSize);
    CHECK_EQ(regions[1].size, 0x10000u);      // 64K work RAM
    CHECK_EQ(regions[2].size, 0x2000u);       // 8K sound RAM
    CHECK_EQ(regions[3].size, 0x10000u);      // VRAM

    // Reading past the end must be refused or clamped, never read out of bounds.
    const auto over = e.backend()->readRegion(2, 0x1FF0, 0x100);
    CHECK(over.size() <= 0x10);
}

TEST(cram_packing_round_trips)
{
    // Pure function, but the one whose inversion made every colour wrong.
    for (int r = 0; r < 256; r += 32)
        for (int g = 0; g < 256; g += 32)
            for (int b = 0; b < 256; b += 32) {
                const uint16_t packed = rgb_to_cram(r, g, b);
                int r2, g2, b2;
                cram_to_rgb(packed, r2, g2, b2);
                CHECK_EQ(r2, r); CHECK_EQ(g2, g); CHECK_EQ(b2, b);
                CHECK((packed & ~0x1FFu) == 0);      // 9 bits, BBBGGGRRR
            }
    // Red must be the LOW bits: decoding a packed entry as the 16-bit bus
    // format is what scrambled the channels, and it looks plausible either way.
    CHECK_EQ(rgb_to_cram(0xE0, 0, 0), 0x007u);
    CHECK_EQ(rgb_to_cram(0, 0, 0xE0), 0x1C0u);
}

// ---------------------------------------------------------------------------
// 68000 run control
// ---------------------------------------------------------------------------
TEST(m68k_step_into_follows_the_jsr)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.parkM68kAt(kJsr);
    REQUIRE(e.stepAndWait(Cpu::M68K));
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubEntry);
    CHECK(e.pauseCpu() == Cpu::M68K);
}

TEST(m68k_step_over_skips_the_jsr)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.parkM68kAt(kJsr);
    REQUIRE(e.stepOverAndWait(Cpu::M68K));
    // jsr <abs.l> is six bytes: getting the operand size wrong lands the resume
    // address inside the operand and the step never returns.
    CHECK_EQ(e.backend()->getM68kRegs().pc, kAfterJsr);
}

// The exec hook runs BEFORE the instruction it reports, and the callstack is
// updated there — so by the time execution stops at an address, that address
// has already been accounted for. Two consequences the tests below depend on:
// arriving at the jsr means it is already on the stack, and a PC set by hand
// skips the very hook that would have recorded it (which is why this test
// arrives by breakpoint instead of parking).
TEST(m68k_callstack_pushes_on_jsr_and_pops_on_rts)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = kJsr; bp.end = kJsr;
    e.backend()->addBreakpoint(bp);

    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    REQUIRE(e.pausePc() == kJsr);
    e.backend()->clearBreakpoints();

    auto cs = e.backend()->getCallstack(Cpu::M68K);
    REQUIRE(!cs.empty());
    CHECK_EQ(cs.back(), kJsr);                            // pushed by this hook
    const size_t depth = cs.size();

    REQUIRE(e.stepAndWait(Cpu::M68K));                    // execute the jsr
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubEntry);
    cs = e.backend()->getCallstack(Cpu::M68K);
    REQUIRE(cs.size() == depth);                          // nop changes nothing
    CHECK_EQ(cs.back(), kJsr);

    REQUIRE(e.stepAndWait(Cpu::M68K));                    // execute the nop
    CHECK_EQ(e.backend()->getM68kRegs().pc, kSubRts);
    // Stopping *at* the rts means its hook already popped.
    CHECK_EQ(e.backend()->getCallstack(Cpu::M68K).size(), depth - 1);

    REQUIRE(e.stepAndWait(Cpu::M68K));                    // execute the rts
    CHECK_EQ(e.backend()->getM68kRegs().pc, kAfterJsr);
    CHECK_EQ(e.backend()->getCallstack(Cpu::M68K).size(), depth - 1);
}

TEST(m68k_execution_breakpoint_fires_at_the_address)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = kSubEntry; bp.end = kSubEntry;
    const int id = e.backend()->addBreakpoint(bp);
    CHECK(id >= 0);

    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    CHECK_EQ(e.pausePc(), kSubEntry);
    CHECK(e.pauseCpu() == Cpu::M68K);

    e.backend()->clearBreakpoints();
    CHECK(e.backend()->getBreakpoints().empty());
}

// (moved below armZ80: the Z80 must be parked on its own program first, or its
// power-on NOP sled walks the whole 16-bit space and legitimately reaches any
// address the test picks)

// ---------------------------------------------------------------------------
// Data breakpoints.
//
// These could be set but could never fire: the core emitted no memory hooks at
// all, so nothing ever asked whether an access matched. A missing feature that
// looks present is worse than an absent one — "what wrote this address?" got a
// confident silence.
// ---------------------------------------------------------------------------
namespace {

// Park the 68000 on the data routine and arm one breakpoint. Returns whether
// the machine stopped within the deadline.
bool dataBpFires(t::Emu& e, BpType type, uint32_t start, uint32_t end, int ms = 1500)
{
    e.parkM68kAt(kDataEntry);
    Breakpoint bp;
    bp.type = type; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = start; bp.end = end;
    e.backend()->addBreakpoint(bp);

    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    const bool hit = e.waitPause(s, ms);
    e.backend()->clearBreakpoints();
    if (!hit) { e.backend()->pause(); e.waitPause(s); }
    return hit;
}

} // namespace

TEST(m68k_write_breakpoint_fires)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    CHECK(dataBpFires(e, BpType::Write, kDataAddr, kDataAddr));
    // The write really happened, so this is the instruction we stopped on.
    const auto v = e.backend()->readMemory(kDataAddr, 4);
    REQUIRE(v.size() == 4);
    CHECK_EQ((uint32_t(v[0]) << 24) | (v[1] << 16) | (v[2] << 8) | v[3], kDataValue);
}

// THE WIDTH CASE: a longword write to FF4276 touches FF4276..FF4279, so a
// breakpoint on FF4278 must see it. Comparing only the access's start address
// silently misses every access that straddles the watched byte — and those are
// the majority, because the interesting variables are rarely longword-aligned.
TEST(m68k_write_breakpoint_sees_a_straddling_longword)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    CHECK(dataBpFires(e, BpType::Write, kDataAddr + 2, kDataAddr + 2));
}

TEST(m68k_write_breakpoint_ignores_the_byte_past_the_access)
{
    // The other half of the contract: overlap, not "anywhere nearby".
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    CHECK(!dataBpFires(e, BpType::Write, kDataAddr + 4, kDataAddr + 4, 700));
}

TEST(m68k_read_breakpoint_fires)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    CHECK(dataBpFires(e, BpType::Read, kDataAddr, kDataAddr));
}

TEST(m68k_read_breakpoint_does_not_fire_on_a_write)
{
    // A read breakpoint that also trips on writes is not a read breakpoint.
    // The routine writes before it reads, so a confused implementation stops
    // one instruction early — at kDataEntry rather than kDataRead.
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    REQUIRE(dataBpFires(e, BpType::Read, kDataAddr, kDataAddr));
    CHECK_EQ(e.pausePc(), kDataRead);
}

// ---------------------------------------------------------------------------
// Z80 run control. The ROM's bring-up releases the bus, so the Z80 is really
// executing by the time these run.
// ---------------------------------------------------------------------------
namespace {

// Load the Z80 test program and park the CPU on its first instruction, with
// nothing executed yet. Returns false if the Z80 never came out of reset, which
// would make every assertion below meaningless.
//
// The first step does not advance: the emulator stopped inside the *68000*
// hook, so the Z80's own hook has not yet run for the instruction it is about
// to execute. That first hook is where it stops.
bool armZ80(t::Emu& e)
{
    const auto prog = buildZ80();
    if (!e.backend()->writeRegion(2, 0, prog.data(), uint32_t(prog.size()))) return false;

    const auto back = e.backend()->readZ80Memory(0, uint16_t(prog.size()));
    if (back.size() != prog.size() || !std::equal(prog.begin(), prog.end(), back.begin()))
        return false;

    Z80Regs z = e.backend()->getZ80Regs();
    z.pc = 0; z.sp = 0x1FF0; z.halt = 0;
    e.backend()->setZ80Regs(z);

    // If the bus were still held, this would never complete; it proves the Z80
    // is actually executing.
    if (!e.stepAndWait(Cpu::Z80, 3000)) return false;
    return e.backend()->getZ80Regs().pc == 0;
}

} // namespace

TEST(z80_steps_one_instruction)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(armZ80(e));                                   // parked at $0000
    CHECK(e.pauseCpu() == Cpu::Z80);

    REQUIRE(e.stepAndWait(Cpu::Z80));                     // ld sp,$1FF0
    CHECK_EQ(e.backend()->getZ80Regs().pc, kZ80Xor);
    CHECK_EQ(e.backend()->getZ80Regs().sp, 0x1FF0u);

    REQUIRE(e.stepAndWait(Cpu::Z80));                     // xor a
    CHECK_EQ(e.backend()->getZ80Regs().pc, kZ80CallNotTaken);
    CHECK_EQ(e.backend()->getZ80Regs().af & 0x40, 0x40u); // Z set by xor a
}

TEST(breakpoint_on_the_other_cpu_does_not_fire)
{
    // Both CPUs are plain numbers in one address space, so a breakpoint that
    // forgets to name its processor matches unrelated code.
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(armZ80(e));          // pin the Z80 to $0000-$000A so it cannot
                                 // legitimately reach the 68000 address below

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::Z80; bp.is_vdp = false;
    bp.start = kSubEntry; bp.end = kSubEntry;      // a 68000 address, tagged Z80
    e.backend()->addBreakpoint(bp);

    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    // The 68000 passes kSubEntry many times per frame; if the CPU filter is
    // broken this trips immediately.
    CHECK(!e.waitPause(s, 700));

    e.backend()->clearBreakpoints();
    REQUIRE(e.pauseAndWait());
}

// REGRESSION: an untaken conditional call was counted as a real call, so a
// driver polling a flag with "call nz" filled the stack to its 256 cap with one
// repeated address. The flags are readable at the hook, so this is decidable.
TEST(z80_callstack_ignores_untaken_conditional_call)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(armZ80(e));                                   // parked at $0000

    REQUIRE(e.stepAndWait(Cpu::Z80));                     // ld sp
    REQUIRE(e.backend()->getZ80Regs().pc == kZ80Xor);
    // Measured before the conditional call has been looked at: the hook that
    // decides is the one that stops us at $0004.
    const size_t base = e.backend()->getCallstack(Cpu::Z80).size();

    REQUIRE(e.stepAndWait(Cpu::Z80));                     // xor a -> Z set
    REQUIRE(e.backend()->getZ80Regs().pc == kZ80CallNotTaken);
    // THE REGRESSION: "call nz" with Z set is not taken, so nothing is pushed.
    // Counting it filled the stack to its 256 cap with one repeated address.
    CHECK_EQ(e.backend()->getCallstack(Cpu::Z80).size(), base);

    REQUIRE(e.stepAndWait(Cpu::Z80));                     // the untaken call
    CHECK_EQ(e.backend()->getZ80Regs().pc, kZ80CallTaken);
    auto cs = e.backend()->getCallstack(Cpu::Z80);
    REQUIRE(cs.size() == base + 1);                       // unconditional call
    CHECK_EQ(cs.back(), uint32_t(kZ80CallTaken));

    REQUIRE(e.stepAndWait(Cpu::Z80));                     // the taken call
    CHECK_EQ(e.backend()->getZ80Regs().pc, kZ80Sub);
    // Stopping *at* the ret means its hook already popped.
    CHECK_EQ(e.backend()->getCallstack(Cpu::Z80).size(), base);

    REQUIRE(e.stepAndWait(Cpu::Z80));                     // the ret
    CHECK_EQ(e.backend()->getZ80Regs().pc, kZ80Loop);
    CHECK_EQ(e.backend()->getCallstack(Cpu::Z80).size(), base);
}

TEST(z80_breakpoint_fires_and_reports_z80)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(armZ80(e));

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::Z80; bp.is_vdp = false;
    bp.start = kZ80Loop; bp.end = kZ80Loop;
    e.backend()->addBreakpoint(bp);

    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s, 3000));
    CHECK_EQ(e.pausePc(), kZ80Loop);
    CHECK(e.pauseCpu() == Cpu::Z80);
    e.backend()->clearBreakpoints();
}

// The Z80 half of the same gap. Three layers were silently inert: the core
// emitted no HOOK_Z80_R/W, onCpuHook had no branch for them, and
// matchBreakpoint did not even classify those bits as reads or writes.
TEST(z80_write_breakpoint_fires)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(armZ80(e));

    Z80Regs z = e.backend()->getZ80Regs();
    z.pc = kZ80DataEntry;
    e.backend()->setZ80Regs(z);

    Breakpoint bp;
    bp.type = BpType::Write; bp.cpu = Cpu::Z80; bp.is_vdp = false;
    bp.start = kZ80DataAddr; bp.end = kZ80DataAddr;
    e.backend()->addBreakpoint(bp);

    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s, 3000));
    CHECK(e.pauseCpu() == Cpu::Z80);      // not the 68000's PC

    const auto v = e.backend()->readZ80Memory(kZ80DataAddr, 1);
    REQUIRE(v.size() == 1);
    CHECK_EQ(v[0], kZ80DataValue);
    e.backend()->clearBreakpoints();
}

TEST(z80_read_breakpoint_fires)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(armZ80(e));

    Z80Regs z = e.backend()->getZ80Regs();
    z.pc = kZ80DataRead;                  // straight to the load, past the store
    e.backend()->setZ80Regs(z);

    Breakpoint bp;
    bp.type = BpType::Read; bp.cpu = Cpu::Z80; bp.is_vdp = false;
    bp.start = kZ80DataAddr; bp.end = kZ80DataAddr;
    e.backend()->addBreakpoint(bp);

    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    CHECK(e.waitPause(s, 3000));
    CHECK(e.pauseCpu() == Cpu::Z80);
    e.backend()->clearBreakpoints();
}

TEST(z80_bank_window_reads_the_68000_bus)
{
    // The $8000 window is where sample banks live; without it half the driver's
    // world reads as nothing.
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    Z80Regs z = e.backend()->getZ80Regs();
    CHECK_EQ(z.bank & 0x7FFFu, 0u);           // bank base is 32K-aligned

    // Point the window at the start of ROM and read our own marker through it.
    z.bank = 0;
    e.backend()->setZ80Regs(z);
    const auto through = e.backend()->readZ80Memory(uint16_t(0x8000 + kMarkerAddr), 18);
    REQUIRE(through.size() == 18);
    CHECK_STR(std::string(through.begin(), through.end()), markerText());

    // Hardware ports must never be sampled behind the driver's back.
    const auto ym = e.backend()->readZ80Memory(0x4000, 4);
    REQUIRE(ym.size() == 4);
    CHECK_EQ(ym[0], 0xFFu);
}

TEST(z80_ram_mirrors_through_3fff)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const uint8_t pat[4] = { 0x11, 0x22, 0x33, 0x44 };
    REQUIRE(e.backend()->writeRegion(2, 0x0100, pat, 4));
    const auto mirrored = e.backend()->readZ80Memory(0x2100, 4);
    REQUIRE(mirrored.size() == 4);
    CHECK(std::equal(pat, pat + 4, mirrored.begin()));
}

// ---------------------------------------------------------------------------
// Save states
// ---------------------------------------------------------------------------
TEST(save_state_restores_memory_and_registers)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const uint8_t before[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
    REQUIRE(e.backend()->writeMemory(0xFF3000, before, 4));
    const uint32_t d0 = 0x12345678;
    M68kRegs r = e.backend()->getM68kRegs();
    r.d[0] = d0;
    e.backend()->setM68kRegs(r);

#ifdef _WIN32
    const std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".")
                             + "\\smd_dgx_test.gpz";
#else
    const std::string path = "/tmp/smd_dgx_test.gpz";
#endif
    bool saved = false;
    e.backend()->runSafely([&] { saved = e.backend()->saveState(path.c_str()); });
    REQUIRE(saved);

    const uint8_t after[4] = { 0, 0, 0, 0 };
    e.backend()->writeMemory(0xFF3000, after, 4);
    r.d[0] = 0;
    e.backend()->setM68kRegs(r);

    bool loaded = false;
    e.backend()->runSafely([&] { loaded = e.backend()->loadState(path.c_str()); });
    REQUIRE(loaded);

    const auto back = e.backend()->readMemory(0xFF3000, 4);
    CHECK(std::equal(before, before + 4, back.begin()));
    CHECK_EQ(e.backend()->getM68kRegs().d[0], d0);
    std::remove(path.c_str());
}

// REGRESSION: shutting down while stopped at a breakpoint that re-hits on the
// very next instruction. resume() alone let the emulation thread park itself
// again before it re-checked the stop flag, and the join never returned — the
// app or IDA hung on close with no clue why.
TEST(host_shuts_down_while_parked_on_a_recurring_breakpoint)
{
    auto emu = std::make_unique<t::Emu>();
    REQUIRE(emu->start());
    REQUIRE(emu->pauseAndWait());

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = kSubEntry; bp.end = kSubEntry;      // hit many times per frame
    emu->backend()->addBreakpoint(bp);

    const uint64_t s = emu->pauseSeq();
    emu->backend()->resume();
    REQUIRE(emu->waitPause(s));

    // Tear down on another thread so a regression is a failure, not a hang.
    // Deliberately a detached thread rather than std::async: a future from
    // std::async blocks in its own destructor, which would reintroduce exactly
    // the hang this test exists to detect.
    auto* raw = emu.release();
    std::mutex m;
    std::condition_variable cv;
    bool finished = false;
    std::thread([&] {
        delete raw;
        { std::lock_guard<std::mutex> lk(m); finished = true; }
        cv.notify_all();
    }).detach();

    std::unique_lock<std::mutex> lk(m);
    const bool shutDown = cv.wait_for(lk, std::chrono::seconds(10), [&] { return finished; });
    CHECK(shutDown);
    // On failure the emulation thread is wedged and the fixture is leaked on
    // purpose: the suite reports the failure rather than hanging with it.
}

TEST(pad_round_trips)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    e.backend()->setPad(0, PAD_A | PAD_START);
    CHECK_EQ(e.backend()->getPad(0), uint16_t(PAD_A | PAD_START));
    e.backend()->setPad(0, 0);
    CHECK_EQ(e.backend()->getPad(0), 0u);
}
