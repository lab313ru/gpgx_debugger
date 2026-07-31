#pragma once
#include "IDebugBackend.h"
#include "DebugApi.h"       // DebugEvent::Type — the same vocabulary, over a wire

#include <atomic>
#include <mutex>
#include <string>

// An IDebugBackend that lives on the other end of a BridgeServer socket.
//
// This is what makes a second IDA instance possible: the database holding the
// Z80 driver attaches to the emulator the 68000 database is already running,
// instead of starting a second one. One machine, two debuggers — which is the
// point, because on a Mega Drive the music is usually sequenced by the main
// CPU and played by the Z80, and watching only one half explains nothing.
//
// Everything is a synchronous request/response round trip on one socket, so
// calls are serialised by a mutex. Reads are cheap enough at debugger rates;
// the emulator is stopped for most of them anyway.
class RemoteBackend final : public IDebugBackend {
public:
    RemoteBackend() = default;
    ~RemoteBackend() override;

    // Port 0 means: find one. See discover().
    bool connect(const char* host = "127.0.0.1", unsigned short port = 0);

    // Every emulator answering on the loopback scan range, with the game each
    // one is running. Scanning beats a file of published ports: a file left
    // behind by a crashed session lies, and a socket that answers cannot.
    static std::vector<SessionInfo> discover();

    // Same range the server binds within; kept here so a client can scan it
    // without depending on the server's header.
    static constexpr unsigned short kBasePort  = 27042;
    static constexpr unsigned short kPortRange = 16;

    // Who we actually reached. Valid after a successful connect().
    const SessionInfo& session() const { return session_; }
    void disconnect();
    bool isConnected() const { return fd_ >= 0; }

    // Drain queued events. Returns false when nothing is pending.
    struct RemoteEvent { DebugEvent::Type type; Cpu cpu; uint32_t pc; };
    bool pollEvent(RemoteEvent& out);

    // --- IDebugBackend ---
    M68kRegs   getM68kRegs() override;
    void       setM68kRegs(const M68kRegs&) override;
    Z80Regs    getZ80Regs() override;
    void       setZ80Regs(const Z80Regs&) override;
    VdpState   getVdpState() override;
    void       setVdpReg(int idx, uint8_t value) override;
    SoundState getSoundState() override;

    std::vector<uint8_t> readMemory(uint32_t addr, uint32_t size) override;
    bool                 writeMemory(uint32_t addr, const uint8_t* data, uint32_t size) override;
    std::vector<uint8_t> readZ80Memory(uint16_t addr, uint16_t size) override;

    std::vector<MemRegion> getMemRegions() override;
    std::vector<uint8_t>   readRegion(int id, uint32_t off, uint32_t size) override;
    bool                   writeRegion(int id, uint32_t off, const uint8_t* data, uint32_t size) override;

    bool saveState(const char* path) override;
    bool loadState(const char* path) override;
    bool runSafely(const std::function<void()>& fn) override;

    void     setPad(int port, uint16_t buttons) override;
    uint16_t getPad(int port) override;

    std::vector<uint32_t> getCallstack(Cpu cpu) override;
    void setConditionEvaluator(ConditionEval) override;

    int  addBreakpoint(const Breakpoint&) override;
    void removeBreakpoint(int id) override;
    void clearBreakpoints() override;
    std::vector<Breakpoint> getBreakpoints() override;

    void pause() override;
    void resume() override;
    void stepInto(Cpu cpu) override;
    void stepOver(Cpu cpu) override;
    bool isPaused() const override { return paused_.load(); }

    void onPaused(PauseCb cb)  override { pauseCb_  = std::move(cb); }
    void onResumed(ResumeCb cb) override { resumeCb_ = std::move(cb); }

    bool loadRom(const char* path) override;      // the host owns the ROM: always false
    bool isRunning() const override { return running_.load(); }

private:
    // Returns the reply with its "ok " stripped, or an empty string on error.
    std::string call(const std::string& line);
    void refreshStatus();

    std::mutex  mx_;
    long long   fd_ = -1;
    std::string rxbuf_;
    SessionInfo session_;

    // Cached from the last status/event so the const accessors stay cheap.
    std::atomic<bool> paused_  { false };
    std::atomic<bool> running_ { false };

    PauseCb  pauseCb_;
    ResumeCb resumeCb_;

    // Local mirror: the far side reports breakpoints, but IDA asks us to
    // delete by id, and the ids come from there.
    std::vector<Breakpoint> bpMirror_;
};
