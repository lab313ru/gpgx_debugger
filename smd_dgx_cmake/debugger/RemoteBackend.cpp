#include "RemoteBackend.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

#include "SocketCompat.h"

namespace {

uint32_t hexU32(const std::string& s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 16)); }

std::string toHex(const uint8_t* d, size_t n)
{
    static const char* k = "0123456789abcdef";
    std::string o; o.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) { o += k[d[i] >> 4]; o += k[d[i] & 15]; }
    return o;
}

std::vector<uint8_t> fromHex(const std::string& s)
{
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> o;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0) break;
        o.push_back(uint8_t((hi << 4) | lo));
    }
    return o;
}

// "a=1 b=2" -> map
std::vector<std::pair<std::string, std::string>> kvPairs(const std::string& s)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream is(s);
    for (std::string tok; is >> tok; ) {
        const size_t eq = tok.find('=');
        if (eq != std::string::npos) out.emplace_back(tok.substr(0, eq), tok.substr(eq + 1));
    }
    return out;
}

} // namespace

RemoteBackend::~RemoteBackend() { disconnect(); }

// A ping reply parsed into an identity. Empty pid means "not one of ours".
static SessionInfo parsePing(const std::string& reply, unsigned short port)
{
    SessionInfo si;
    if (reply.rfind("smd_dgx", 0) != 0) return si;      // someone else's socket
    si.port = port;
    si.pid  = 1;   // provisional: an old host answers "smd_dgx" with no fields
    for (const auto& kv : kvPairs(reply)) {
        if      (kv.first == "pid")    si.pid  = uint32_t(std::strtoul(kv.second.c_str(), nullptr, 10));
        else if (kv.first == "crc")    si.crc  = uint16_t(hexU32(kv.second));
        else if (kv.first == "serial") si.serial = kv.second;
        else if (kv.first == "name")   si.name   = kv.second;
    }
    return si;
}

std::vector<SessionInfo> RemoteBackend::discover()
{
    std::vector<SessionInfo> found;
    if (!sockcompat::netInit()) return found;

    for (unsigned p = kBasePort; p < kBasePort + kPortRange; ++p) {
        socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd == BAD_SOCK) continue;
        sockcompat::suppressSigpipe(fd);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons((unsigned short)p);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

        // Loopback: a refused connection comes back immediately, so scanning
        // the whole range costs milliseconds and needs no timeout juggling.
        if (::connect(fd, (sockaddr*)&addr, sizeof addr) != 0) { CLOSESOCK(fd); continue; }

        const char* req = "ping\n";
        std::string rx;
        if (sockcompat::sendAll(fd, req, 5)) {
            char buf[512];
            const int n = ::recv(fd, buf, sizeof buf - 1, 0);
            if (n > 0) rx.assign(buf, n);
        }
        CLOSESOCK(fd);

        const size_t nl = rx.find('\n');
        if (nl != std::string::npos) rx.resize(nl);
        if (rx.rfind("ok ", 0) != 0) continue;

        SessionInfo si = parsePing(rx.substr(3), (unsigned short)p);
        if (si.pid) found.push_back(std::move(si));
    }
    return found;
}

bool RemoteBackend::connect(const char* host, unsigned short port)
{
    disconnect();
    if (!sockcompat::netInit()) return false;

    // Port 0: find one. With several emulators up this picks the lowest, which
    // is stable and predictable; a caller that cares which game it gets should
    // call discover() and pass the port it wants.
    if (port == 0) {
        const auto sessions = discover();
        if (sessions.empty()) return false;
        port = sessions.front().port;
    }

    socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == BAD_SOCK) return false;
    sockcompat::suppressSigpipe(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    ::inet_pton(AF_INET, host, &addr.sin_addr);

    if (::connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
        CLOSESOCK(fd);
        return false;
    }
    fd_ = static_cast<long long>(fd);
    rxbuf_.clear();

    const std::string pong = call("ping");
    if (pong.empty()) { disconnect(); return false; }
    session_ = parsePing(pong, port);
    refreshStatus();
    return true;
}

void RemoteBackend::disconnect()
{
    std::lock_guard<std::mutex> lk(mx_);
    if (fd_ >= 0) { CLOSESOCK(static_cast<socket_t>(fd_)); fd_ = -1; }
    running_.store(false);
}

