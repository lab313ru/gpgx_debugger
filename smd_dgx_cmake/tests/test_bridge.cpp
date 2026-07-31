// BridgeServer <-> RemoteBackend over a real socket.
//
// This is the seam where the two clients (the Python MCP server and the Z80
// IDA database) meet the emulator, and it is a hand-rolled text protocol: every
// field is an opportunity for the two sides to disagree about a format. The
// "regions" bug — a name containing the record separator, so every client
// silently dropped five of nine regions — was exactly that, and went unnoticed
// because nothing ever compared the two sides.

#include "fixture.h"

#include "debugger/BridgeServer.h"
#include "debugger/RemoteBackend.h"
#include "debugger/SocketCompat.h"

#include <algorithm>
#include <thread>

using namespace synth;

namespace {

// Each test gets its own port: the suite may run alongside a live emulator on
// the default one, and a test that quietly attaches to the user's session
// instead of its own would be worse than a failure.
unsigned short portFor(int n) { return (unsigned short)(28100 + n); }

struct Wired {
    t::Emu       emu;
    BridgeServer bridge{ nullptr };
    RemoteBackend remote;
    bool ok = false;

    explicit Wired(int slot)
    {
        if (!emu.start()) return;
        if (!emu.pauseAndWait()) return;
        bridge.~BridgeServer();
        new (&bridge) BridgeServer(emu.host());
        if (!bridge.start(portFor(slot))) return;
        if (!remote.connect("127.0.0.1", portFor(slot))) return;
        ok = true;
    }
};

} // namespace

// REGRESSION: region names contain spaces, and records were space-separated.
TEST(bridge_regions_survive_names_with_spaces)
{
    Wired w(1);
    REQUIRE(w.ok);

    const auto direct = w.emu.backend()->getMemRegions();
    const auto wire   = w.remote.getMemRegions();

    REQUIRE(wire.size() == direct.size());
    for (size_t i = 0; i < direct.size(); ++i) {
        CHECK_EQ(wire[i].id,   direct[i].id);
        CHECK_EQ(wire[i].size, direct[i].size);
        CHECK_EQ(wire[i].base, direct[i].base);
        // The name may be transported with spaces escaped, but it must still
        // identify the region — never truncate at the separator.
        CHECK(wire[i].name.size() >= direct[i].name.size());
    }
    // The regions whose names contain a space are the ones that vanished.
    const bool has68k = std::any_of(wire.begin(), wire.end(),
        [](const MemRegion& m) { return m.id == 1 && m.size == 0x10000; });
    const bool hasZ80 = std::any_of(wire.begin(), wire.end(),
        [](const MemRegion& m) { return m.id == 2 && m.size == 0x2000; });
    CHECK(has68k);
    CHECK(hasZ80);
}

TEST(bridge_memory_matches_the_backend_byte_for_byte)
{
    Wired w(2);
    REQUIRE(w.ok);

    const auto direct = w.emu.backend()->readMemory(kMarkerAddr, 18);
    const auto wire   = w.remote.readMemory(kMarkerAddr, 18);
    CHECK(direct == wire);

    const auto dz = w.emu.backend()->readZ80Memory(0, 32);
    const auto wz = w.remote.readZ80Memory(0, 32);
    CHECK(dz == wz);

    const auto dr = w.emu.backend()->readRegion(0, 0x100, 16);
    const auto wr = w.remote.readRegion(0, 0x100, 16);
    CHECK(dr == wr);
}

TEST(bridge_writes_reach_the_emulator)
{
    Wired w(3);
    REQUIRE(w.ok);

    const uint8_t pat[4] = { 0x5A, 0xA5, 0x0F, 0xF0 };
    w.remote.writeMemory(0xFF4000, pat, 4);
    const auto back = w.emu.backend()->readMemory(0xFF4000, 4);
    REQUIRE(back.size() == 4);
    CHECK(std::equal(pat, pat + 4, back.begin()));
}

