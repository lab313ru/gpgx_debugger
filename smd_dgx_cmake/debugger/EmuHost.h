#pragma once
#include "GpgxBackend.h"
#include "DebugApi.h"
#include "CoreOptions.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Headless emulator host: owns a GpgxBackend, runs the emulator on its own
// std::thread, and bridges it to a command/event transport. No Qt, no SDL, no
// audio/video — the pixel/sound sinks are optional callbacks.
//
// This is the reusable "server" glue shared by every host:
//   - standalone Qt app  : may drive the backend directly OR via a transport;
//   - IDA plugin (static) : holds the EmuHost in-process, calls backend()
//                           directly for synchronous reads (valid while paused,
//                           the emulation thread is blocked), and consumes
//                           DebugEvents to build IDA debug_event_t's.
//
// Run-control commands may arrive either by direct backend() calls (in-proc) or
// through the transport queue; both are serviced on the emulation thread, so
// the emulator core is only ever touched from one thread.
class EmuHost {
public:
    // Optional sinks. FrameSink receives each finished frame as the picture to
    // show (coreopt::Picture): width x height RGB565 pixels from the top left
    // of `data`, rows `pitch` bytes apart. Its size follows H32/H40, the
    // borders, the NTSC filter and double-field interlace, and its pixels are
    // square only by chance, so `aspect` is the width / height to show it at.
    // A host that sets no core options, the IDA plugin, gets 0 there: show
    // the pixels square. Omit the sink for a truly headless run.
    using FrameSink = std::function<void(const uint8_t* data, int width, int height, int pitch,
                                         double aspect)>;
    // Receives one frame's worth of 16-bit stereo samples. Omit for silence.
    using AudioSink = std::function<void(const int16_t* stereo, int frames)>;
    using EventSink = std::function<void(const DebugEvent&)>;

    EmuHost();
    ~EmuHost();

    // Attach a transport (e.g. InProcTransport). The host will drain commands
    // from it (between frames and during pauses) and post events to it. May be
    // null for a pure direct-access host.
    void setTransport(IDebugTransport* t) { transport_ = t; }
    void setFrameSink(FrameSink s) { frameSink_ = std::move(s); }
    void setAudioSink(AudioSink s) { audioSink_ = std::move(s); }
    // Several consumers now: the IDA plugin turns events into debug_event_t,
    // the bridge queues them for attached clients. Sinks run on the EMULATION
    // thread — keep them short and never call back into run control.
    //
    // Returns a token for removeEventSink. A sink that does not outlive this
    // host MUST remove itself: stop() emits a final Stopped event, so a sink
    // belonging to an already-deleted object is called exactly when it is most
    // certainly gone.
    int  addEventSink(EventSink s);
    void removeEventSink(int id);

    // Direct access for in-proc hosts (synchronous reads/writes; call while
    // paused). Never null after construction.
    IDebugBackend* backend() { return &backend_; }
    GpgxBackend*   gpgx()    { return &backend_; }

    // Load a ROM and start the emulation thread. Returns false if the ROM
    // failed to load (no thread is started in that case).
    bool start(const std::string& romPath);

    // ---- Core options, reset, reload ----
    //
    // For the standalone app. A host that never calls setCoreOptions runs on
    // the core's defaults — the IDA plugin does exactly that.
    //
    // Before start(): the options the next start() loads the ROM with. While
    // running, the change is queued and applied at the top of the run loop,
    // between two frames — never inside a pause, which sits in the middle of
    // a frame and of an instruction; a paused emulator applies it after it is
    // resumed and the frame ends. LIVE options take effect there; RESET ones
    // at the next reset(true), RELOAD ones at the next reload() or start().
    //
    // Returns why part of it cannot work as asked — a TMSS boot ROM switched
    // on but missing or invalid, a lock-on cartridge whose ROM file is not
    // there — or "" when all is well.
    std::string setCoreOptions(const coreopt::CoreOptions& o);
    coreopt::CoreOptions coreOptions() const;     // as last set
    // What the core runs with: coreOptions() less each change not applied
    // yet — RESET options until a hard reset, RELOAD ones until a reload or a
    // start, the rest until the next frame boundary. So
    // coreopt::needs(coreOptionsRunning(), coreOptions()) is what a UI still
    // has to ask the user for.
    coreopt::CoreOptions coreOptionsRunning() const;

    // Queued like setCoreOptions. Hard: a power cycle that keeps cartridge
    // SRAM and picks up RESET-class options. Soft: the console's reset button.
    // False when nothing is running.
    bool reset(bool hard);
    // Loads the ROM again with the current options, keeping cartridge SRAM.
    // False when nothing is running or the ROM file cannot be read now.
    bool reload();

    // Resets and reloads performed so far, to tell that a queued one is done.
    uint64_t resetCount() const { return resets_.load(); }

    // Signal the emulation thread to exit and join it.
    void stop();

    bool isRunning() const { return running_.load(); }

    // Execute a single command on the calling thread (used by the pump and by
    // hosts that want to dispatch synchronously). Returns any reply data for
    // read-style commands via the out params; control commands ignore them.
    void dispatch(const DebugCommand& cmd);

