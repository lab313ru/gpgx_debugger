// Core options, reset and reload through EmuHost: that settings reach the
// core and stay there, and that the console is only ever rebuilt between
// frames. Assertions read the core's own globals — what the game runs on —
// rather than anything the host merely remembers having been told.

#include "fixture.h"

#include "debugger/CoreOptions.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <thread>

extern "C" {
#include <shared.h>      // config, vdp_pal, region_code, io_reg, m68k, bitmap
}

using namespace synth;
using coreopt::CoreOptions;

namespace {

bool waitFor(const std::function<bool()>& cond, int ms = 5000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!cond()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// Resume, let `frames` frames finish, and pause at the start of the next one.
bool runFrames(t::Emu& e, int frames)
{
    const uint64_t s = e.pauseSeq();
    e.host()->advanceFrames(frames);
    e.backend()->resume();
    return e.waitPause(s);
}

int addPcBreakpoint(t::Emu& e, uint32_t pc)
{
    Breakpoint bp;
    bp.type  = BpType::PC;
    bp.cpu   = Cpu::M68K;
    bp.start = bp.end = pc;
    return e.backend()->addBreakpoint(bp);
}

} // namespace

// ---------------------------------------------------------------------------
// The table: every option round-trips through its own value strings, and a
// value it does not list is refused rather than reaching the core.
// ---------------------------------------------------------------------------
TEST(core_options_table_round_trips)
{
    for (const auto& def : coreopt::options()) {
        CoreOptions o;
        const std::string dflt = coreopt::valueOf(o, def);
        if (dflt.empty())
            t::fail(__FILE__, __LINE__, std::string(def.key) + ": default is not one of its values");
        for (const auto& c : def.choices) {
            CHECK(coreopt::setValue(o, def, c.value));
            CHECK_STR(coreopt::valueOf(o, def), c.value);
        }
        CoreOptions keep;
        CHECK(!coreopt::setValue(keep, def, "no such value"));
        CHECK(keep == CoreOptions{});
    }

    // The defaults are this program's behaviour before options existed.
    const CoreOptions d;
    CHECK_STR(coreopt::valueOf(d, *coreopt::findOption("genesis_plus_gx_audio_filter")), "low-pass");
    CHECK_STR(coreopt::valueOf(d, *coreopt::findOption("genesis_plus_gx_lowpass_range")), "60");
    CHECK_STR(coreopt::valueOf(d, *coreopt::findOption("genesis_plus_gx_psg_preamp")), "150");

    CoreOptions o;
    o.overscan = coreopt::OverscanFull;
    CHECK(coreopt::needs(d, o) == coreopt::Apply::Live);
    o.lockOn = coreopt::LockOnGameGenie;
    CHECK(coreopt::needs(d, o) == coreopt::Apply::Reset);
    o.hardware = coreopt::HardwareMegaDrive;
    CHECK(coreopt::needs(d, o) == coreopt::Apply::Reload);
}

// ---------------------------------------------------------------------------
// gx::init() resets config to defaults inside the load, so options written
// before it are lost; EmuHost must write them after it and before the ROM.
// ---------------------------------------------------------------------------
TEST(core_options_survive_rom_load)
{
    {
        // Control: a value written before the load does not survive it, so
        // the case below is not passing on a config nobody resets.
        config.region_detect = coreopt::RegionEurope;
        t::Emu e;
        REQUIRE(e.start());
        REQUIRE(e.pauseAndWait());
        CHECK_EQ(config.region_detect, 0);
    }

    CoreOptions o;
    o.region       = coreopt::RegionEurope;
    o.lockups      = 0;
    o.addrError    = 0;
    o.fmCore       = coreopt::FmNukedYm3438;
    o.filter       = coreopt::FilterEq;
    o.psgPreamp    = 100;
    o.lowPassRange = 30;
    o.overscan     = coreopt::OverscanFull;
    o.lcdGhosting  = 1;

    t::Emu e;
    CHECK_STR(e.host()->setCoreOptions(o), "");
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    CHECK_EQ(config.region_detect, 2);
    CHECK_EQ(region_code, REGION_EUROPE);
    CHECK_EQ(vdp_pal, 1);
    CHECK_EQ(config.force_dtack, 1);
    CHECK_EQ(m68k.aerr_enabled, 0u);
    CHECK_EQ(config.ym3438, 2);
    CHECK_EQ(config.filter, 2);
    CHECK_EQ(config.psg_preamp, 100);
    CHECK_EQ(config.lp_range, uint32_t(30 * 65536 / 100));
    CHECK_EQ(config.overscan, 3);
    CHECK(config.lcd != 0);
    // Borders made it as far as the VDP's own viewport.
    CHECK_EQ(bitmap.viewport.x, 14);
}

// ---------------------------------------------------------------------------
// A pause stops mid-frame and mid-instruction. A reset asked for there must
// wait: nothing changes while paused, and after resume the paused frame runs
// to its end before the console is rebuilt.
// ---------------------------------------------------------------------------
TEST(reset_while_paused_waits_for_the_frame_boundary)
{
    t::Emu e;
    e.host()->setCoreOptions(CoreOptions{});
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const int atJsr = addPcBreakpoint(e, kJsr);
    uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    REQUIRE(e.pausePc() == kJsr);

    // A hard reset clears work RAM, so this is what a premature one destroys.
    const uint8_t mark[4] = { 0xC0, 0xFF, 0xEE, 0x42 };
    REQUIRE(e.backend()->writeMemory(0xFF1000, mark, 4));
    const uint64_t frame  = e.host()->frameCount();
    const uint64_t resets = e.host()->resetCount();

    REQUIRE(e.host()->reset(true));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    CHECK_EQ(e.host()->resetCount(), resets);
    CHECK(e.backend()->isPaused());
    CHECK_EQ(e.backend()->getM68kRegs().pc, kJsr);
    const auto still = e.backend()->readMemory(0xFF1000, 4);
    CHECK(still.size() == 4 && std::equal(mark, mark + 4, still.begin()));

    // The loop never returns to the reset entry on its own: stopping there
    // means the reset happened, and the frame count says when.
    e.backend()->removeBreakpoint(atJsr);
    addPcBreakpoint(e, kEntry);
    s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));

    CHECK_EQ(e.pausePc(), kEntry);
    CHECK_EQ(e.host()->resetCount(), resets + 1);
    // One more frame than when the reset was asked for: the paused one
    // finished first. A reset inside the pause would stop in that same frame.
    CHECK_EQ(e.host()->frameCount(), frame + 1);
    const auto cleared = e.backend()->readMemory(0xFF1000, 4);
    CHECK(cleared == std::vector<uint8_t>(4, 0));
}