std::string RemoteBackend::call(const std::string& line)
{
    std::lock_guard<std::mutex> lk(mx_);
    if (fd_ < 0) return {};
    const socket_t fd = static_cast<socket_t>(fd_);

    std::string req = line;
    req += '\n';
    if (!sockcompat::sendAll(fd, req.data(), req.size())) {
        // Same as a failed read: the peer is gone, so stop pretending.
        CLOSESOCK(fd);
        fd_ = -1;
        running_.store(false);
        return {};
    }

    char chunk[8192];
    size_t nl;
    while ((nl = rxbuf_.find('\n')) == std::string::npos) {
        const int n = ::recv(fd, chunk, sizeof chunk, 0);
        if (n <= 0) {
            // The peer is gone. Drop the socket rather than leaving
            // isConnected() reporting health while every read quietly
            // returns zeros that look like real emulator state.
            CLOSESOCK(fd);
            fd_ = -1;
            running_.store(false);
            return {};
        }
        rxbuf_.append(chunk, n);
    }
    std::string reply = rxbuf_.substr(0, nl);
    rxbuf_.erase(0, nl + 1);
    if (!reply.empty() && reply.back() == '\r') reply.pop_back();

    if (reply.rfind("ok", 0) != 0) return {};        // "err ..." or garbage
    return reply.size() > 3 ? reply.substr(3) : std::string();
}

void RemoteBackend::refreshStatus()
{
    const std::string r = call("status");
    if (r.empty()) return;
    for (const auto& kv : kvPairs(r)) {
        if      (kv.first == "running") running_.store(kv.second != "0");
        else if (kv.first == "paused")  paused_.store(kv.second != "0");
    }
}

bool RemoteBackend::pollEvent(RemoteEvent& out)
{
    const std::string r = call("events 1");
    if (r.empty()) return false;

    std::istringstream is(r);
    int n = 0;
    is >> n;
    if (n <= 0) return false;

    std::string tuple;
    is >> tuple;
    // seq,type,pc,cpu
    std::vector<std::string> f;
    for (size_t p = 0; p <= tuple.size(); ) {
        const size_t c = tuple.find(',', p);
        f.push_back(tuple.substr(p, c == std::string::npos ? std::string::npos : c - p));
        if (c == std::string::npos) break;
        p = c + 1;
    }
    if (f.size() < 4) return false;

    const std::string& t = f[1];
    if      (t == "paused")  out.type = DebugEvent::Type::Paused;
    else if (t == "resumed") out.type = DebugEvent::Type::Resumed;
    else if (t == "started") out.type = DebugEvent::Type::Started;
    else if (t == "stopped") out.type = DebugEvent::Type::Stopped;
    else return false;                                // "dropped" and friends

    out.pc  = hexU32(f[2]);
    out.cpu = (f[3] == "z80") ? Cpu::Z80 : Cpu::M68K;

    // Keep the cached run state in step, and tell whoever is listening.
    if (out.type == DebugEvent::Type::Paused) {
        paused_.store(true);
        if (pauseCb_) pauseCb_(out.pc, out.cpu);
    } else if (out.type == DebugEvent::Type::Resumed) {
        paused_.store(false);
        if (resumeCb_) resumeCb_();
    } else if (out.type == DebugEvent::Type::Stopped) {
        running_.store(false);
    }
    return true;
}

// ---------------------------------------------------------------------------
// registers
// ---------------------------------------------------------------------------
M68kRegs RemoteBackend::getM68kRegs()
{
    M68kRegs r{};
    for (const auto& kv : kvPairs(call("regs68k"))) {
        const std::string& k = kv.first;
        const uint32_t v = hexU32(kv.second);
        if (k.size() == 2 && (k[0] == 'd' || k[0] == 'a') && k[1] >= '0' && k[1] <= '7')
            (k[0] == 'd' ? r.d : r.a)[k[1] - '0'] = v;
        else if (k == "pc")  r.pc  = v;
        else if (k == "sr")  r.sr  = v;
        else if (k == "usp") r.usp = v;
        else if (k == "isp") r.isp = v;
    }
    return r;
}

void RemoteBackend::setM68kRegs(const M68kRegs& r)
{
    std::ostringstream o;
    o << "wreg68k" << std::hex;
    for (int i = 0; i < 8; ++i) o << " d" << i << "=" << r.d[i];
    for (int i = 0; i < 8; ++i) o << " a" << i << "=" << r.a[i];
    o << " pc=" << r.pc << " sr=" << r.sr << " usp=" << r.usp << " isp=" << r.isp;
    call(o.str());
}

