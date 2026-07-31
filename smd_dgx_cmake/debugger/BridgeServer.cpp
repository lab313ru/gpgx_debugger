#include "BridgeServer.h"
#include "EmuHost.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <memory>
#include <vector>

#include "SocketCompat.h"

namespace {

const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const uint8_t* data, size_t n)
{
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        const unsigned b0 = data[i];
        const unsigned b1 = (i + 1 < n) ? data[i + 1] : 0;
        const unsigned b2 = (i + 2 < n) ? data[i + 2] : 0;
        const unsigned v = (b0 << 16) | (b1 << 8) | b2;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += (i + 1 < n) ? kB64[(v >> 6) & 63] : '=';
        out += (i + 2 < n) ? kB64[v & 63]        : '=';
    }
    return out;
}

std::string toHex(const uint8_t* data, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { out += d[data[i] >> 4]; out += d[data[i] & 15]; }
    return out;
}

std::vector<uint8_t> fromHex(const std::string& s)
{
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

uint32_t parseU32(const std::string& s, int base = 16)
{
    return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, base));
}


unsigned currentPid()
{
#ifdef _WIN32
    return (unsigned)GetCurrentProcessId();
#else
    return (unsigned)getpid();
#endif
}

// The reply is whitespace-separated key=value, so a value may contain neither.
// Game titles contain both, routinely.
std::string sanitise(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const bool bad = (c == ' ') || (c == '=') || (c == ':')
                      || (unsigned char)c < 0x20 || (unsigned char)c > 0x7E;
        out += bad ? '_' : c;
    }
    return out;
}

} // namespace

// Try one port. Returns true and fills fd on success; leaves fd untouched
// otherwise, so the caller can simply try the next.
bool BridgeServer::bindOne(unsigned short p, long long& fdOut)
{
    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BAD_SOCK) return false;

    // Exclusivity, not reuse. On Windows SO_REUSEADDR lets a SECOND process
    // bind a port that is already listening: both emulators would claim the
    // same one, whichever the kernel favoured would get every client, and the
    // other would sit there looking healthy and unreachable — which also makes
    // scanning for a free port meaningless. On POSIX SO_REUSEADDR only skips
    // TIME_WAIT, which is what we want there.
    int yes = 1;
#ifdef _WIN32
    ::setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&yes, sizeof yes);
#else
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(p);
    // Loopback only: this is an unauthenticated control channel with full
    // memory read/write over the emulated machine — it must never be reachable
    // from another host.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, (sockaddr*)&addr, sizeof addr) != 0 || ::listen(fd, 4) != 0) {
        CLOSESOCK(fd);
        return false;
    }
    fdOut = static_cast<long long>(fd);
    return true;
}

// ---------------------------------------------------------------------------
bool BridgeServer::start(unsigned short port)
{
    stop();
    stopFlag_.store(false);

    if (!sockcompat::netInit()) return false;

    // An explicit port must be honoured exactly — a caller that asked for one
    // wants to fail loudly, not land somewhere else. Only the default scans.
    unsigned short first = port, last = port;
    if (port == 0) {
        if (const char* env = std::getenv("SMD_DGX_PORT")) {
            const unsigned v = (unsigned)std::strtoul(env, nullptr, 10);
            if (v > 0 && v < 65536) { first = last = (unsigned short)v; }
            else { first = kBasePort; last = kBasePort + kPortRange - 1; }
        } else {
            first = kBasePort;
            last  = kBasePort + kPortRange - 1;
        }
    }

    socket_t fd = BAD_SOCK;
    for (unsigned p = first; p <= last; ++p) {
        long long got = -1;
        if (!bindOne((unsigned short)p, got)) continue;
        fd = (socket_t)got;
        port_ = (unsigned short)p;
        break;
    }
    if (fd == BAD_SOCK) return false;

    listenFd_ = static_cast<long long>(fd);
    running_.store(true);
    sinkId_ = host_->addEventSink([this](const DebugEvent& ev) { onEmuEvent(&ev); });
    thread_   = std::thread(&BridgeServer::serve, this);
    return true;
}

