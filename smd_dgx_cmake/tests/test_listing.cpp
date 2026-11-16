// listAround and listAroundZ80: the lines a disassembly window shows, on the
// synthetic ROM and on Z80 code the tests write into sound RAM.
//
// Two things here matter more than the text, which test_disasm already pins:
// that the lines before an address are whole instructions of the real stream,
// not the tail of one read from the middle; and that building a listing reads
// nothing but ROM and RAM, because on this bus a read of anything else
// is an access the game sees.

#include "fixture.h"

#include "debugger/Disasm68k.h"
#include "debugger/DisasmListing.h"
#include "debugger/DisasmZ80.h"

#include <optional>
#include <vector>

using namespace synth;
using disasm::ListingLine;
using disasm::listAround;
using disasm::listAroundZ80;

namespace {

// Forwards to the real backend and records every read of memory, so a test
// can show which reads a listing makes instead of inferring it from the text.
class ReadSpy : public IDebugBackend {
public:
    struct RegionRead { int id; uint32_t off; uint32_t size; };

    explicit ReadSpy(IDebugBackend* be) : be_(be) {}

    int busReads = 0;
    std::vector<RegionRead> regionReads;

    std::vector<uint8_t> readMemory(uint32_t a, uint32_t n) override { ++busReads; return be_->readMemory(a, n); }
    std::vector<uint8_t> readZ80Memory(uint16_t a, uint16_t n) override { ++busReads; return be_->readZ80Memory(a, n); }
    std::vector<uint8_t> readRegion(int id, uint32_t off, uint32_t n) override
    {
        regionReads.push_back({ id, off, n });
        return be_->readRegion(id, off, n);
    }

    M68kRegs   getM68kRegs() override { return be_->getM68kRegs(); }
    void       setM68kRegs(const M68kRegs& r) override { be_->setM68kRegs(r); }
    Z80Regs    getZ80Regs() override { return be_->getZ80Regs(); }
    void       setZ80Regs(const Z80Regs& r) override { be_->setZ80Regs(r); }
    VdpState   getVdpState() override { return be_->getVdpState(); }
    void       setVdpReg(int i, uint8_t v) override { be_->setVdpReg(i, v); }
    SoundState getSoundState() override { return be_->getSoundState(); }
    bool writeMemory(uint32_t a, const uint8_t* d, uint32_t n) override { return be_->writeMemory(a, d, n); }
    std::vector<MemRegion> getMemRegions() override { return be_->getMemRegions(); }
    bool writeRegion(int id, uint32_t off, const uint8_t* d, uint32_t n) override { return be_->writeRegion(id, off, d, n); }
    bool saveState(const char* p) override { return be_->saveState(p); }
    bool loadState(const char* p) override { return be_->loadState(p); }
    bool runSafely(const std::function<void()>& fn) override { return be_->runSafely(fn); }
    void     setPad(int port, uint16_t b) override { be_->setPad(port, b); }
    uint16_t getPad(int port) override { return be_->getPad(port); }
    std::vector<uint32_t> getCallstack(Cpu cpu) override { return be_->getCallstack(cpu); }
    void setConditionEvaluator(ConditionEval f) override { be_->setConditionEvaluator(std::move(f)); }
    int  addBreakpoint(const Breakpoint& bp) override { return be_->addBreakpoint(bp); }
    void removeBreakpoint(int id) override { be_->removeBreakpoint(id); }
    bool setBreakpointEnabled(int id, bool on) override { return be_->setBreakpointEnabled(id, on); }
    void clearBreakpoints() override { be_->clearBreakpoints(); }
    std::vector<Breakpoint> getBreakpoints() override { return be_->getBreakpoints(); }
    void pause() override { be_->pause(); }
    void resume() override { be_->resume(); }
    void stepInto(Cpu cpu) override { be_->stepInto(cpu); }
    void stepOver(Cpu cpu) override { be_->stepOver(cpu); }
    void stepOut(Cpu cpu) override { be_->stepOut(cpu); }
    bool isPaused() const override { return be_->isPaused(); }
    void onPaused(PauseCb cb) override { be_->onPaused(std::move(cb)); }
    void onResumed(ResumeCb cb) override { be_->onResumed(std::move(cb)); }
    bool loadRom(const char* p) override { return be_->loadRom(p); }
    bool isRunning() const override { return be_->isRunning(); }

private:
    IDebugBackend* be_;
};

const ListingLine* lineAt(const std::vector<ListingLine>& v, uint32_t addr)
{
    for (const auto& l : v)
        if (l.addr == addr) return &l;
    return nullptr;
}

// Every line starts where the one before it ended: no gap, no overlap.
bool contiguous(const std::vector<ListingLine>& v)
{
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i].addr != v[i - 1].addr + v[i - 1].len) return false;
    return true;
}

size_t indexOf(const std::vector<ListingLine>& v, uint32_t addr)
{
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].addr == addr) return i;
    return v.size();
}

} // namespace