Z80Regs RemoteBackend::getZ80Regs()
{
    Z80Regs r{};
    for (const auto& kv : kvPairs(call("regsz80"))) {
        const std::string& k = kv.first;
        const uint32_t v = hexU32(kv.second);
        if      (k == "af")  r.af  = uint16_t(v); else if (k == "bc")  r.bc  = uint16_t(v);
        else if (k == "de")  r.de  = uint16_t(v); else if (k == "hl")  r.hl  = uint16_t(v);
        else if (k == "af2") r.af2 = uint16_t(v); else if (k == "bc2") r.bc2 = uint16_t(v);
        else if (k == "de2") r.de2 = uint16_t(v); else if (k == "hl2") r.hl2 = uint16_t(v);
        else if (k == "ix")  r.ix  = uint16_t(v); else if (k == "iy")  r.iy  = uint16_t(v);
        else if (k == "sp")  r.sp  = uint16_t(v); else if (k == "pc")  r.pc  = uint16_t(v);
        else if (k == "i")   r.i   = uint8_t(v);  else if (k == "r")   r.r   = uint8_t(v);
        else if (k == "im")  r.im  = uint8_t(v);  else if (k == "halt") r.halt = uint8_t(v);
        else if (k == "iff1") r.iff1 = uint8_t(v); else if (k == "iff2") r.iff2 = uint8_t(v);
        else if (k == "bank") r.bank = v;
    }
    return r;
}

void RemoteBackend::setZ80Regs(const Z80Regs& r)
{
    std::ostringstream o;
    o << "wregz80" << std::hex
      << " af=" << r.af  << " bc=" << r.bc  << " de=" << r.de  << " hl=" << r.hl
      << " af2=" << r.af2 << " bc2=" << r.bc2 << " de2=" << r.de2 << " hl2=" << r.hl2
      << " ix=" << r.ix  << " iy=" << r.iy  << " sp=" << r.sp  << " pc=" << r.pc
      << " i=" << unsigned(r.i) << " r=" << unsigned(r.r) << " im=" << unsigned(r.im)
      << " iff1=" << unsigned(r.iff1) << " iff2=" << unsigned(r.iff2)
      << " halt=" << unsigned(r.halt);
    call(o.str());
}

VdpState RemoteBackend::getVdpState()
{
    VdpState v{};
    // No raw pointers over a socket: the graphics views need bulk memory, which
    // they get through readRegion. Only the scalars cross.
    for (const auto& kv : kvPairs(call("vdp"))) {
        if (kv.first == "reg") {
            const auto b = fromHex(kv.second);
            std::memcpy(v.reg, b.data(), b.size() < sizeof(v.reg) ? b.size() : sizeof(v.reg));
        }
        else if (kv.first == "status")   v.status   = uint16_t(hexU32(kv.second));
        else if (kv.first == "dma_len")  v.dma_len  = hexU32(kv.second);
        else if (kv.first == "dma_src")  v.dma_src  = hexU32(kv.second);
        else if (kv.first == "dma_type") v.dma_type = uint8_t(std::strtoul(kv.second.c_str(), nullptr, 10));
    }
    return v;
}

void RemoteBackend::setVdpReg(int idx, uint8_t value)
{
    std::ostringstream o;
    o << "setvdpreg " << std::dec << idx << " " << std::hex << unsigned(value);
    call(o.str());
}

