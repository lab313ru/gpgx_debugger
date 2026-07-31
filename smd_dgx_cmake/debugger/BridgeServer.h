#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class EmuHost;

// Localhost control socket over an EmuHost. Exists so something outside this
// process — the Python MCP server, or a second IDA instance holding the Z80
// database — drives the *same* emulator session the user is looking at rather
// than a private copy.
//
// The wire format is deliberately dumb: one request per line, one response per
// line, "ok ..." or "err ...". No JSON library, no dependency beyond sockets;
// all structure lives in the client.
//
// Threading: one thread accepts, one thread per client. Commands are NOT
// serialised against each other — the backend guards its own state, reads are
// racy-but-benign exactly as the debug views are, and anything that must not
// race the core goes through EmuHost::invoke(). A single lock around command
// handling would be simpler but would let one client's save-state block every
// other client for the length of a frame.
//
// Events are polled, not pushed (`events`), because a line protocol has no way
// to interleave unsolicited output with responses. Each client gets its own
// queue and sees events from the moment it connected.
class BridgeServer {
public:
    explicit BridgeServer(EmuHost* host) : host_(host) {}
    ~BridgeServer() { stop(); }

    // Port 0 (the default) means: honour $SMD_DGX_PORT if it is set, otherwise
    // take the first free port from kBasePort upwards. Two emulators can then
    // run at once; port() reports which one this session actually got.
    //
    // A fixed port would be simpler, but it makes a second game silently
    // unreachable, and clients cannot tell whose game they reached.
    static constexpr unsigned short kBasePort  = 27042;
    static constexpr unsigned short kPortRange = 16;
    bool start(unsigned short port = 0);
    void stop();
    bool isRunning() const { return running_.load(); }
    unsigned short port() const { return port_; }

private:
    struct QueuedEvent {
        uint64_t seq;
        uint8_t  type;      // DebugEvent::Type
        uint8_t  cpu;       // Cpu
        uint32_t pc;
        int32_t  bpId;      // which breakpoint, or -1
    };

    struct Client {
        long long fd = -1;
        std::deque<QueuedEvent> events;
        uint64_t dropped = 0;       // overflowed events; reported once, then cleared
        // Snapshots for `snap`/`diff`, per client and per region: the classic
        // RAM-search loop, done here so an agent compares 64K of memory
        // without hauling it across the wire as ASCII hex every round.
        std::map<int, std::vector<uint8_t>> snaps;
    };

    bool bindOne(unsigned short port, long long& fdOut);
    void serve();                                   // accept loop
    void serveClient(std::shared_ptr<Client> c);    // one connection
    std::string handle(const std::string& line, Client& c);
    std::string handleEvents(Client& c, int max);
    std::string handleWait(Client& c, int timeoutMs);
    void onEmuEvent(const void* ev);                // DebugEvent, type-erased for the header

    EmuHost*          host_ = nullptr;
    std::thread       thread_;
    std::atomic<bool> running_ { false };
    std::atomic<bool> stopFlag_{ false };
    unsigned short    port_ = 0;
    long long         listenFd_ = -1;
    int               sinkId_ = -1;     // our registration on the host

    // Signalled when an event is queued, so `wait` can block instead of
    // making every client poll.
    std::condition_variable               eventCv_;
    std::mutex                            clientsMx_;
    std::vector<std::shared_ptr<Client>>  clients_;
    std::vector<std::thread>              clientThreads_;
    uint64_t                              nextSeq_ = 1;
};