void BridgeServer::stop()
{
    // First, before anything else: the host outlives this object in every
    // caller, and its own shutdown emits a final Stopped event. A sink still
    // pointing here would be called on a half-destroyed BridgeServer.
    if (host_ && sinkId_ >= 0) { host_->removeEventSink(sinkId_); sinkId_ = -1; }

    if (!thread_.joinable()) { running_.store(false); return; }
    stopFlag_.store(true);
    if (listenFd_ >= 0) {
        CLOSESOCK(static_cast<socket_t>(listenFd_));   // unblocks accept()
        listenFd_ = -1;
    }
    // Close the client sockets so their recv() returns and the threads exit;
    // otherwise a connected debugger keeps this object alive past the host.
    {
        std::lock_guard<std::mutex> lk(clientsMx_);
        for (auto& c : clients_)
            if (c->fd >= 0) { CLOSESOCK(static_cast<socket_t>(c->fd)); c->fd = -1; }
    }
    thread_.join();
    for (auto& t : clientThreads_) if (t.joinable()) t.join();
    clientThreads_.clear();
    { std::lock_guard<std::mutex> lk(clientsMx_); clients_.clear(); }
    running_.store(false);
    // No WSACleanup: it is refcounted per process, and this object is not the
    // only socket user in an IDA process. See SocketCompat.h.
}

void BridgeServer::serve()
{
    while (!stopFlag_.load()) {
        socket_t fd = ::accept(static_cast<socket_t>(listenFd_), nullptr, nullptr);
        if (fd == BAD_SOCK) break;               // listener closed by stop()

        sockcompat::suppressSigpipe(fd);

        auto c = std::make_shared<Client>();
        c->fd = static_cast<long long>(fd);
        {
            std::lock_guard<std::mutex> lk(clientsMx_);
            clients_.push_back(c);
            clientThreads_.emplace_back(&BridgeServer::serveClient, this, c);
        }
    }
}

void BridgeServer::serveClient(std::shared_ptr<Client> c)
{
    const socket_t fd = static_cast<socket_t>(c->fd);
    std::string buf;
    char chunk[4096];

    while (!stopFlag_.load()) {
        const int n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) break;
        buf.append(chunk, n);

        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::string reply = handle(line, *c);
            reply += '\n';
            if (!sockcompat::sendAll(fd, reply.data(), reply.size()))
                goto gone;              // peer vanished mid-reply
        }
    }
gone:

    // Claim the descriptor before closing it. stop() closes client sockets to
    // wake their threads, so without this handshake the same fd is closed
    // twice — and a descriptor number reused by then belongs to someone else.
    {
        std::lock_guard<std::mutex> lk(clientsMx_);
        if (c->fd >= 0) { c->fd = -1; CLOSESOCK(fd); }
        for (size_t i = 0; i < clients_.size(); ++i)
            if (clients_[i] == c) { clients_.erase(clients_.begin() + i); break; }
    }
}

// Runs on the EMULATION thread. Queue and return — never touch the backend
// from here, and never block: the emulator is waiting on us.
void BridgeServer::onEmuEvent(const void* evp)
{
    const DebugEvent& ev = *static_cast<const DebugEvent*>(evp);
    std::lock_guard<std::mutex> lk(clientsMx_);
    const uint64_t seq = nextSeq_++;
    for (auto& c : clients_) {
        if (c->events.size() >= 256) { c->events.pop_front(); ++c->dropped; }
        c->events.push_back(QueuedEvent{ seq, uint8_t(ev.type), uint8_t(ev.cpu), ev.pc, ev.bpId });
    }
    eventCv_.notify_all();
}

std::string BridgeServer::handleEvents(Client& c, int max)
{
    std::lock_guard<std::mutex> lk(clientsMx_);
    std::ostringstream o;
    std::vector<std::string> tuples;

    if (c.dropped) {                       // tell the client it missed some
        std::ostringstream d;
        d << "0,dropped," << std::dec << c.dropped << ",-";
        tuples.push_back(d.str());
        c.dropped = 0;
    }
    static const char* const kNames[] = { "started", "paused", "resumed", "stopped" };
    while (!c.events.empty() && (int)tuples.size() < max) {
        const QueuedEvent e = c.events.front();
        c.events.pop_front();
        std::ostringstream t;
        t << std::dec << e.seq << ","
          << (e.type < 4 ? kNames[e.type] : "?") << ","
          << std::hex << e.pc << ","
          << (e.cpu ? "z80" : "m68k") << ","
          << std::dec << e.bpId;
        tuples.push_back(t.str());
    }

    o << "ok " << std::dec << tuples.size();
    for (const auto& t : tuples) o << " " << t;
    return o.str();
}


