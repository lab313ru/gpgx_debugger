#include "InProcTransport.h"
#include <chrono>

bool InProcTransport::waitEvent(DebugEvent& e, int timeoutMs)
{
    std::unique_lock<std::mutex> lk(evMx_);
    if (!evCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                        [this] { return !events_.empty(); }))
        return false;
    e = std::move(events_.front());
    events_.pop_front();
    return true;
}

bool InProcTransport::waitCommand(DebugCommand& c, int timeoutMs)
{
    std::unique_lock<std::mutex> lk(cmdMx_);
    if (!cmdCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                         [this] { return !commands_.empty(); }))
        return false;
    c = std::move(commands_.front());
    commands_.pop_front();
    return true;
}