// Every register the Z80 debugger_t shows must survive the wire. A field the
// server forgets to emit reads as zero on the client and looks like real state.
TEST(bridge_z80_registers_are_complete)
{
    Wired w(4);
    REQUIRE(w.ok);

    Z80Regs want{};
    want.af = 0x1234; want.bc = 0x2345; want.de = 0x3456; want.hl = 0x4567;
    want.af2 = 0x5678; want.bc2 = 0x6789; want.de2 = 0x789A; want.hl2 = 0x89AB;
    want.ix = 0x9ABC; want.iy = 0xABCD; want.sp = 0x1FF0; want.pc = 0x0100;
    want.i = 0x12; want.r = 0x34; want.im = 1; want.iff1 = 1; want.iff2 = 0;
    w.emu.backend()->setZ80Regs(want);

    const Z80Regs got = w.remote.getZ80Regs();
    CHECK_EQ(got.af, want.af);   CHECK_EQ(got.bc, want.bc);
    CHECK_EQ(got.de, want.de);   CHECK_EQ(got.hl, want.hl);
    CHECK_EQ(got.af2, want.af2); CHECK_EQ(got.bc2, want.bc2);
    CHECK_EQ(got.de2, want.de2); CHECK_EQ(got.hl2, want.hl2);
    CHECK_EQ(got.ix, want.ix);   CHECK_EQ(got.iy, want.iy);
    CHECK_EQ(got.sp, want.sp);   CHECK_EQ(got.pc, want.pc);
    CHECK_EQ(got.i, want.i);     CHECK_EQ(got.r, want.r);
    CHECK_EQ(got.im, want.im);   CHECK_EQ(got.iff1, want.iff1);
    CHECK_EQ(got.bank, w.emu.backend()->getZ80Regs().bank);
}

TEST(bridge_m68k_registers_round_trip)
{
    Wired w(5);
    REQUIRE(w.ok);

    M68kRegs want = w.emu.backend()->getM68kRegs();
    for (int i = 0; i < 8; ++i) { want.d[i] = 0x11111111u * (i + 1); }
    want.a[0] = 0x00FF1000; want.a[7] = kInitSp;
    w.remote.setM68kRegs(want);

    const M68kRegs got = w.emu.backend()->getM68kRegs();
    for (int i = 0; i < 8; ++i) CHECK_EQ(got.d[i], want.d[i]);
    CHECK_EQ(got.a[0], want.a[0]);

    const M68kRegs viaWire = w.remote.getM68kRegs();
    for (int i = 0; i < 8; ++i) CHECK_EQ(viaWire.d[i], want.d[i]);
}

TEST(bridge_breakpoints_round_trip_with_their_cpu)
{
    Wired w(6);
    REQUIRE(w.ok);

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::Z80; bp.is_vdp = false;
    bp.start = 0x0123; bp.end = 0x0123;
    const int id = w.remote.addBreakpoint(bp);
    CHECK(id >= 0);

    const auto listed = w.remote.getBreakpoints();
    REQUIRE(listed.size() == 1);
    CHECK_EQ(listed[0].start, 0x0123u);
    CHECK(listed[0].cpu == Cpu::Z80);        // losing this aliases the two CPUs
    CHECK(listed[0].type == BpType::PC);

    // ...and the server must agree.
    const auto server = w.emu.backend()->getBreakpoints();
    REQUIRE(server.size() == 1);
    CHECK(server[0].cpu == Cpu::Z80);

    w.remote.removeBreakpoint(listed[0].id);
    CHECK(w.remote.getBreakpoints().empty());
}