SoundState RemoteBackend::getSoundState()
{
    SoundState s{};
    for (const auto& kv : kvPairs(call("sound"))) {
        if (kv.first == "fm0" || kv.first == "fm1") {
            const auto b = fromHex(kv.second);
            const int bank = (kv.first == "fm0") ? 0 : 1;
            std::memcpy(s.fm[bank], b.data(), b.size() < 256 ? b.size() : 256);
        } else if (kv.first == "psg") {
            std::istringstream is(kv.second);
            for (int i = 0; i < 8 && is; ++i) {
                std::string n;
                if (!std::getline(is, n, ',')) break;
                s.psg[i] = std::atoi(n.c_str());
            }
        }
    }
    return s;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------
std::vector<uint8_t> RemoteBackend::readMemory(uint32_t addr, uint32_t size)
{
    std::ostringstream o;
    o << "read " << std::hex << addr << " " << std::dec << size;
    return fromHex(call(o.str()));
}

bool RemoteBackend::writeMemory(uint32_t addr, const uint8_t* data, uint32_t size)
{
    std::ostringstream o;
    o << "write " << std::hex << addr << " " << toHex(data, size);
    return !call(o.str()).empty() || true;   // "ok" with no payload is an empty string
}

std::vector<uint8_t> RemoteBackend::readZ80Memory(uint16_t addr, uint16_t size)
{
    std::ostringstream o;
    o << "readz80 " << std::hex << addr << " " << std::dec << size;
    return fromHex(call(o.str()));
}

std::vector<MemRegion> RemoteBackend::getMemRegions()
{
    std::vector<MemRegion> out;
    std::istringstream is(call("regions"));
    for (std::string tok; is >> tok; ) {
        // id:name:base:size:writable — the name may not contain ':'
        std::vector<std::string> f;
        for (size_t p = 0; p <= tok.size(); ) {
            const size_t c = tok.find(':', p);
            f.push_back(tok.substr(p, c == std::string::npos ? std::string::npos : c - p));
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (f.size() < 5) continue;
        MemRegion m;
        m.id = std::atoi(f[0].c_str());
        m.name = f[1];
        m.base = hexU32(f[2]);
        m.size = hexU32(f[3]);
        m.writable = (f[4] != "0");
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<uint8_t> RemoteBackend::readRegion(int id, uint32_t off, uint32_t size)
{
    std::ostringstream o;
    o << "readregion " << std::dec << id << " " << std::hex << off << " " << std::dec << size;
    return fromHex(call(o.str()));
}

bool RemoteBackend::writeRegion(int id, uint32_t off, const uint8_t* data, uint32_t size)
{
    std::ostringstream o;
    o << "writeregion " << std::dec << id << " " << std::hex << off << " " << toHex(data, size);
    call(o.str());
    return true;
}

// ---------------------------------------------------------------------------
// control
// ---------------------------------------------------------------------------
bool RemoteBackend::saveState(const char* path) { call(std::string("savestate ") + path); return true; }
bool RemoteBackend::loadState(const char* path) { call(std::string("loadstate ") + path); return true; }

bool RemoteBackend::runSafely(const std::function<void()>& fn)
{
    // The far side already serialises anything that touches the core; there is
    // nothing local to protect.
    if (fn) fn();
    return true;
}

void RemoteBackend::setPad(int port, uint16_t buttons)
{
    std::ostringstream o;
    o << "pad " << std::hex << buttons << " " << std::dec << port;
    call(o.str());
}

uint16_t RemoteBackend::getPad(int port)
{
    std::ostringstream o;
    o << "getpad " << std::dec << port;
    return uint16_t(hexU32(call(o.str())));
}

std::vector<uint32_t> RemoteBackend::getCallstack(Cpu cpu)
{
    std::vector<uint32_t> out;
    std::istringstream is(call(cpu == Cpu::Z80 ? "callstack z80" : "callstack"));
    for (std::string tok; is >> tok; ) out.push_back(hexU32(tok));
    return out;
}

void RemoteBackend::setConditionEvaluator(ConditionEval)
{
    // Not possible over this transport: the far side would have to call *us*
    // mid-instruction, and the protocol is strictly client-initiated. Remote
    // breakpoints are therefore unconditional — the owning host evaluates its
    // own. Silently ignored rather than pretending to install it.
}

int RemoteBackend::addBreakpoint(const Breakpoint& bp)
{
    std::ostringstream o;
    o << "bpadd " << (bp.type == BpType::PC ? "x" : bp.type == BpType::Read ? "r" : "w")
      << " " << std::hex << bp.start << " " << bp.end
      << " cpu=" << (bp.cpu == Cpu::Z80 ? "z80" : "m68k")
      << " vdp=" << (bp.is_vdp ? 1 : 0);
    const std::string r = call(o.str());
    return r.empty() ? -1 : std::atoi(r.c_str());
}

void RemoteBackend::removeBreakpoint(int id)
{
    std::ostringstream o;
    o << "bpdel " << std::dec << id;
    call(o.str());
}

void RemoteBackend::clearBreakpoints() { call("bpclear"); }

std::vector<Breakpoint> RemoteBackend::getBreakpoints()
{
    std::vector<Breakpoint> out;
    std::istringstream is(call("bplist"));
    for (std::string tok; is >> tok; ) {
        std::vector<std::string> f;
        for (size_t p = 0; p <= tok.size(); ) {
            const size_t c = tok.find(',', p);
            f.push_back(tok.substr(p, c == std::string::npos ? std::string::npos : c - p));
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (f.size() < 4) continue;
        Breakpoint b;
        b.id    = std::atoi(f[0].c_str());
        b.type  = (f[1] == "r") ? BpType::Read : (f[1] == "w") ? BpType::Write : BpType::PC;
        b.start = hexU32(f[2]);
        b.end   = hexU32(f[3]);
        if (f.size() >= 7) {
            b.cpu     = (f[4] == "z80") ? Cpu::Z80 : Cpu::M68K;
            b.is_vdp  = (f[5] != "0");
            b.enabled = (f[6] != "0");
        }
        out.push_back(b);
    }
    return out;
}

void RemoteBackend::pause()  { call("pause");  paused_.store(true);  }
void RemoteBackend::resume() { call("resume"); paused_.store(false); }
void RemoteBackend::stepInto(Cpu cpu) { call(cpu == Cpu::Z80 ? "stepi z80" : "stepi"); }
void RemoteBackend::stepOver(Cpu cpu) { call(cpu == Cpu::Z80 ? "stepo z80" : "stepo"); }

bool RemoteBackend::loadRom(const char*)
{
    return false;   // the emulator belongs to the host we attached to
}