// ---------------------------------------------------------------------------
// The region is LIVE, but a game reads it at boot; what matters is that a
// reset keeps the forced region instead of falling back to the ROM's own.
// ---------------------------------------------------------------------------
TEST(region_change_is_in_the_core_after_reset)
{
    t::Emu e;
    e.host()->setCoreOptions(CoreOptions{});
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(region_code != REGION_EUROPE);
    REQUIRE(vdp_pal == 0);

    CoreOptions o;
    o.region = coreopt::RegionEurope;
    const uint64_t resets = e.host()->resetCount();
    e.host()->setCoreOptions(o);
    REQUIRE(e.host()->reset(true));
    e.backend()->resume();
    REQUIRE(waitFor([&] { return e.host()->resetCount() > resets; }));
    REQUIRE(runFrames(e, 2));

    CHECK_EQ(region_code, REGION_EUROPE);
    CHECK_EQ(vdp_pal, 1);
    CHECK_EQ(io_reg[0] & 0xC0, REGION_EUROPE);     // the version register the game reads
    CHECK_EQ(lines_per_frame, 313);
    CHECK_EQ(status & 1, 1);                       // PAL flag in the VDP status
}

// ---------------------------------------------------------------------------
// A RESET option waits for a hard reset and a RELOAD one for a reload; until
// then the core keeps what it was built with. A soft reset rebuilds nothing.
// ---------------------------------------------------------------------------
TEST(reset_and_reload_classes_wait_for_their_event)
{
    t::Emu e;
    e.host()->setCoreOptions(CoreOptions{});
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    CoreOptions o;
    o.lockOn   = coreopt::LockOnGameGenie;     // RESET
    o.hardware = coreopt::HardwareMegaDrive;   // RELOAD
    e.host()->setCoreOptions(o);
    REQUIRE(runFrames(e, 2));
    CHECK_EQ(config.lock_on, 0);
    CHECK_EQ(config.system, 0);

    // Soft: the 68000 restarts at the entry point, RAM survives.
    const uint8_t mark[2] = { 0x5A, 0xA5 };
    REQUIRE(e.backend()->writeMemory(0xFF1000, mark, 2));
    const int atEntry = addPcBreakpoint(e, kEntry);
    uint64_t r = e.host()->resetCount();
    REQUIRE(e.host()->reset(false));
    uint64_t s = e.pauseSeq();
    e.backend()->resume();
    REQUIRE(e.waitPause(s));
    CHECK_EQ(e.pausePc(), kEntry);
    CHECK_EQ(e.host()->resetCount(), r + 1);
    CHECK(e.backend()->readMemory(0xFF1000, 2) == std::vector<uint8_t>(mark, mark + 2));
    CHECK_EQ(config.lock_on, 0);
    e.backend()->removeBreakpoint(atEntry);

    r = e.host()->resetCount();
    REQUIRE(e.host()->reset(true));
    e.backend()->resume();
    REQUIRE(waitFor([&] { return e.host()->resetCount() > r; }));
    REQUIRE(e.pauseAndWait());
    CHECK_EQ(config.lock_on, TYPE_GG);
    CHECK_EQ(config.system, 0);

    r = e.host()->resetCount();
    REQUIRE(e.host()->reload());
    e.backend()->resume();
    REQUIRE(waitFor([&] { return e.host()->resetCount() > r; }));
    REQUIRE(e.pauseAndWait());
    CHECK_EQ(config.system, SYSTEM_MD);
    CHECK_EQ(config.lock_on, TYPE_GG);
    // The reloaded game runs and stops like the first one did.
    CHECK(e.stepAndWait(Cpu::M68K));
}