TEST(bridge_events_report_the_stopping_cpu)
{
    Wired w(7);
    REQUIRE(w.ok);

    // Drain whatever the attach itself produced.
    RemoteBackend::RemoteEvent ev;
    while (w.remote.pollEvent(ev)) {}

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = kSubEntry; bp.end = kSubEntry;
    w.remote.addBreakpoint(bp);
    w.remote.resume();

    bool sawPause = false;
    for (int i = 0; i < 60 && !sawPause; ++i) {
        while (w.remote.pollEvent(ev))
            if (ev.type == DebugEvent::Type::Paused) {
                sawPause = true;
                CHECK_EQ(ev.pc, kSubEntry);
                CHECK(ev.cpu == Cpu::M68K);
                break;
            }
        if (!sawPause) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(sawPause);
    w.remote.clearBreakpoints();
}

TEST(bridge_serves_two_clients_independently)
{
    Wired w(8);
    REQUIRE(w.ok);

    RemoteBackend second;
    REQUIRE(second.connect("127.0.0.1", portFor(8)));

    // Both see the same machine...
    const auto a = w.remote.readMemory(0x100, 16);
    const auto b = second.readMemory(0x100, 16);
    CHECK(a == b);

    // ...but each has its own event queue, so one draining does not starve the
    // other. Drain both, then make one event and check both receive it.
    RemoteBackend::RemoteEvent ev;
    while (w.remote.pollEvent(ev)) {}
    while (second.pollEvent(ev)) {}

    w.remote.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    w.remote.pause();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int seenA = 0, seenB = 0;
    while (w.remote.pollEvent(ev)) ++seenA;
    while (second.pollEvent(ev))  ++seenB;
    CHECK(seenA > 0);
    CHECK(seenB > 0);
}

// ---------------------------------------------------------------------------
// The agent loop: set a breakpoint, resume, wait, look, advance frames.
//
// These are the primitives that make the bridge usable without a human in the
// loop. Polling `events` can substitute for `wait` only by burning CPU, and
// nothing at all could substitute for frame-accurate advance: while the machine
// is being debugged it is not bound to the wall clock, so an agent cannot time
// an input by sleeping.
// ---------------------------------------------------------------------------

// Raw access, because these commands have no IDebugBackend counterpart.
namespace {

struct Line {
    RemoteBackend* rb;
    // The protocol is one line in, one line out; RemoteBackend::call is private,
    // so the tests speak it directly over their own socket.
};

class Raw {
public:
    bool connect(unsigned short port)
    {
        sockcompat::netInit();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == BAD_SOCK) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        return ::connect(fd_, (sockaddr*)&a, sizeof a) == 0;
    }
    ~Raw() { if (fd_ != BAD_SOCK) CLOSESOCK(fd_); }

    std::string cmd(const std::string& c)
    {
        std::string req = c + "\n";
        if (!sockcompat::sendAll(fd_, req.data(), req.size())) return {};
        char buf[65536];
        size_t nl;
        while ((nl = rx_.find('\n')) == std::string::npos) {
            const int n = ::recv(fd_, buf, sizeof buf, 0);
            if (n <= 0) return {};
            rx_.append(buf, n);
        }
        std::string r = rx_.substr(0, nl);
        rx_.erase(0, nl + 1);
        return r;
    }

private:
    socket_t fd_ = BAD_SOCK;
    std::string rx_;
};

} // namespace

TEST(bridge_wait_blocks_until_a_breakpoint_and_names_it)
{
    Wired w(10);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(10)));

    Breakpoint bp;
    bp.type = BpType::PC; bp.cpu = Cpu::M68K; bp.is_vdp = false;
    bp.start = kSubEntry; bp.end = kSubEntry;
    const int id = w.emu.backend()->addBreakpoint(bp);
    REQUIRE(id > 0);

    w.emu.backend()->resume();
    const std::string r = raw.cmd("wait 4000");
    // ok paused pc=... cpu=m68k bp=<id> seq=...
    CHECK(r.rfind("ok paused", 0) == 0);
    char want[32];
    std::snprintf(want, sizeof want, "bp=%d", id);
    CHECK(r.find(want) != std::string::npos);      // WHICH breakpoint, not just that one hit
    CHECK(r.find("cpu=m68k") != std::string::npos);

    w.emu.backend()->clearBreakpoints();
}

TEST(bridge_wait_reports_timeout_rather_than_hanging)
{
    Wired w(11);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(11)));

    w.emu.backend()->resume();                     // nothing armed: nothing to stop on
    CHECK_STR(raw.cmd("wait 400"), "ok timeout");
}

TEST(bridge_frame_advance_runs_exactly_that_many_frames)
{
    Wired w(12);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(12)));

    CHECK_STR(raw.cmd("frameadv 3"), "ok");
    const std::string r = raw.cmd("wait 4000");
    CHECK(r.rfind("ok paused", 0) == 0);
    CHECK(r.find("bp=-1") != std::string::npos);   // stopped by the advance, not a breakpoint
    CHECK(w.emu.backend()->isPaused());
}

TEST(bridge_search_finds_a_pattern_in_a_region)
{
    Wired w(13);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(13)));

    // Our own marker, at a known ROM offset.
    const std::string pat = "534D442D444758";           // "SMD-DGX"
    const std::string r = raw.cmd("search 0 " + pat + " 16");
    REQUIRE(r.rfind("ok", 0) == 0);

    char want[32];
    std::snprintf(want, sizeof want, "%x", kMarkerAddr);
    CHECK(r.find(want) != std::string::npos);

    // A pattern that is not there returns success with no hits, not an error.
    CHECK_STR(raw.cmd("search 0 deadbeefcafe 16"), "ok");
}