// The return address of the jsr is 6 bytes past it. Reading back from there
// one word at a time lands inside the jsr's operand ("ori.b #$a1, D0" and the
// like decode just as well), so this is the case resync exists for.
TEST(listing_before_the_return_address_is_the_whole_jsr)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    for (int before : { 2, 8 }) {
        const auto v = listAround(*e.backend(), kAfterJsr, before, 4);
        REQUIRE(v.size() == size_t(before) + 1 + 4);
        CHECK(contiguous(v));
        REQUIRE(indexOf(v, kAfterJsr) == size_t(before));

        const ListingLine& jsr = v[before - 1];
        CHECK_EQ(jsr.addr, kJsr);
        CHECK_EQ(jsr.len, 6u);
        CHECK_STR(jsr.text, "jsr     $300.l");
        CHECK_STR(jsr.hex, "4EB9 0000 0300");
        CHECK(jsr.readable);
        CHECK(jsr.target == std::optional<uint32_t>(kSubEntry));

        // And the line before it is the bus write that really precedes it.
        CHECK_EQ(v[before - 2].addr, kEntry + 0x10);
        CHECK_EQ(v[before - 2].len, 8u);
        CHECK(!v[before - 2].target);

        CHECK(lineAt(v, kJsr + 2) == nullptr);
        CHECK(lineAt(v, kJsr + 4) == nullptr);

        const ListingLine& bra = v[before + 1];
        CHECK_EQ(bra.addr, kAfterJsr + 2);
        CHECK_STR(bra.text, "bra     $218");
        CHECK(bra.target == std::optional<uint32_t>(kJsr));
    }

    // Nothing comes before address 0, and the lines after it are not padded
    // out to make up the count.
    const auto start = listAround(*e.backend(), 0, 8, 4);
    REQUIRE(start.size() == 5u);
    CHECK_EQ(start.front().addr, 0u);
    CHECK(contiguous(start));
}

// From the subroutine back to the jsr is $E8 bytes: the jsr only shows up as
// a whole instruction if resync reaches back as far as the lines asked for,
// through the $FFFF fill between the routines.
TEST(listing_flags_the_pc_and_a_breakpoint_and_resyncs_far_back)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    e.parkM68kAt(kSubEntry);

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = kSubEntry; bp.end = kSubEntry;
    REQUIRE(e.backend()->addBreakpoint(bp) >= 0);

    // On the next instruction, every breakpoint that must not count as one.
    auto off = bp;
    off.start = off.end = kSubRts;
    off.enabled = false;
    e.backend()->addBreakpoint(off);
    auto read = bp;
    read.start = read.end = kSubRts;
    read.type = BpType::Read;
    e.backend()->addBreakpoint(read);
    auto z80 = bp;
    z80.start = z80.end = kSubRts;
    z80.cpu = Cpu::Z80;
    e.backend()->addBreakpoint(z80);
    auto vdp = bp;
    vdp.start = vdp.end = kSubRts;
    vdp.is_vdp = true;
    e.backend()->addBreakpoint(vdp);

    // A range covers every line that starts inside it.
    auto range = bp;
    range.start = kAfterJsr;
    range.end   = kAfterJsr + 3;
    e.backend()->addBreakpoint(range);

    const int before = 120;
    const auto v = listAround(*e.backend(), kSubEntry, before, 3);
    REQUIRE(v.size() == size_t(before) + 1 + 3);
    CHECK(contiguous(v));
    REQUIRE(indexOf(v, kSubEntry) == size_t(before));

    const ListingLine& entry = v[before];
    CHECK_STR(entry.text, "nop");
    CHECK(entry.isPc);
    CHECK(entry.hasBreakpoint);

    const ListingLine* rts = lineAt(v, kSubRts);
    REQUIRE(rts != nullptr);
    CHECK_STR(rts->text, "rts");
    CHECK(!rts->isPc);
    CHECK(!rts->hasBreakpoint);

    int pcLines = 0, bpLines = 0;
    for (const auto& l : v) {
        pcLines += l.isPc;
        bpLines += l.hasBreakpoint;
    }
    CHECK_EQ(pcLines, 1);
    CHECK_EQ(bpLines, 3);   // kSubEntry, and the nop and bra inside the range
    REQUIRE(lineAt(v, kAfterJsr) != nullptr);
    CHECK(lineAt(v, kAfterJsr)->hasBreakpoint);
    REQUIRE(lineAt(v, kAfterJsr + 2) != nullptr);
    CHECK(lineAt(v, kAfterJsr + 2)->hasBreakpoint);

    const ListingLine* jsr = lineAt(v, kJsr);
    REQUIRE(jsr != nullptr);
    CHECK_EQ(jsr->len, 6u);
    CHECK_STR(jsr->text, "jsr     $300.l");
    CHECK(!jsr->hasBreakpoint);
    CHECK(lineAt(v, kJsr + 2) == nullptr);
    CHECK(lineAt(v, kJsr + 4) == nullptr);

    const ListingLine* store = lineAt(v, kDataEntry);
    REQUIRE(store != nullptr);
    CHECK_EQ(store->len, 10u);
    CHECK_STR(store->hex, "23FC 1234 5678 00FF 4276");

    e.backend()->clearBreakpoints();
}