// Block until this client sees a stop, instead of making it spin on `events`.
//
// This is the primitive the whole agent loop is built on: set a breakpoint,
// resume, and wait. Polling instead means either a busy loop or a latency that
// makes "which frame did that happen on?" unanswerable.
std::string BridgeServer::handleWait(Client& c, int timeoutMs)
{
    static const char* const kNames[] = { "started", "paused", "resumed", "stopped" };
    std::unique_lock<std::mutex> lk(clientsMx_);

    QueuedEvent hit{};
    const bool got = eventCv_.wait_for(
        lk, std::chrono::milliseconds(timeoutMs),
        [&] {
            while (!c.events.empty()) {
                const QueuedEvent e = c.events.front();
                c.events.pop_front();
                // Only a stop ends a wait. Resumes and starts are still drained
                // so they cannot make the next wait return instantly.
                if (e.type == uint8_t(DebugEvent::Type::Paused) ||
                    e.type == uint8_t(DebugEvent::Type::Stopped)) {
                    hit = e;
                    return true;
                }
            }
            return stopFlag_.load();
        });

    if (!got || hit.seq == 0) return "ok timeout";

    std::ostringstream o;
    o << "ok " << (hit.type < 4 ? kNames[hit.type] : "?")
      << " pc=" << std::hex << hit.pc
      << " cpu=" << (hit.cpu ? "z80" : "m68k")
      << " bp=" << std::dec << hit.bpId
      << " seq=" << hit.seq;
    return o.str();
}