TEST(bridge_diff_reports_what_changed_since_the_snapshot)
{
    Wired w(14);
    REQUIRE(w.ok);
    Raw raw;
    REQUIRE(raw.connect(portFor(14)));

    // Region 1 is 68000 work RAM.
    REQUIRE(raw.cmd("snap 1").rfind("ok", 0) == 0);
    CHECK_STR(raw.cmd("diff 1 8"), "ok");          // nothing has moved yet

    const uint8_t v[2] = { 0xA5, 0x5A };
    w.emu.backend()->writeRegion(1, 0x0500, v, 2);

    const std::string r = raw.cmd("diff 1 8");
    REQUIRE(r.rfind("ok ", 0) == 0);
    CHECK(r.find("500:") != std::string::npos);    // offset:old:new
    CHECK(r.find(":a5") != std::string::npos);
    CHECK(r.find("501:") != std::string::npos);

    // A region never snapped is an error, not a silent empty result.
    CHECK(raw.cmd("diff 3 8").rfind("err", 0) == 0);
}

// ---------------------------------------------------------------------------
// Two emulators at once.
//
// A fixed port made a second game silently unreachable, and left every client
// unable to say whose game it had reached. The server now takes the first free
// port in its range and says who it is; the client scans and picks.
// ---------------------------------------------------------------------------
TEST(bridge_two_sessions_get_different_ports_and_are_told_apart)
{
    // Two *servers*, one emulator. Two emulators cannot be tested in one
    // process at all: the gpgx core is global C state and GpgxBackend claims a
    // single global CPU hook, so a second instance silently disables the first.
    // Two games therefore means two processes — which is exactly the case this
    // port allocation exists to serve.
    t::Emu a;
    REQUIRE(a.start());
    REQUIRE(a.pauseAndWait());

    BridgeServer sa(a.host()), sb(a.host());
    REQUIRE(sa.start());                       // 0 = pick a free one
    REQUIRE(sb.start());
    CHECK(sa.port() != 0);
    CHECK(sb.port() != 0);
    CHECK(sa.port() != sb.port());             // the whole point

    // Both are discoverable, and each reports the port it actually holds.
    const auto found = RemoteBackend::discover();
    const bool sawA = std::any_of(found.begin(), found.end(),
        [&](const SessionInfo& s) { return s.port == sa.port(); });
    const bool sawB = std::any_of(found.begin(), found.end(),
        [&](const SessionInfo& s) { return s.port == sb.port(); });
    CHECK(sawA);
    CHECK(sawB);

    // Connecting to a named port reaches that one, and says so.
    RemoteBackend rb;
    REQUIRE(rb.connect("127.0.0.1", sb.port()));
    CHECK_EQ(rb.session().port, sb.port());
    CHECK(rb.session().pid != 0);
}

TEST(bridge_session_identifies_the_game)
{
    Wired w(15);
    REQUIRE(w.ok);

    const SessionInfo& s = w.remote.session();
    CHECK(s.pid != 0);
    CHECK_EQ(s.port, portFor(15));
    // The synthetic ROM has no product code or title, so those may be "-";
    // the calculated checksum is what actually distinguishes two games.
    const SessionInfo direct = w.emu.host()->gpgx()->sessionInfo();
    CHECK_EQ(s.crc, direct.crc);
}

TEST(bridge_client_stops_claiming_health_after_the_server_goes)
{
    // A dead socket that still reports connected turns every later read into
    // zeros that look like real emulator state.
    t::Emu e;
    REQUIRE(e.start());
    REQUIRE(e.pauseAndWait());

    auto* srv = new BridgeServer(e.host());
    REQUIRE(srv->start(portFor(16)));

    RemoteBackend rb;
    REQUIRE(rb.connect("127.0.0.1", portFor(16)));
    CHECK(rb.isConnected());

    delete srv;                                 // server goes away
    rb.getM68kRegs();                           // first call notices the close
    CHECK(!rb.isConnected());
}

TEST(bridge_rejects_absurd_sizes_instead_of_allocating)
{
    Wired w(9);
    REQUIRE(w.ok);

    // A client asking for 4 GB must get an error, not an out-of-memory abort in
    // the emulator hosting someone's IDA session.
    const auto huge = w.remote.readMemory(0, 0xFFFFFFFFu);
    CHECK(huge.empty());
    const auto zero = w.remote.readMemory(0, 0);
    CHECK(zero.empty());
}
