#pragma once
#include "DebugApi.h"
#include <condition_variable>
#include <deque>
#include <mutex>

// In-process transport: the emulator runs on its own thread inside the client
// process (standalone Qt app or an IDA plugin that statically links the
// emulator). Two mutex-guarded queues; no serialization needed.
class InProcTransport final : public IDebugTransport {
public:
    // emulator side
    bool sendEvent(const DebugEvent& e) override {
        { std::lock_guard<std::mutex> lk(evMx_); events_.push_back(e); }
        evCv_.notify_one();
        return true;
    }
    bool recvCommand(DebugCommand& c) override {
        std::lock_guard<std::mutex> lk(cmdMx_);
        if (commands_.empty()) return false;
        c = std::move(commands_.front());
        commands_.pop_front();
        return true;
    }

    // client side
    bool sendCommand(const DebugCommand& c) override {
        { std::lock_guard<std::mutex> lk(cmdMx_); commands_.push_back(c); }
        cmdCv_.notify_one();
        return true;
    }
    bool recvEvent(DebugEvent& e) override {
        std::lock_guard<std::mutex> lk(evMx_);
        if (events_.empty()) return false;
        e = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    // blocking waits (optional helpers for hosts without their own event loop)
    bool waitEvent(DebugEvent& e, int timeoutMs);
    bool waitCommand(DebugCommand& c, int timeoutMs);

    bool isConnected() const override { return true; }

private:
    std::mutex cmdMx_, evMx_;
    std::condition_variable cmdCv_, evCv_;
    std::deque<DebugCommand> commands_;
    std::deque<DebugEvent>   events_;
};