    // Run fn on the emulation thread and wait for it. Use for anything that
    // must not race the core — save states above all. Safe to call while the
    // emulator is paused: the pause pump drains these too. Returns false if
    // the emulator is not running (fn is then not executed).
    bool invoke(const std::function<void()>& fn);

    // Run n more frames, then pause. Zero cancels a pending advance.
    void advanceFrames(int n) { framesLeft_.store(n < 0 ? 0 : n); }

    // Monotonic counters a client can compare across calls.
    //
    // Without them, "resume then poll until paused" is unsound: the poll can
    // observe the pause the caller was already standing on and conclude a new
    // one has happened. A caller that remembers the stop count instead cannot
    // be fooled — and the frame count answers "how long did that take?" on its
    // own.
    uint64_t stopCount()  const { return stops_.load(); }
    uint64_t frameCount() const { return frames_.load(); }

    // A host that keeps its OWN model of run control and breakpoints installs
    // these. IDA does: its Breakpoints window and its run state are IDA's, not
    // ours, and it has no way to learn that something else changed them —
    // there is not even a "resumed" debug event to send it (idd.hpp).
    //
    // So an external client does not reach into the backend behind the host's
    // back. It asks the host to perform the mutation, the host does it through
    // its own API, and the host's normal path arrives at the backend as usual.
    // One road instead of two, so nothing can go stale and nothing can loop.
    //
    // Each returns true if it handled the request. A host that installs
    // nothing — the standalone app — falls through to the backend.
    //
    // Called on a bridge client thread. An implementation may block (IDA's
    // does, marshalling onto its main thread) but must NOT call back into
    // EmuHost::invoke: the emulation thread may itself be waiting on the host,
    // and that pair of waits is a deadlock.
    struct HostMutations {
        std::function<bool()>                        resume;
        std::function<bool()>                        pause;
        std::function<bool(const Breakpoint&, int*)> addBreakpoint;   // out: id
        std::function<bool(int)>                     removeBreakpoint;
        std::function<bool()>                        clearBreakpoints;
    };
    void setHostMutations(HostMutations m) { hostMut_ = std::move(m); }
    const HostMutations& hostMutations() const { return hostMut_; }

    // Copy the picture the frame sink is given as tightly packed RGB565
    // (w*h*2 bytes): borders, the NTSC filter's width and both interlaced
    // fields included when they are on. Call from the emulation thread — i.e.
    // through invoke() — so the framebuffer is not being redrawn underneath.
    void copyPicture(std::vector<uint8_t>& out, int& w, int& h) const;

private:
    void run(std::string romPath);
    void applyPending();
    void beforeLoad();
    void hardReset();
    void doReload();
    void installBootRomFor(const coreopt::CoreOptions& o);
    void setInCore(const coreopt::CoreOptions& o);
    void drainCommands();
    void drainTasks();
    void emitEvent(const DebugEvent& ev);   // not `emit`: clashes with Qt's macro

    GpgxBackend       backend_;
    IDebugTransport*  transport_ = nullptr;
    FrameSink         frameSink_;
    AudioSink         audioSink_;
    struct Sink { int id; EventSink fn; };
    std::mutex        sinkMx_;
    std::vector<Sink> eventSinks_;
    int               nextSinkId_ = 1;

    std::thread       thread_;
    std::atomic<bool> stopFlag_ { false };
    std::atomic<bool> running_  { false };
    std::atomic<int>  framesLeft_ { 0 };   // frame-advance countdown
    std::atomic<uint64_t> stops_  { 0 };
    std::atomic<uint64_t> frames_ { 0 };
    std::atomic<uint64_t> resets_ { 0 };
    HostMutations     hostMut_;

    // Requests for the top of the run loop, applied in the order made.
    struct Pending {
        enum class Kind { Options, Reset, Reload } kind;
        coreopt::CoreOptions opts;   // Options
        bool hard = false;           // Reset
    };
    std::string             romPath_;
    mutable std::mutex      optMx_;
    coreopt::CoreOptions    want_;              // as last set       (optMx_)
    bool                    haveOptions_ = false;                  // (optMx_)
    std::deque<Pending>     pending_;                              // (optMx_)
    // Emulation thread only (and start(), before the thread exists):
    bool                    useOptions_ = false;   // haveOptions_ as of the run
    coreopt::CoreOptions    requested_;   // the latest Options request applied
    // What the core is running with. Written there under optMx_ (setInCore),
    // so coreOptionsRunning() can read it from another thread; the emulation
    // thread itself reads it without the lock.
    coreopt::CoreOptions    inCore_;

    // Work handed to the emulation thread by invoke().
    //
    // Shared ownership, not a pointer to the caller's stack: invoke() waits
    // with a timeout, and on timeout the caller used to return while its Task
    // was still queued — the emulation thread then executed a function object
    // living in a dead stack frame (observed as a guard-page fault in a
    // foreign thread's stack). The queue and the caller now co-own the task;
    // `abandoned` cancels one the caller gave up on before it was picked up,
    // and `started` makes the caller wait out one already executing, because
    // fn's captures live on the caller's stack either way.
    struct Task { std::function<void()> fn; bool done; bool started; bool abandoned; };
    std::mutex              taskMx_;
    std::condition_variable taskCv_;
    std::deque<std::shared_ptr<Task>> tasks_;
};
