#pragma once
//
// A running emulator, paused on demand, with a timeout on every wait.
//
// Run control is asynchronous: pause() and stepInto() set a flag and the
// emulation thread reports back. A test that waits without a deadline turns a
// regression into a hung CI job, so every wait here fails loudly instead.

#include "test_harness.h"
#include "synth_rom.h"

#include "debugger/EmuHost.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace t {

class Emu {
public:
    bool start()
    {
        if (!rom_.ok()) return false;
        host_.addEventSink([this](const DebugEvent& ev) {
            std::lock_guard<std::mutex> lk(mx_);
            if (ev.type == DebugEvent::Type::Paused) {
                paused_ = true;
                pausePc_ = ev.pc;
                pauseCpu_ = ev.cpu;
                ++pauseSeq_;
            } else if (ev.type == DebugEvent::Type::Resumed) {
                paused_ = false;
            }
            cv_.notify_all();
        });
        return host_.start(rom_.path());
    }

    ~Emu() { host_.stop(); }

    EmuHost*       host()    { return &host_; }
    IDebugBackend* backend() { return host_.backend(); }

    // Wait for a pause event newer than `since`. Returns false on timeout.
    bool waitPause(uint64_t since, int ms = 5000)
    {
        std::unique_lock<std::mutex> lk(mx_);
        return cv_.wait_for(lk, std::chrono::milliseconds(ms),
                            [&] { return pauseSeq_ > since; });
    }

    uint64_t pauseSeq() { std::lock_guard<std::mutex> lk(mx_); return pauseSeq_; }
    uint32_t pausePc()  { std::lock_guard<std::mutex> lk(mx_); return pausePc_; }
    Cpu      pauseCpu() { std::lock_guard<std::mutex> lk(mx_); return pauseCpu_; }

    bool pauseAndWait(int ms = 5000)
    {
        const uint64_t s = pauseSeq();
        backend()->pause();
        return waitPause(s, ms);
    }

    bool stepAndWait(Cpu cpu, int ms = 5000)
    {
        const uint64_t s = pauseSeq();
        backend()->stepInto(cpu);
        return waitPause(s, ms);
    }

    bool stepOverAndWait(Cpu cpu, int ms = 5000)
    {
        const uint64_t s = pauseSeq();
        backend()->stepOver(cpu);
        return waitPause(s, ms);
    }

    // Park the 68000 on a known instruction so a test does not depend on where
    // the free-running loop happened to be when it was interrupted.
    void parkM68kAt(uint32_t pc)
    {
        M68kRegs r = backend()->getM68kRegs();
        r.pc   = pc;
        r.a[7] = synth::kInitSp;
        backend()->setM68kRegs(r);
    }

private:
    synth::RomFile rom_;
    EmuHost        host_;

    std::mutex              mx_;
    std::condition_variable cv_;
    bool     paused_   = false;
    uint32_t pausePc_  = 0;
    Cpu      pauseCpu_ = Cpu::M68K;
    uint64_t pauseSeq_ = 0;
};

} // namespace t
