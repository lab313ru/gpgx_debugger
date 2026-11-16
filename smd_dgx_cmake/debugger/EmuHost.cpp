#include "EmuHost.h"

#include <chrono>
#include <cstring>
#include <fstream>

#include <gx/gx.hpp>

extern "C" {
#include <shared.h>       // uint8/uint16/uint32 macros — must be first
#include <system.h>       // system_frame_gen, vdp_pal
#include <vdp_ctrl.h>
}

EmuHost::EmuHost()
{
    backend_.setBeforeLoad([this] { beforeLoad(); });
}

EmuHost::~EmuHost()
{
    stop();
}

bool EmuHost::start(const std::string& romPath)
{
    stop();
    stopFlag_.store(false);

    {
        std::lock_guard<std::mutex> lk(optMx_);
        pending_.clear();             // requests made for the previous run
        useOptions_ = haveOptions_;
        requested_ = inCore_ = haveOptions_ ? want_ : coreopt::CoreOptions{};
    }
    romPath_ = romPath;

    // Load synchronously so the caller learns immediately whether the ROM is
    // valid; the thread only runs frames.
    if (!backend_.loadRom(romPath.c_str()))
        return false;

    // Anything that must not race the core (save states above all) goes
    // through the emulation thread rather than the caller's.
    backend_.setSafeExecutor([this](const std::function<void()>& fn) { return invoke(fn); });

    // While paused, service commands so run-control/reads work mid-pause.
    backend_.setPausePump([this] {
        drainCommands();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    backend_.onPaused([this](uint32_t pc, Cpu cpu) {
        DebugEvent ev;
        ev.type = DebugEvent::Type::Paused;
        ev.cpu = cpu;
        ev.pc = pc;
        ev.bpId = backend_.lastStopBreakpoint();
        ev.stopSeq = stops_.fetch_add(1) + 1;
        ev.changed = backend_.takeCodemap();
        emitEvent(ev);
    });
    backend_.onResumed([this] {
        DebugEvent ev;
        ev.type = DebugEvent::Type::Resumed;
        emitEvent(ev);
    });

    running_.store(true);
    thread_ = std::thread(&EmuHost::run, this, romPath);

    DebugEvent started;
    started.type = DebugEvent::Type::Started;
    started.pc = backend_.getM68kRegs().pc;
    emitEvent(started);
    return true;
}

void EmuHost::stop()
{
    if (!thread_.joinable()) { running_.store(false); return; }
    stopFlag_.store(true);
    // Not just resume(): the instruction right after a resume can hit the same
    // breakpoint and park the thread again before it ever re-checks stopFlag_,
    // and then this join waits forever.
    backend_.abortRunControl();
    thread_.join();
    running_.store(false);

    DebugEvent stopped;
    stopped.type = DebugEvent::Type::Stopped;
    stopped.changed = backend_.takeCodemap();
    emitEvent(stopped);
}

void EmuHost::run(std::string /*romPath*/)
{
    namespace chr = std::chrono;   // core/system.h declares a global system_clock

    auto nextFrame = chr::steady_clock::now();

    while (!stopFlag_.load()) {
        // Resets, reloads and option changes happen here and nowhere else: a
        // pause stops mid-frame and mid-instruction, where the core cannot be
        // rebuilt underneath the CPU that is executing.
        applyPending();
        if (stopFlag_.load()) break;     // a reload that could not load the ROM
        drainCommands();

        system_frame_gen(0);      // one field/frame; blocks in firePause on bp
        frames_.fetch_add(1);

        // Frame advance: run exactly N frames, then stop. An agent cannot
        // time an input by sleeping — while it is being debugged the
        // emulator is not bound to the wall clock at all.
        if (framesLeft_.load() > 0 && framesLeft_.fetch_sub(1) == 1)
            backend_.pause();

        if (frameSink_) {
            const coreopt::Picture pic = coreopt::picture(inCore_);
            frameSink_(::bitmap.data, pic.width, pic.height, ::bitmap.pitch,
                       useOptions_ ? pic.aspect : 0.0);
        }

        if (audioSink_ && snd.enabled && snd.blips[0]) {
            static int16_t audioBuf[2048 * 2];   // stereo, gx::SOUND_SAMPLES_SIZE
            const int frames = audio_update(audioBuf);
            if (frames > 0)
                audioSink_(audioBuf, frames);
        }

        // Run at console speed. Unpaced, the emulator spins a core flat out for
        // no benefit — a debugger host still wants the game in real time. A
        // pause blocks inside system_frame_gen, so re-baseline once we are
        // behind rather than bursting frames to catch up. The rate is read
        // every frame: a region or VDP mode change switches 60 Hz and 50 Hz.
        nextFrame += chr::duration_cast<chr::steady_clock::duration>(
            chr::duration<double>(1.0 / (vdp_pal ? 50.0 : 60.0)));
        const auto now = chr::steady_clock::now();
        if (nextFrame > now)
            std::this_thread::sleep_until(nextFrame);
        else
            nextFrame = now;
    }
    gx::shutdown();
}

// ---------------------------------------------------------------------------
// Core options, reset, reload
// ---------------------------------------------------------------------------

std::string EmuHost::setCoreOptions(const coreopt::CoreOptions& o)
{
    std::string problem;
    if (o.bootRom) problem = coreopt::findBootRom().problem;

    std::lock_guard<std::mutex> lk(optMx_);
    want_ = o;
    haveOptions_ = true;
    if (running_.load())
        pending_.push_back({ Pending::Kind::Options, o, false });
    return problem;
}

coreopt::CoreOptions EmuHost::coreOptions() const
{
    std::lock_guard<std::mutex> lk(optMx_);
    return want_;
}

bool EmuHost::reset(bool hard)
{
    if (!running_.load()) return false;
    std::lock_guard<std::mutex> lk(optMx_);
    pending_.push_back({ Pending::Kind::Reset, {}, hard });
    return true;
}

bool EmuHost::reload()
{
    if (!running_.load()) return false;
    if (!std::ifstream(romPath_, std::ios::binary)) return false;
    std::lock_guard<std::mutex> lk(optMx_);
    pending_.push_back({ Pending::Kind::Reload, {}, false });
    return true;
}

void EmuHost::applyPending()
{
    std::deque<Pending> ops;
    {
        std::lock_guard<std::mutex> lk(optMx_);
        if (pending_.empty()) return;
        ops.swap(pending_);
        useOptions_ = haveOptions_;
    }
    for (const Pending& op : ops) {
        if (stopFlag_.load()) return;
        switch (op.kind) {
        case Pending::Kind::Options: {
            requested_ = op.opts;
            const coreopt::CoreOptions next =
                coreopt::withClass(inCore_, op.opts, coreopt::Apply::Live);
            const bool reinit = coreopt::applyLive(inCore_, next);
            inCore_ = next;
            if (reinit) hardReset();
            break;
        }
        case Pending::Kind::Reset:
            if (op.hard) {
                hardReset();
            } else {
                coreopt::softReset();
                backend_.onSystemReset();
                resets_.fetch_add(1);
            }
            break;
        case Pending::Kind::Reload:
            doReload();
            break;
        }
    }
}

// GpgxBackend::loadRom calls this between gx::init() and the ROM load: from
// start() on the caller's thread, before the emulation thread exists, and from
// a reload on the emulation thread.
void EmuHost::beforeLoad()
{
    if (!useOptions_) return;
    coreopt::applyAtLoad(requested_);
    installBootRomFor(requested_);
    inCore_ = requested_;
}

void EmuHost::hardReset()
{
    if (useOptions_) {
        inCore_ = coreopt::withClass(inCore_, requested_, coreopt::Apply::Reset);
        coreopt::applyResetFields(inCore_);
        installBootRomFor(inCore_);
    }
    coreopt::reinitMachine();
    backend_.onSystemReset();
    resets_.fetch_add(1);
}

void EmuHost::doReload()
{
    // Checked again: if the file has gone since the request, the game being
    // played is worth more than the reload.
    if (!std::ifstream(romPath_, std::ios::binary)) return;

    // SRAM the game has written exists only in memory until shutdown, and the
    // load would replace it with whatever was last saved to disk.
    std::vector<uint8_t> keep(sram.sram, sram.sram + sizeof sram.sram);
    const bool hadSram = sram.on != 0;

    if (!backend_.loadRom(romPath_.c_str())) {
        // No cartridge is left whose SRAM gx::shutdown() should write over
        // the good copy on disk.
        sram.on = 0;
        stopFlag_.store(true);
        return;
    }
    // stop() may have abandoned run control while the load re-armed it.
    if (stopFlag_.load()) backend_.abortRunControl();
    if (hadSram && sram.on) std::memcpy(sram.sram, keep.data(), keep.size());
    resets_.fetch_add(1);
}

void EmuHost::installBootRomFor(const coreopt::CoreOptions& o)
{
    if (!o.bootRom) { coreopt::installBootRom(nullptr); return; }
    const coreopt::BootRom rom = coreopt::findBootRom();
    coreopt::installBootRom(rom.ok() ? &rom : nullptr);
}

void EmuHost::drainCommands()
{
    drainTasks();
    if (!transport_) return;
    DebugCommand cmd;
    while (transport_->recvCommand(cmd))
        dispatch(cmd);
}

void EmuHost::drainTasks()
{
    for (;;) {
        std::shared_ptr<Task> t;
        {
            std::lock_guard<std::mutex> lk(taskMx_);
            if (tasks_.empty()) return;
            t = tasks_.front();
            tasks_.pop_front();
            if (t->abandoned) continue;   // its caller timed out and returned
            t->started = true;
        }
        (t->fn)();
        {
            std::lock_guard<std::mutex> lk(taskMx_);
            t->done = true;
        }
        taskCv_.notify_all();
    }
}

bool EmuHost::invoke(const std::function<void()>& fn)
{
    if (!running_.load() || !thread_.joinable()) return false;

    auto task = std::make_shared<Task>(Task{ fn, false, false, false });
    {
        std::lock_guard<std::mutex> lk(taskMx_);
        tasks_.push_back(task);
    }
    std::unique_lock<std::mutex> lk(taskMx_);
    // The emulation thread drains tasks between frames and while paused, so
    // this completes in either state; the timeout only guards a dying host.
    if (taskCv_.wait_for(lk, std::chrono::seconds(5), [&] { return task->done; }))
        return true;

    // Timed out. fn's captures die the moment we return, so the task must
    // never touch them after that: cancel it if it is still queued, and wait
    // it out if the emulation thread is executing it at this very moment.
    if (!task->started) {
        task->abandoned = true;
        return false;
    }
    taskCv_.wait(lk, [&] { return task->done; });
    return true;
}

void EmuHost::dispatch(const DebugCommand& cmd)
{
    using Op = DebugCommand::Op;
    switch (cmd.op) {
    case Op::Pause:    backend_.pause();    break;
    case Op::Resume:   backend_.resume();   break;
    case Op::StepInto: backend_.stepInto(cmd.cpu); break;
    case Op::StepOver: backend_.stepOver(cmd.cpu); break;
    case Op::WriteMemory:
        if (!cmd.data.empty())
            backend_.writeMemory(cmd.addr, cmd.data.data(), (uint32_t)cmd.data.size());
        break;
    case Op::SetVdpReg:
        backend_.setVdpReg(cmd.index, (uint8_t)cmd.value);
        break;
    case Op::AddBreakpoint: {
        Breakpoint bp;
        bp.type   = (BpType)cmd.bpType;
        bp.cpu    = cmd.cpu;
        bp.is_vdp = cmd.bpIsVdp != 0;
        bp.start  = cmd.bpStart;
        bp.end    = cmd.bpEnd;
        bp.condition = cmd.bpCondition;
        backend_.addBreakpoint(bp);
        break;
    }
    case Op::RemoveBreakpoint: backend_.removeBreakpoint(cmd.index); break;
    case Op::ClearBreakpoints: backend_.clearBreakpoints();          break;
    case Op::ExitEmulation:    stopFlag_.store(true); backend_.resume(); break;
    // Read-style ops (GetM68kRegs/GetVdpState/ReadMemory/...) are serviced by
    // in-proc hosts via backend() directly (valid while paused). A separate
    // process transport will add a reply channel; not needed for static link.
    default: break;
    }
}

void EmuHost::copyPicture(std::vector<uint8_t>& out, int& w, int& h) const
{
    const t_bitmap& bm = ::bitmap;
    const coreopt::Picture pic = coreopt::picture(inCore_);
    w = pic.width;
    h = pic.height;
    out.clear();
    if (!bm.data || w <= 0 || h <= 0) { w = h = 0; return; }

    out.resize(static_cast<size_t>(w) * h * 2);
    for (int y = 0; y < h; ++y)
        std::memcpy(out.data() + (size_t)y * w * 2, bm.data + (size_t)y * bm.pitch, (size_t)w * 2);
}

int EmuHost::addEventSink(EventSink s)
{
    if (!s) return -1;
    std::lock_guard<std::mutex> lk(sinkMx_);
    const int id = nextSinkId_++;
    eventSinks_.push_back({ id, std::move(s) });
    return id;
}

void EmuHost::removeEventSink(int id)
{
    if (id < 0) return;
    std::lock_guard<std::mutex> lk(sinkMx_);
    for (size_t i = 0; i < eventSinks_.size(); ++i)
        if (eventSinks_[i].id == id) { eventSinks_.erase(eventSinks_.begin() + i); return; }
}

void EmuHost::emitEvent(const DebugEvent& ev)
{
    if (transport_) transport_->sendEvent(ev);
    std::vector<Sink> sinks;
    { std::lock_guard<std::mutex> lk(sinkMx_); sinks = eventSinks_; }
    for (const auto& s : sinks) s.fn(ev);   // copied: a sink may outlive the lock
}