// ---------------------------------------------------------------------------
std::string BridgeServer::handle(const std::string& line, Client& client)
{
    if (!host_) return "err no host";
    IDebugBackend* be = host_->backend();

    std::istringstream is(line);
    std::string cmd;
    is >> cmd;
    if (cmd.empty()) return "err empty";

    auto arg = [&is]() { std::string s; is >> s; return s; };

    // Identity, not just liveness: with several emulators running, a client
    // that only knows "something answered" cannot tell whose game it is.
    if (cmd == "ping") {
        const SessionInfo si = host_->gpgx()->sessionInfo();
        std::ostringstream o;
        o << "ok smd_dgx pid=" << std::dec << currentPid()
          << " port=" << port_
          << " crc=" << std::hex << si.crc
          << " serial=" << (si.serial.empty() ? "-" : sanitise(si.serial))
          << " name=" << (si.name.empty() ? "-" : sanitise(si.name));
        return o.str();
    }

    if (cmd == "status") {
        char out[128];
        std::snprintf(out, sizeof out, "ok running=%d paused=%d pc=%06X",
                      host_->isRunning() ? 1 : 0, be->isPaused() ? 1 : 0,
                      be->getM68kRegs().pc & 0xFFFFFF);
        return out;
    }

    if (cmd == "regs68k") {
        const M68kRegs r = be->getM68kRegs();
        std::ostringstream o;
        o << "ok";
        for (int i = 0; i < 8; ++i) o << " d" << i << "=" << std::hex << r.d[i];
        for (int i = 0; i < 8; ++i) o << " a" << i << "=" << std::hex << r.a[i];
        o << " pc=" << std::hex << r.pc << " sr=" << r.sr
          << " usp=" << r.usp << " isp=" << r.isp;
        return o.str();
    }

    if (cmd == "regsz80") {
        const Z80Regs r = be->getZ80Regs();
        std::ostringstream o;
        o << "ok" << std::hex
          << " af=" << r.af << " bc=" << r.bc << " de=" << r.de << " hl=" << r.hl
          << " af2=" << r.af2 << " bc2=" << r.bc2 << " de2=" << r.de2 << " hl2=" << r.hl2
          << " ix=" << r.ix << " iy=" << r.iy << " sp=" << r.sp << " pc=" << r.pc
          << " i=" << (int)r.i << " r=" << (int)r.r << " im=" << (int)r.im
          << " iff1=" << (int)r.iff1 << " iff2=" << (int)r.iff2
          << " halt=" << (int)r.halt << " bank=" << r.bank;
        return o.str();
    }

    if (cmd == "vdp") {
        const VdpState v = be->getVdpState();
        std::ostringstream o;
        o << "ok reg=" << toHex(v.reg, 24)
          << " status=" << std::hex << v.status
          << " dma_len=" << v.dma_len << " dma_src=" << v.dma_src
          << " dma_type=" << (int)v.dma_type;
        return o.str();
    }

    if (cmd == "read") {
        const uint32_t a = parseU32(arg());
        const uint32_t n = parseU32(arg(), 10);
        if (n == 0 || n > (1u << 20)) return "err bad size";
        const auto d = be->readMemory(a, n);
        return "ok " + toHex(d.data(), d.size());
    }

    if (cmd == "write") {
        const uint32_t a = parseU32(arg());
        const auto d = fromHex(arg());
        if (d.empty()) return "err no data";
        return be->writeMemory(a, d.data(), (uint32_t)d.size()) ? "ok" : "err write failed";
    }

    if (cmd == "regions") {
        std::ostringstream o;
        o << "ok";
        for (const auto& r : be->getMemRegions()) {
            // Records are space-separated, so a name may not contain a space:
            // "RAM 68K" would split into two bogus records and every client
            // that checks the field count would silently drop the region.
            std::string name = r.name;
            for (char& c : name) if (c == ' ') c = '_';
            o << " " << r.id << ":" << name << ":" << std::hex << r.base
              << ":" << r.size << ":" << (r.writable ? 1 : 0) << std::dec;
        }
        return o.str();
    }

    if (cmd == "readregion") {
        const int id     = (int)parseU32(arg(), 10);
        const uint32_t o = parseU32(arg());
        const uint32_t n = parseU32(arg(), 10);
        if (n == 0 || n > (1u << 20)) return "err bad size";
        const auto d = be->readRegion(id, o, n);
        return "ok " + toHex(d.data(), d.size());
    }


    if (cmd == "events") {
        int max = (int)parseU32(arg(), 10);
        if (max <= 0 || max > 256) max = 64;
        return handleEvents(client, max);
    }

    // Block until the machine stops. Default 10s: long enough for a breakpoint
    // deep in a level, short enough that a wedged emulator still answers.
    if (cmd == "wait") {
        int ms = (int)parseU32(arg(), 10);
        if (ms <= 0) ms = 10000;
        if (ms > 120000) ms = 120000;
        return handleWait(client, ms);
    }

    // Run exactly n frames and stop. The only way to time an input: while the
    // emulator is being debugged it is not bound to the wall clock.
    if (cmd == "frameadv") {
        int n = (int)parseU32(arg(), 10);
        if (n <= 0) n = 1;
        if (n > 100000) return "err too many frames";
        host_->advanceFrames(n);
        be->resume();
        return "ok";
    }

    // Find a byte pattern in a region. Server-side because the alternative is
    // hauling the whole region across as ASCII hex on every attempt.
    if (cmd == "search") {
        const int id = (int)parseU32(arg(), 10);
        const auto pat = fromHex(arg());
        int max = (int)parseU32(arg(), 10);
        if (pat.empty()) return "err empty pattern";
        if (max <= 0 || max > 4096) max = 256;

        uint32_t size = 0;
        for (const auto& r : be->getMemRegions()) if (r.id == id) size = r.size;
        if (!size) return "err no such region";

        const auto data = be->readRegion(id, 0, size);
        std::ostringstream o;
        o << "ok";
        int found = 0;
        for (size_t i = 0; i + pat.size() <= data.size() && found < max; ++i) {
            if (std::memcmp(data.data() + i, pat.data(), pat.size()) == 0) {
                o << " " << std::hex << (unsigned)i;
                ++found;
            }
        }
        return o.str();
    }

    // Snapshot a region for later comparison.
    if (cmd == "snap") {
        const int id = (int)parseU32(arg(), 10);
        uint32_t size = 0;
        for (const auto& r : be->getMemRegions()) if (r.id == id) size = r.size;
        if (!size) return "err no such region";

        auto data = be->readRegion(id, 0, size);
        const size_t n = data.size();
        {
            std::lock_guard<std::mutex> lk(clientsMx_);
            client.snaps[id] = std::move(data);
        }
        std::ostringstream o; o << "ok " << std::dec << n;
        return o.str();
    }

    // What changed since `snap`. Returns offset:old:new triples — the RAM-search
    // loop that finds a variable by playing the game and asking what moved.
    if (cmd == "diff") {
        const int id = (int)parseU32(arg(), 10);
        int max = (int)parseU32(arg(), 10);
        if (max <= 0 || max > 4096) max = 256;

        std::vector<uint8_t> before;
        {
            std::lock_guard<std::mutex> lk(clientsMx_);
            auto it = client.snaps.find(id);
            if (it == client.snaps.end()) return "err no snapshot for that region";
            before = it->second;
        }
        const auto now = be->readRegion(id, 0, (uint32_t)before.size());
        std::ostringstream o;
        o << "ok";
        int found = 0;
        const size_t n = now.size() < before.size() ? now.size() : before.size();
        for (size_t i = 0; i < n && found < max; ++i) {
            if (now[i] != before[i]) {
                o << " " << std::hex << (unsigned)i << ":" << (unsigned)before[i]
                  << ":" << (unsigned)now[i];
                ++found;
            }
        }
        return o.str();
    }

    // --- register writes -------------------------------------------------
    // One command per CPU rather than per register: the get/modify/set has to
    // be atomic, and a half-applied register set is worse than none.
    if (cmd == "wreg68k") {
        M68kRegs r = be->getM68kRegs();
        for (std::string kv; is >> kv; ) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq);
            const uint32_t    v = parseU32(kv.substr(eq + 1));
            if (k.size() == 2 && (k[0] == 'd' || k[0] == 'a') && k[1] >= '0' && k[1] <= '7') {
                (k[0] == 'd' ? r.d : r.a)[k[1] - '0'] = v;
            }
            else if (k == "pc")  r.pc  = v;
            else if (k == "sr")  r.sr  = v;
            else if (k == "usp") r.usp = v;
            else if (k == "isp") r.isp = v;
        }
        be->setM68kRegs(r);
        return "ok";
    }

    if (cmd == "wregz80") {
        Z80Regs r = be->getZ80Regs();
        for (std::string kv; is >> kv; ) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq);
            const uint32_t    v = parseU32(kv.substr(eq + 1));
            if      (k == "af")  r.af  = uint16_t(v); else if (k == "bc")  r.bc  = uint16_t(v);
            else if (k == "de")  r.de  = uint16_t(v); else if (k == "hl")  r.hl  = uint16_t(v);
            else if (k == "af2") r.af2 = uint16_t(v); else if (k == "bc2") r.bc2 = uint16_t(v);
            else if (k == "de2") r.de2 = uint16_t(v); else if (k == "hl2") r.hl2 = uint16_t(v);
            else if (k == "ix")  r.ix  = uint16_t(v); else if (k == "iy")  r.iy  = uint16_t(v);
            else if (k == "sp")  r.sp  = uint16_t(v); else if (k == "pc")  r.pc  = uint16_t(v);
            else if (k == "i")   r.i   = uint8_t(v);  else if (k == "r")   r.r   = uint8_t(v);
            else if (k == "im")  r.im  = uint8_t(v);  else if (k == "halt") r.halt = uint8_t(v);
            else if (k == "iff1") r.iff1 = uint8_t(v); else if (k == "iff2") r.iff2 = uint8_t(v);
        }
        // Writing PC mid-instruction is only sound while the core is parked.
        bool done = false;
        if (!host_->invoke([&] { be->setZ80Regs(r); done = true; })) return "err emulator not running";
        return done ? "ok" : "err failed";
    }

    if (cmd == "readz80") {
        const uint32_t a = parseU32(arg());
        const uint32_t n = parseU32(arg(), 10);
        if (n == 0 || n > (1u << 16)) return "err bad size";
        const auto d = be->readZ80Memory(uint16_t(a), uint16_t(n));
        return "ok " + toHex(d.data(), d.size());
    }

    if (cmd == "writeregion") {
        const int id     = (int)parseU32(arg(), 10);
        const uint32_t o = parseU32(arg());
        const auto d = fromHex(arg());
        if (d.empty()) return "err no data";
        return be->writeRegion(id, o, d.data(), (uint32_t)d.size()) ? "ok" : "err write failed";
    }

    if (cmd == "setvdpreg") {
        const int idx = (int)parseU32(arg(), 10);
        be->setVdpReg(idx, uint8_t(parseU32(arg())));
        return "ok";
    }

    if (cmd == "callstack") {
        const Cpu cpu = (arg() == "z80") ? Cpu::Z80 : Cpu::M68K;
        std::ostringstream o;
        o << "ok";
        for (uint32_t ea : be->getCallstack(cpu)) o << " " << std::hex << ea;
        return o.str();
    }

    if (cmd == "sound") {
        const SoundState st = be->getSoundState();
        std::ostringstream o;
        o << "ok fm0=" << toHex(st.fm[0], 256) << " fm1=" << toHex(st.fm[1], 256) << " psg=";
        for (int i = 0; i < 8; ++i) o << (i ? "," : "") << std::dec << st.psg[i];
        return o.str();
    }

    if (cmd == "getpad") {
        const int port = (int)parseU32(arg(), 10);
        std::ostringstream o; o << "ok " << std::hex << be->getPad(port);
        return o.str();
    }

    if (cmd == "pause")  { be->pause();    return "ok"; }
    if (cmd == "resume") { be->resume();   return "ok"; }
    // optional trailing "z80" selects the sound CPU
    if (cmd == "stepi" || cmd == "stepo") {
        const Cpu cpu = (arg() == "z80") ? Cpu::Z80 : Cpu::M68K;
        if (cmd == "stepi") be->stepInto(cpu); else be->stepOver(cpu);
        return "ok";
    }

    if (cmd == "pad") {
        const uint16_t mask = (uint16_t)parseU32(arg());
        const std::string p = arg();
        be->setPad(p.empty() ? 0 : (int)parseU32(p, 10), mask);
        return "ok";
    }

    if (cmd == "bpadd") {
        Breakpoint bp;
        const std::string t = arg();
        bp.type  = (t == "r") ? BpType::Read : (t == "w") ? BpType::Write : BpType::PC;
        bp.start = parseU32(arg());
        bp.end   = parseU32(arg());
        if (bp.end < bp.start) bp.end = bp.start;
        // Optional key=value tail. cpu and vdp are not cosmetic: matchBreakpoint
        // requires both to agree or the breakpoint silently never fires.
        for (std::string kv; is >> kv; ) {
            const size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if      (k == "cpu")   bp.cpu     = (v == "z80") ? Cpu::Z80 : Cpu::M68K;
            else if (k == "vdp")   bp.is_vdp  = (v != "0");
            else if (k == "elang") bp.elang   = parseU32(v, 10);
            else if (k == "cond") {           // last: conditions contain spaces
                std::string rest;
                std::getline(is, rest);
                bp.condition = v + rest;
                break;
            }
        }
        return "ok " + std::to_string(be->addBreakpoint(bp));
    }

    if (cmd == "bpdel")  { be->removeBreakpoint((int)parseU32(arg(), 10)); return "ok"; }
    if (cmd == "bpclear"){ be->clearBreakpoints(); return "ok"; }

    if (cmd == "bplist") {
        std::ostringstream o;
        o << "ok";
        for (const auto& b : be->getBreakpoints())
            o << " " << b.id << ","
              << (b.type == BpType::PC ? "x" : b.type == BpType::Read ? "r" : "w") << ","
              << std::hex << b.start << "," << b.end << std::dec
              << "," << (b.cpu == Cpu::Z80 ? "z80" : "m68k")
              << "," << (b.is_vdp ? 1 : 0) << "," << (b.enabled ? 1 : 0);
        return o.str();
    }

    // Whole-machine operations: must not race the core.
    if (cmd == "savestate" || cmd == "loadstate") {
        std::string path;
        std::getline(is, path);
        while (!path.empty() && (path.front() == ' ' || path.front() == '\t')) path.erase(0, 1);
        if (path.empty()) return "err no path";
        const bool save = (cmd == "savestate");
        bool okFlag = false;
        if (!host_->invoke([&] { okFlag = save ? be->saveState(path.c_str())
                                               : be->loadState(path.c_str()); }))
            return "err emulator not running";
        return okFlag ? "ok" : "err state io failed";
    }

    if (cmd == "frame") {
        // Visible viewport as raw RGB565, base64. The Python side turns it
        // into a PNG so this stays dependency-free.
        const VdpState v = be->getVdpState();
        (void)v;
        int w = 0, h = 0;
        std::vector<uint8_t> rgb;
        if (!host_->invoke([&] { host_->copyViewport(rgb, w, h); }))
            return "err emulator not running";
        if (rgb.empty()) return "err no frame";
        std::ostringstream o;
        o << "ok " << w << " " << h << " " << base64(rgb.data(), rgb.size());
        return o.str();
    }

    return "err unknown command";
}