// ---------------------------------------------------------------------------
// "System lock-ups" means what the Wii GUI means: on, the console locks up on
// an access real hardware never acknowledges (config.force_dtack 0). libretro
// stores the opposite under the same key, so this is pinned by behaviour.
// ---------------------------------------------------------------------------
namespace {

// 68000 code in work RAM: write to $800000 (no device answers there), then
// a nop the CPU reaches only if the write did not lock it up.
constexpr uint32_t kLockProg  = 0xFF8000;
constexpr uint32_t kAfterLock = 0xFF8008;

void loadLockupProgram(t::Emu& e)
{
    const uint8_t prog[] = {
        0x33, 0xFC, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,   // move.w #0,$800000
        0x4E, 0x71,                                       // nop
        0x60, 0xFE,                                       // bra.s *
    };
    CHECK(e.backend()->writeMemory(kLockProg, prog, sizeof prog));
    e.parkM68kAt(kLockProg);
    addPcBreakpoint(e, kAfterLock);
}

} // namespace

TEST(system_lockups_follow_the_wii_meaning)
{
    const coreopt::OptionDef* def = coreopt::findOption("genesis_plus_gx_force_dtack");
    REQUIRE(def != nullptr);

    {
        CoreOptions o;
        REQUIRE(coreopt::setValue(o, *def, "enabled"));
        t::Emu e;
        e.host()->setCoreOptions(o);
        REQUIRE(e.start());
        REQUIRE(e.pauseAndWait());
        CHECK_EQ(config.force_dtack, 0);

        loadLockupProgram(e);
        const uint64_t s = e.pauseSeq();
        e.backend()->resume();
        CHECK(!e.waitPause(s, 500));                // never reaches the nop
        unsigned halted = 0;
        REQUIRE(e.host()->invoke([&] { halted = m68k.stopped & 2; }));   // STOP_LEVEL_HALT
        CHECK(halted != 0);
    }
    {
        CoreOptions o;
        REQUIRE(coreopt::setValue(o, *def, "disabled"));
        t::Emu e;
        e.host()->setCoreOptions(o);
        REQUIRE(e.start());
        REQUIRE(e.pauseAndWait());
        CHECK_EQ(config.force_dtack, 1);

        loadLockupProgram(e);
        const uint64_t s = e.pauseSeq();
        e.backend()->resume();
        REQUIRE(e.waitPause(s));
        CHECK_EQ(e.pausePc(), kAfterLock);
    }
}