// The backend stops on an execution breakpoint that touches the opcode word,
// so one on the odd byte of it marks the line, and only that line.
TEST(listing_marks_a_breakpoint_on_the_odd_byte_of_an_opcode)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = bp.end = kSubEntry + 1;
    REQUIRE(e.backend()->addBreakpoint(bp) >= 0);

    const auto v = listAround(*e.backend(), kSubEntry, 4, 4);
    const ListingLine* nop = lineAt(v, kSubEntry);
    const ListingLine* rts = lineAt(v, kSubRts);
    REQUIRE(nop != nullptr);
    REQUIRE(rts != nullptr);
    CHECK(nop->hasBreakpoint);
    CHECK(!rts->hasBreakpoint);

    e.backend()->clearBreakpoints();
}

// An I/O address is never read, not even to find out it is not code: the
// VDP control port drops a half-written command when read, and unmapped
// space halts the 68000.
TEST(listing_of_an_io_address_reads_nothing_there)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    ReadSpy spy(e.backend());

    for (uint32_t centre : { 0xA10000u, 0xA10002u, 0xC00000u, 0xC00004u, 0x800000u }) {
        spy.regionReads.clear();
        const auto v = listAround(spy, centre, 8, 8);
        REQUIRE(v.size() == 17u);
        CHECK(contiguous(v));
        CHECK_EQ(v[8].addr, centre);
        int decoded = 0;
        for (const auto& l : v) {
            decoded += l.readable;
            CHECK_EQ(l.len, 2u);
            CHECK_STR(l.text, disasm::kNotReadable);
            CHECK(l.hex.empty());
            CHECK(!l.target);
        }
        CHECK_EQ(decoded, 0);
        CHECK(spy.regionReads.empty());
    }
    CHECK_EQ(spy.busReads, 0);

    // A ROM listing reads ROM and nothing else.
    spy.regionReads.clear();
    const auto rom = listAround(spy, kJsr, 4, 4);
    CHECK(!rom.empty());
    CHECK(!spy.regionReads.empty());
    for (const auto& r : spy.regionReads) {
        CHECK_EQ(r.id, 0);
        CHECK(r.off + r.size <= kRomSize);
    }
    CHECK_EQ(spy.busReads, 0);
}

// Past the end of the ROM image is not ROM: an instruction whose operands
// would come from there is a data word, and what follows is not read.
TEST(listing_stops_decoding_at_the_end_of_rom)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const uint8_t jsrAbsL[2] = { 0x4E, 0xB9 };
    REQUIRE(e.backend()->writeRegion(0, kRomSize - 2, jsrAbsL, 2));

    ReadSpy spy(e.backend());
    const auto v = listAround(spy, kRomSize - 2, 2, 3);
    REQUIRE(v.size() == 6u);
    CHECK(contiguous(v));

    const ListingLine& last = v[2];
    CHECK_EQ(last.addr, kRomSize - 2);
    CHECK(last.readable);
    CHECK_EQ(last.len, 2u);
    CHECK_STR(last.text, "dc.w $4eb9");
    CHECK_STR(last.hex, "4EB9");
    CHECK(!last.target);

    for (size_t i = 3; i < v.size(); ++i) {
        CHECK(!v[i].readable);
        CHECK_STR(v[i].text, disasm::kNotReadable);
    }
    for (const auto& r : spy.regionReads) {
        CHECK_EQ(r.id, 0);
        CHECK(r.off + r.size <= kRomSize);
    }
    CHECK_EQ(spy.busReads, 0);
}

// Work RAM lists at $FF0000 and at its mirrors, each under its own address:
// a PC-relative target follows the address shown, an absolute one does not.
TEST(listing_reads_work_ram_and_its_mirrors)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    const uint8_t code[] = {
        0x4E, 0x71,                               // 0100 nop
        0x4E, 0xB9, 0x00, 0xFF, 0x01, 0x00,       // 0102 jsr $ff0100.l
        0x60, 0xF6,                               // 0108 bra.s 0100
    };
    REQUIRE(e.backend()->writeRegion(1, 0x100, code, sizeof code));

    // Known code at both ends of RAM, for the lines that resync finds there.
    std::vector<uint8_t> sled;
    for (int i = 0; i < 0x20; ++i) { sled.push_back(0x4E); sled.push_back(0x71); }
    REQUIRE(e.backend()->writeRegion(1, 0x10000 - 0x40, sled.data(), 0x40));

    ReadSpy spy(e.backend());
    for (uint32_t base : { 0xFF0000u, 0xE00000u, 0xFE0000u }) {
        const auto v = listAround(spy, base + 0x100, 0, 2);
        REQUIRE(v.size() == 3u);
        CHECK(contiguous(v));
        CHECK_EQ(v[0].addr, base + 0x100);
        CHECK_STR(v[0].text, "nop");
        CHECK_STR(v[1].text, "jsr     $ff0100.l");
        CHECK_STR(v[1].hex, "4EB9 00FF 0100");
        CHECK(v[1].target == std::optional<uint32_t>(0xFF0100u));
        CHECK_EQ(v[2].addr, base + 0x108);
        CHECK(v[2].target == std::optional<uint32_t>(base + 0x100));
    }

    // The listing ends with the bus instead of wrapping round to address 0,
    // and the lines before $FF0000 come from the mirror below it, which holds
    // the top of RAM.
    const auto top = listAround(spy, 0xFFFFFE, 2, 4);
    REQUIRE(top.size() == 3u);
    CHECK(contiguous(top));
    CHECK_EQ(top.front().addr, 0xFFFFFAu);
    CHECK_EQ(top.back().addr, 0xFFFFFEu);
    CHECK_STR(top.back().text, "nop");

    const auto bottom = listAround(spy, 0xFF0000, 2, 0);
    REQUIRE(bottom.size() == 3u);
    CHECK(contiguous(bottom));
    CHECK_EQ(bottom.front().addr, 0xFEFFFCu);
    CHECK_STR(bottom.front().text, "nop");
    CHECK(bottom.front().readable);

    // Each read stays inside the 64K of RAM, the one across $FF0000 included.
    for (const auto& r : spy.regionReads) {
        CHECK_EQ(r.id, 1);
        CHECK(r.off + r.size <= 0x10000u);
    }
    CHECK_EQ(spy.busReads, 0);
}

