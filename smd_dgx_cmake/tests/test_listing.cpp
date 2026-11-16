// listAround: the lines a disassembly window shows, on the synthetic ROM.
//
// Two things here matter more than the text, which test_disasm already pins:
// that the lines before an address are whole instructions of the real stream,
// not the tail of one read from the middle; and that building a listing reads
// nothing but ROM and work RAM, because on this bus a read of anything else
// is an access the game sees.

#include "fixture.h"

#include "debugger/Disasm68k.h"
#include "debugger/DisasmListing.h"

#include <optional>
#include <vector>

using namespace synth;
using disasm::ListingLine;
using disasm::listAround;

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