// ---------------------------------------------------------------------------
// Blargg's NTSC filter: the renderer dereferences its tables on every line, so
// switching it on must allocate them. The filtered line is wider than the
// picture, so a pixel past the picture's right edge shows the filter ran.
// ---------------------------------------------------------------------------
namespace {

constexpr int kRow = 100;      // a visible line with no borders
constexpr int kCol = 400;      // past 320, inside both filters' output width
constexpr uint16_t kSentinel = 0xA5A5;

void paintSentinel()
{
    auto* line = reinterpret_cast<uint16_t*>(bitmap.data + (size_t)kRow * bitmap.pitch);
    for (int x = 0; x < bitmap.width; ++x) line[x] = kSentinel;
}

uint16_t pixelPastPicture()
{
    return reinterpret_cast<const uint16_t*>(bitmap.data + (size_t)kRow * bitmap.pitch)[kCol];
}

} // namespace

TEST(ntsc_filter_renders_frames)
{
    CoreOptions o;
    o.ntscFilter = coreopt::NtscComposite;
    t::Emu e;
    e.host()->setCoreOptions(o);
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    CHECK_EQ(config.ntsc, 1);

    // H32 at power-on: the Master System filter's path.
    paintSentinel();
    REQUIRE(runFrames(e, 2));
    CHECK(pixelPastPicture() != kSentinel);

    // Control, switched live: unfiltered lines end at the picture's edge.
    o.ntscFilter = coreopt::NtscOff;
    e.host()->setCoreOptions(o);
    REQUIRE(runFrames(e, 1));
    CHECK_EQ(config.ntsc, 0);
    paintSentinel();
    REQUIRE(runFrames(e, 2));
    CHECK_EQ(pixelPastPicture(), kSentinel);

    // H40: the Mega Drive filter's path.
    e.backend()->setVdpReg(12, 0x81);
    o.ntscFilter = coreopt::NtscRgb;
    e.host()->setCoreOptions(o);
    REQUIRE(runFrames(e, 1));
    CHECK_EQ(config.ntsc, 1);
    CHECK_EQ(reg[12] & 1, 1);
    paintSentinel();
    REQUIRE(runFrames(e, 2));
    CHECK(pixelPastPicture() != kSentinel);
}

// ---------------------------------------------------------------------------
// TMSS without bios_MD.bin still runs — the core emulates the lock itself —
// but the caller has to be told the boot ROM is not there.
// ---------------------------------------------------------------------------
TEST(missing_boot_rom_is_reported)
{
    const coreopt::BootRom found = coreopt::findBootRom();
    if (found.ok()) {
        std::printf("    (skipped: %s exists here)\n", found.path.c_str());
        return;
    }
    CoreOptions o;
    o.bootRom = 1;
    t::Emu e;
    CHECK(!e.host()->setCoreOptions(o).empty());
    CHECK(e.host()->setCoreOptions(CoreOptions{}).empty());
    e.host()->setCoreOptions(o);
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    CHECK_EQ(config.bios & 1, 1);
    CHECK_EQ(system_bios & SYSTEM_MD, 0);
}

// ---------------------------------------------------------------------------
// The IDA plugin never sets options. Its emulator must load exactly as before:
// on the core's defaults, with nothing from a previous host's options.
// ---------------------------------------------------------------------------
TEST(host_without_options_runs_on_core_defaults)
{
    {
        CoreOptions o;
        o.region = coreopt::RegionEurope;
        t::Emu e;
        e.host()->setCoreOptions(o);
        REQUIRE(e.start());
        REQUIRE(e.pauseAndWait());
        REQUIRE(config.region_detect == 2);
    }
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    CHECK_EQ(config.region_detect, 0);
    CHECK_EQ(config.filter, 1);
    CHECK_EQ(config.force_dtack, 0);

    // A reset does not bring them in either.
    const uint64_t r = e.host()->resetCount();
    REQUIRE(e.host()->reset(true));
    e.backend()->resume();
    REQUIRE(waitFor([&] { return e.host()->resetCount() > r; }));
    REQUIRE(e.pauseAndWait());
    CHECK_EQ(config.region_detect, 0);
    CHECK_EQ(region_code, REGION_USA);
}