// A target comes from the operand that names it, with the CPU's own
// arithmetic: .w sign-extends, and everything is 24 bits.
TEST(listing_flow_targets_from_disasm_text)
{
    struct Case { std::vector<uint8_t> bytes; uint32_t addr; std::optional<uint32_t> target; };
    const Case cases[] = {
        { { 0x60, 0xFE },                         0x001000, 0x001000 },   // bra.s *
        { { 0x61, 0x04 },                         0x001000, 0x001006 },   // bsr.s
        { { 0x61, 0x00, 0x00, 0x10 },             0x001000, 0x001012 },   // bsr.w
        { { 0x67, 0x06 },                         0x001000, 0x001008 },   // beq.s
        { { 0x66, 0x00, 0xFF, 0xFE },             0x001000, 0x001000 },   // bne.w
        { { 0x6E, 0xFE },                         0xFF8000, 0xFF8000 },   // bgt.s in RAM
        { { 0x51, 0xC8, 0xFF, 0xFE },             0x001000, 0x001000 },   // dbra d0
        { { 0x56, 0xC9, 0x00, 0x04 },             0x001000, 0x001006 },   // dbne d1
        { { 0x50, 0xCA, 0x00, 0x04 },             0x001000, 0x001006 },   // dbt d2
        { { 0x60, 0x02 },                         0xFFFFFE, 0x000002 },   // past the top of the bus
        { { 0x60, 0xFA },                         0x000000, 0xFFFFFC },   // below 0
        { { 0x4E, 0xB8, 0x0F, 0x54 },             0x001000, 0x000F54 },   // jsr $f54.w
        { { 0x4E, 0xB8, 0x80, 0x00 },             0x001000, 0xFF8000 },   // jsr $8000.w: sign-extended
        { { 0x4E, 0xF9, 0x00, 0x00, 0x03, 0x00 }, 0x001000, 0x000300 },   // jmp $300.l
        { { 0x4E, 0xF9, 0xFF, 0x00, 0x03, 0x00 }, 0x001000, 0x000300 },   // top byte is not on the bus
        { { 0x4E, 0xBA, 0x00, 0x10 },             0x001000, 0x001012 },   // jsr (d16,PC)
        { { 0x4E, 0xFA, 0xFF, 0xFE },             0x001000, 0x001000 },   // jmp (-2,PC)
        { { 0x4E, 0xD0 },                         0x001000, std::nullopt }, // jmp (A0)
        { { 0x4E, 0xA8, 0x00, 0x10 },             0x001000, std::nullopt }, // jsr ($10,A0)
        { { 0x4E, 0xFB, 0x00, 0x04 },             0x001000, std::nullopt }, // jmp (4,PC,D0.w)
        { { 0x41, 0xFA, 0x00, 0x10 },             0x001000, std::nullopt }, // lea (d16,PC): not a jump
        { { 0x48, 0x78, 0x0F, 0x54 },             0x001000, std::nullopt }, // pea $f54.w
        { { 0x33, 0xFC, 0x12, 0x34, 0x00, 0xC0, 0x00, 0x04 }, 0x001000, std::nullopt },
        { { 0x08, 0xF8, 0x00, 0x01, 0x10, 0x00 }, 0x001000, std::nullopt }, // bset #1,$1000.w
        { { 0x4E, 0x75 },                         0x001000, std::nullopt }, // rts
        { { 0x4E, 0x40 },                         0x001000, std::nullopt }, // trap #0
        { { 0xF0, 0x00 },                         0x001000, std::nullopt }, // dc.w
    };
    for (const auto& c : cases) {
        const auto line = disasm::disasm68k(c.bytes.data(), c.bytes.size(), c.addr);
        const auto got = disasm::flowTarget(line.text);
        if (got != c.target)
            t::fail(__FILE__, __LINE__,
                    t::hexBytes(c.bytes) + " at " + t::hex(c.addr) + " \"" + line.text + "\": got "
                        + (got ? t::hex(*got) : std::string("none")) + ", want "
                        + (c.target ? t::hex(*c.target) : std::string("none")));
    }
}

// ---------------------------------------------------------------------------
// listAroundZ80
// ---------------------------------------------------------------------------
namespace {

// A Z80 program for the listing, written into sound RAM:
//
//   0100  3E 12        ld a,$12
//   0102  DD 21 00 10  ld ix,$1000
//   0106  DD CB 05 06  rlc (ix+$05)
//   010A  CD 20 01     call $0120
//   010D  18 F1        jr $0100
//   0120  C9           ret
//
// Sound RAM is zero at reset, so nops lead up to it. Two bytes back from the
// return address, $010D, is the call's operand: "20 01" is jr nz and lands
// on $010D just as well, which is the case resync exists for.
constexpr uint16_t kListProg   = 0x0100;
constexpr uint16_t kListIx     = 0x0102;
constexpr uint16_t kListRlc    = 0x0106;
constexpr uint16_t kListCall   = 0x010A;
constexpr uint16_t kListJr     = 0x010D;
constexpr uint16_t kListCallee = 0x0120;

bool putZ80Listing(t::Emu& e)
{
    const uint8_t prog[] = {
        0x3E, 0x12,
        0xDD, 0x21, 0x00, 0x10,
        0xDD, 0xCB, 0x05, 0x06,
        0xCD, 0x20, 0x01,
        0x18, 0xF1,
    };
    const uint8_t ret = 0xC9;
    return e.backend()->writeRegion(2, kListProg, prog, sizeof prog)
        && e.backend()->writeRegion(2, kListCallee, &ret, 1);
}

// Shifts `bank` into the bank register the way a sound driver does: nine
// writes to $6000, bit 15 of the 68000 address first. Runs the Z80 to the
// end of it and leaves the machine paused there; false if it never got there.
constexpr uint16_t kBankProg = 0x0200;

bool setZ80Bank(t::Emu& e, uint32_t bank)
{
    std::vector<uint8_t> prog = { 0x21, 0x00, 0x60 };   // ld hl,$6000
    for (int bit = 15; bit <= 23; ++bit) {
        prog.push_back(0x36);                          // ld (hl),n
        prog.push_back(uint8_t((bank >> bit) & 1));
    }
    const uint16_t done = uint16_t(kBankProg + prog.size());
    prog.push_back(0x18);                              // jr $
    prog.push_back(0xFE);
    if (!e.backend()->writeRegion(2, kBankProg, prog.data(), uint32_t(prog.size()))) return false;

    Z80Regs z = e.backend()->getZ80Regs();
    z.pc = kBankProg;
    e.backend()->setZ80Regs(z);

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::Z80; bp.is_vdp = false;
    bp.start = bp.end = done;
    const int id = e.backend()->addBreakpoint(bp);

    const uint64_t s = e.pauseSeq();
    e.backend()->resume();
    const bool stopped = e.waitPause(s, 3000);
    e.backend()->removeBreakpoint(id);
    return stopped && e.pauseCpu() == Cpu::Z80 && e.backend()->getZ80Regs().pc == done;
}

// Every read a Z80 listing made went to sound RAM, inside its 8K.
bool onlySoundRam(const ReadSpy& spy)
{
    for (const auto& r : spy.regionReads)
        if (r.id != 2 || r.off + r.size > 0x2000u) return false;
    return true;
}

} // namespace

TEST(listing_z80_before_the_return_address_is_the_whole_call)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(putZ80Listing(e));

    Z80Regs z = e.backend()->getZ80Regs();
    z.pc = kListRlc;
    e.backend()->setZ80Regs(z);

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::Z80; bp.is_vdp = false;
    bp.start = bp.end = kListCall;
    REQUIRE(e.backend()->addBreakpoint(bp) >= 0);

    // On the rlc, every breakpoint that must not count as one: the 68000's
    // at the same number, a disabled one, a read and a VDP one.
    auto m68k = bp;
    m68k.start = m68k.end = kListRlc;
    m68k.cpu = Cpu::M68K;
    auto off = bp;
    off.start = off.end = kListRlc;
    off.enabled = false;
    auto read = bp;
    read.start = read.end = kListRlc;
    read.type = BpType::Read;
    auto vdp = bp;
    vdp.start = vdp.end = kListRlc;
    vdp.is_vdp = true;
    for (const Breakpoint& b : { m68k, off, read, vdp })
        REQUIRE(e.backend()->addBreakpoint(b) >= 0);

    ReadSpy spy(e.backend());
    for (int before : { 4, 12 }) {
        const auto v = listAroundZ80(spy, kListJr, before, 2);
        REQUIRE(v.size() == size_t(before) + 1 + 2);
        CHECK(contiguous(v));
        REQUIRE(indexOf(v, kListJr) == size_t(before));

        const ListingLine& call = v[before - 1];
        CHECK_EQ(call.addr, kListCall);
        CHECK_EQ(call.len, 3u);
        CHECK_STR(call.text, "call    $0120");
        CHECK_STR(call.hex, "CD 20 01");
        CHECK(call.readable);
        CHECK(call.target == std::optional<uint32_t>(kListCallee));
        CHECK(call.hasBreakpoint);
        CHECK(!call.isPc);

        const ListingLine& rlc = v[before - 2];
        CHECK_EQ(rlc.addr, kListRlc);
        CHECK_EQ(rlc.len, 4u);
        CHECK_STR(rlc.text, "rlc     (ix+$05)");
        CHECK_STR(rlc.hex, "DD CB 05 06");
        CHECK(!rlc.target);
        CHECK(rlc.isPc);
        CHECK(!rlc.hasBreakpoint);

        CHECK_EQ(v[before - 3].addr, kListIx);
        CHECK_STR(v[before - 3].text, "ld      ix,$1000");
        CHECK_EQ(v[before - 4].addr, kListProg);
        CHECK_STR(v[before - 4].text, "ld      a,$12");
        CHECK_STR(v[before - 4].hex, "3E 12");
        for (int i = 0; i < before - 4; ++i) {
            CHECK_EQ(v[i].addr, uint32_t(kListProg - (before - 4) + i));
            CHECK_STR(v[i].text, "nop");
        }

        // Nothing starts inside an instruction: not in the rlc's operand, and
        // not at the "jr nz" in the call's.
        CHECK(lineAt(v, kListRlc + 1) == nullptr);
        CHECK(lineAt(v, kListCall + 1) == nullptr);
        CHECK(lineAt(v, kListCall + 2) == nullptr);

        const ListingLine& jr = v[before];
        CHECK_STR(jr.text, "jr      $0100");
        CHECK_STR(jr.hex, "18 F1");
        CHECK(jr.target == std::optional<uint32_t>(kListProg));
        CHECK_STR(v[before + 1].text, "nop");

        int pcLines = 0, bpLines = 0;
        for (const auto& l : v) {
            pcLines += l.isPc;
            bpLines += l.hasBreakpoint;
        }
        CHECK_EQ(pcLines, 1);
        CHECK_EQ(bpLines, 1);
    }

    // Nothing comes before address 0, and the lines after it are not padded
    // out to make up the count.
    const auto start = listAroundZ80(spy, 0, 8, 4);
    REQUIRE(start.size() == 5u);
    CHECK_EQ(start.front().addr, 0u);
    CHECK(contiguous(start));

    CHECK(onlySoundRam(spy));
    CHECK_EQ(spy.busReads, 0);
    e.backend()->clearBreakpoints();
}

// Centre on $0101, the operand of "ld a,$12": every stream before it runs
// into $0100 and steps over $0101, so none lands. The nops still lead up to
// it, and the byte at $0100 is a one-byte line, so scrolling up from the
// first line (which re-centres on it) always moves.
TEST(listing_z80_lines_lead_to_a_centre_no_stream_lands_on)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(putZ80Listing(e));

    ReadSpy spy(e.backend());
    const auto v = listAroundZ80(spy, kListProg + 1, 4, 1);
    REQUIRE(v.size() == 6u);
    CHECK(contiguous(v));
    REQUIRE(indexOf(v, kListProg + 1) == 4u);
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(v[i].addr, uint32_t(kListProg - 3 + i));
        CHECK_STR(v[i].text, "nop");
    }
    CHECK_EQ(v[3].addr, uint32_t(kListProg));
    CHECK_EQ(v[3].len, 1u);
    CHECK_STR(v[3].text, "db      $3e");
    CHECK_STR(v[3].hex, "3E");
    CHECK(v[3].readable);
    CHECK_STR(v[4].text, "ld      (de),a");

    // What ListingPane::scrollBy(-n) does: centre on the first line, again
    // and again. Each pass must reach further back, down to $0000.
    uint32_t centre = kListProg + 1;
    for (int pass = 0; pass < 200 && centre > 0; ++pass) {
        const auto up = listAroundZ80(spy, uint16_t(centre), 3, 3);
        REQUIRE(!up.empty());
        REQUIRE(up.front().addr < centre);
        centre = up.front().addr;
    }
    CHECK_EQ(centre, 0u);
    CHECK(onlySoundRam(spy));
}

// $2000-$3FFF is the same 8K again, listed under its own address. The YM2612,
// the bank register, the VDP and the PSG behind $4000-$7FFF are never read.
TEST(listing_z80_reads_sound_ram_and_its_mirror_and_never_the_ports)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    REQUIRE(putZ80Listing(e));
    ReadSpy spy(e.backend());

    // A relative target follows the address shown; an absolute one does not.
    const auto m = listAroundZ80(spy, 0x2000 + kListCall, 1, 1);
    REQUIRE(m.size() == 3u);
    CHECK(contiguous(m));
    CHECK_EQ(m[0].addr, 0x2000u + kListRlc);
    CHECK_STR(m[0].text, "rlc     (ix+$05)");
    CHECK(m[0].readable);
    CHECK_EQ(m[1].addr, 0x2000u + kListCall);
    CHECK_STR(m[1].text, "call    $0120");
    CHECK(m[1].target == std::optional<uint32_t>(kListCallee));
    CHECK_STR(m[2].text, "jr      $2100");
    CHECK(m[2].target == std::optional<uint32_t>(0x2000u + kListProg));
    CHECK(onlySoundRam(spy));

    for (uint16_t centre : { 0x4020, 0x4100, 0x5FFF, 0x6000, 0x7F00, 0x7F11 }) {
        spy.regionReads.clear();
        const auto v = listAroundZ80(spy, centre, 4, 8);
        REQUIRE(v.size() == 13u);
        CHECK(contiguous(v));
        CHECK_EQ(v[4].addr, uint32_t(centre));
        for (const auto& l : v) {
            CHECK(!l.readable);
            CHECK_EQ(l.len, 1u);
            CHECK_STR(l.text, disasm::kNotReadable);
            CHECK(l.hex.empty());
            CHECK(!l.target);
        }
        CHECK(spy.regionReads.empty());
    }

    // Across $4000: RAM up to it, nothing from it on.
    spy.regionReads.clear();
    const auto edge = listAroundZ80(spy, 0x4000, 4, 4);
    REQUIRE(edge.size() == 9u);
    CHECK(contiguous(edge));
    for (size_t i = 0; i < edge.size(); ++i) {
        CHECK_EQ(edge[i].readable, i < 4);
        CHECK_STR(edge[i].text, i < 4 ? "nop" : disasm::kNotReadable);
    }
    CHECK(onlySoundRam(spy));
    CHECK_EQ(spy.busReads, 0);
}

// The window shows ROM where the core hands the Z80 ROM bytes, and nothing
// anywhere else: not I/O, not the VDP, not the 68000's work RAM, which the Z80
// can write but not read, and not the space past the end of the ROM.
TEST(listing_z80_bank_window_reads_rom_and_nothing_else)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());
    ReadSpy spy(e.backend());

    const uint16_t centre = uint16_t(0x8000 + kEntry);
    for (uint32_t bank : { 0xA10000u, 0xC00000u, 0xFF0000u, 0x400000u, 0x008000u }) {
        REQUIRE(setZ80Bank(e, bank));
        REQUIRE(e.backend()->getZ80Regs().bank == bank);
        spy.regionReads.clear();
        const auto v = listAroundZ80(spy, centre, 4, 4);
        REQUIRE(v.size() == 9u);
        CHECK(contiguous(v));
        for (const auto& l : v) {
            CHECK(!l.readable);
            CHECK_EQ(l.len, 1u);
            CHECK_STR(l.text, disasm::kNotReadable);
        }
        CHECK(spy.regionReads.empty());
    }

    // Back at the start of the ROM: the 68000 bring-up, decoded as Z80 code.
    REQUIRE(setZ80Bank(e, 0));
    REQUIRE(e.backend()->getZ80Regs().bank == 0u);
    const std::vector<uint8_t> rom = e.backend()->readRegion(0, 0, kRomSize);
    REQUIRE(rom.size() == kRomSize);

    spy.regionReads.clear();
    const auto v = listAroundZ80(spy, centre, 4, 8);
    REQUIRE(v.size() == 13u);
    CHECK(contiguous(v));
    REQUIRE(indexOf(v, centre) == 4u);
    for (const auto& l : v) {
        CHECK(l.readable);
        const uint32_t off = l.addr - 0x8000;
        const auto want = disasm::disasmZ80(&rom[off], rom.size() - off, uint16_t(l.addr));
        CHECK_EQ(l.len, want.len);
        CHECK_STR(l.text, want.text);
    }
    CHECK_STR(v[4].text, "inc     sp");                 // 33
    CHECK_STR(v[5].text, "call    m,$0001");            // FC 01 00
    CHECK_STR(v[5].hex, "FC 01 00");
    CHECK(v[5].target == std::optional<uint32_t>(0x0001u));
    for (const auto& r : spy.regionReads) {
        CHECK_EQ(r.id, 0);
        CHECK(r.off + r.size <= kRomSize);
    }

    // Across $8000 the ports end and the ROM begins; at $FFFF the lines end
    // instead of wrapping round to sound RAM.
    const auto low = listAroundZ80(spy, 0x8000, 2, 2);
    REQUIRE(low.size() == 5u);
    CHECK(contiguous(low));
    CHECK(!low[0].readable);
    CHECK(!low[1].readable);
    CHECK_EQ(low[2].addr, 0x8000u);
    CHECK_STR(low[2].text, "nop");                      // the stack pointer's top byte
    CHECK_STR(low[3].text, "rst     $38");
    CHECK(low[3].target == std::optional<uint32_t>(0x38u));

    const auto high = listAroundZ80(spy, 0xFFFE, 2, 4);
    REQUIRE(high.size() == 4u);
    CHECK(contiguous(high));
    CHECK_EQ(high.back().addr, 0xFFFFu);
    CHECK_STR(high.back().text, "rst     $38");          // the ROM's $FF fill
    CHECK(high.back().readable);

    CHECK_EQ(spy.busReads, 0);
}

// Each write a game makes to $A11100 and $A11200 shows in zstate: the 68000
// below does them one step at a time. The first pause can come before the
// ROM's own bring-up has run, so the routine starts by releasing both.
TEST(z80_state_follows_busreq_and_reset)
{
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    constexpr uint32_t at = 0xFF2000;
    const uint8_t code[] = {
        0x33, 0xFC, 0x00, 0x00, 0x00, 0xA1, 0x11, 0x00,   // move.w #$0000,$A11100  bus released
        0x33, 0xFC, 0x01, 0x00, 0x00, 0xA1, 0x12, 0x00,   // move.w #$0100,$A11200  /RESET released
        0x33, 0xFC, 0x01, 0x00, 0x00, 0xA1, 0x11, 0x00,   // move.w #$0100,$A11100  BUSREQ
        0x33, 0xFC, 0x00, 0x00, 0x00, 0xA1, 0x12, 0x00,   // move.w #$0000,$A11200  /RESET asserted
        0x33, 0xFC, 0x01, 0x00, 0x00, 0xA1, 0x12, 0x00,   // move.w #$0100,$A11200  /RESET released
        0x33, 0xFC, 0x00, 0x00, 0x00, 0xA1, 0x11, 0x00,   // move.w #$0000,$A11100  bus released
        0x60, 0xFE,                                       // bra.s *
    };
    REQUIRE(e.backend()->writeRegion(1, at & 0xFFFF, code, sizeof code));
    e.parkM68kAt(at);
    REQUIRE(e.stepAndWait(Cpu::M68K));

    uint32_t pc = at + 8;
    for (unsigned want : { 1u, 3u, 2u, 3u, 1u }) {
        REQUIRE(e.stepAndWait(Cpu::M68K));
        pc += 8;
        REQUIRE(e.backend()->getM68kRegs().pc == pc);
        CHECK_EQ(e.backend()->getZ80Regs().zstate, want);
    }

    // It belongs to the bus glue, not the CPU: setZ80Regs leaves it alone.
    Z80Regs z = e.backend()->getZ80Regs();
    z.zstate = 0;
    e.backend()->setZ80Regs(z);
    CHECK_EQ(e.backend()->getZ80Regs().zstate, 1u);
}

// A target comes from the operand that names it, with 16-bit arithmetic.
TEST(listing_z80_flow_targets_from_disasm_text)
{
    struct Case { std::vector<uint8_t> bytes; uint16_t addr; std::optional<uint32_t> target; };
    const Case cases[] = {
        { { 0xC3, 0x34, 0x12 }, 0x0100, 0x1234 },          // jp $1234
        { { 0xC2, 0x34, 0x12 }, 0x0100, 0x1234 },          // jp nz,$1234
        { { 0xFA, 0x00, 0x80 }, 0x0100, 0x8000 },          // jp m,$8000
        { { 0x18, 0xFE },       0x0100, 0x0100 },          // jr $
        { { 0x20, 0x02 },       0x0100, 0x0104 },          // jr nz
        { { 0x38, 0x80 },       0x0100, 0x0082 },          // jr c, back 128
        { { 0x10, 0xFE },       0x0100, 0x0100 },          // djnz $
        { { 0x18, 0x7F },       0xFFF0, 0x0071 },          // past $FFFF
        { { 0x18, 0x80 },       0x0010, 0xFF92 },          // below 0
        { { 0xCD, 0x20, 0x01 }, 0x0100, 0x0120 },          // call $0120
        { { 0xC4, 0x20, 0x01 }, 0x0100, 0x0120 },          // call nz,$0120
        { { 0xFC, 0x01, 0x00 }, 0x0100, 0x0001 },          // call m,$0001
        { { 0xC7 },             0x0100, 0x0000 },          // rst $00
        { { 0xFF },             0x0100, 0x0038 },          // rst $38
        { { 0xE9 },             0x0100, std::nullopt },    // jp (hl)
        { { 0xDD, 0xE9 },       0x0100, std::nullopt },    // jp (ix)
        { { 0xFD, 0xE9 },       0x0100, std::nullopt },    // jp (iy)
        { { 0xC9 },             0x0100, std::nullopt },    // ret
        { { 0xC0 },             0x0100, std::nullopt },    // ret nz
        { { 0xED, 0x4D },       0x0100, std::nullopt },    // reti
        { { 0x21, 0x00, 0x10 }, 0x0100, std::nullopt },    // ld hl,$1000
        { { 0x3A, 0x00, 0x10 }, 0x0100, std::nullopt },    // ld a,($1000)
        { { 0x22, 0x00, 0x10 }, 0x0100, std::nullopt },    // ld ($1000),hl
        { { 0xDB, 0x12 },       0x0100, std::nullopt },    // in a,($12)
        { { 0x3E, 0x12 },       0x0100, std::nullopt },    // ld a,$12
        { { 0xDD, 0x00 },       0x0100, std::nullopt },    // db $dd
    };
    for (const auto& c : cases) {
        const auto line = disasm::disasmZ80(c.bytes.data(), c.bytes.size(), c.addr);
        const auto got = disasm::flowTargetZ80(line.text);
        if (got != c.target)
            t::fail(__FILE__, __LINE__,
                    t::hexBytes(c.bytes) + " at " + t::hex(c.addr) + " \"" + line.text + "\": got "
                        + (got ? t::hex(*got) : std::string("none")) + ", want "
                        + (c.target ? t::hex(*c.target) : std::string("none")));
    }
}
