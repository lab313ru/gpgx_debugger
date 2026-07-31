#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "DebugState.h"   // Cpu

// ---------------------------------------------------------------------------
// Transport-agnostic debug protocol.
//
// This is the minimal command/event surface distilled from the old
// smd_ida_tools2 protobuf/gRPC protocol (DbgServer/DbgClient services).
// The protobuf layer is gone; the same payloads can be carried by:
//   - InProcTransport (std::deque + mutex)  — static linking: emulator lives
//     inside the client process (e.g. the IDA plugin) on its own thread;
//   - a future shared-memory ring            — separate processes;
//   - optionally a protobuf adapter          — legacy Gensida compatibility.
//
// Payloads deliberately keep flat/simple types so they can later be
// serialized for IPC without redesign. `changed` (the codemap) is the one
// variable-size payload: executed-pc -> predecessor-pc, flushed and cleared
// atomically with every pause/stop event, exactly like the Gens original.
// ---------------------------------------------------------------------------

struct DebugCommand {
    enum class Op : uint8_t {
        Pause, Resume, StepInto, StepOver,
        ReadMemory, WriteMemory,
        GetM68kRegs, SetM68kRegs, GetZ80Regs,
        GetVdpState, SetVdpReg, GetSoundState,
        AddBreakpoint, RemoveBreakpoint, ClearBreakpoints,
        GetCallstack,
        LoadRom, ExitEmulation,
    };
    Op       op{};
    Cpu      cpu = Cpu::M68K;   // StepInto/StepOver/AddBreakpoint target
    uint32_t addr = 0;      // ReadMemory/WriteMemory
    uint32_t size = 0;
    int      index = 0;     // SetVdpReg reg index / RemoveBreakpoint id
    uint32_t value = 0;     // SetVdpReg value
    std::vector<uint8_t> data;   // WriteMemory payload / LoadRom path bytes
    // AddBreakpoint payload (flat, mirrors DebugState.h Breakpoint)
    uint8_t  bpType = 0;
    uint8_t  bpIsVdp = 0;
    uint32_t bpStart = 0, bpEnd = 0;
    std::string bpCondition;
};

struct DebugEvent {
    enum class Type : uint8_t {
        Started,        // emulation started (ROM loaded)
        Paused,         // pc valid; changed = codemap since last event
        Resumed,
        Stopped,        // emulator exiting; changed flushed one last time
    };
    Type     type{};
    Cpu      cpu = Cpu::M68K;   // which processor stopped
    uint32_t pc  = 0;
    int      bpId = -1;         // breakpoint that caused it, or -1
    std::map<uint32_t, uint32_t> changed;   // executed pc -> predecessor pc
};

// Bidirectional transport endpoint. The emulator side sends events and
// receives commands; the client side (UI / IDA) does the reverse.
class IDebugTransport {
public:
    virtual ~IDebugTransport() = default;

    // emulator side
    virtual bool sendEvent(const DebugEvent&) = 0;
    virtual bool recvCommand(DebugCommand&) = 0;      // non-blocking; false = empty

    // client side
    virtual bool sendCommand(const DebugCommand&) = 0;
    virtual bool recvEvent(DebugEvent&) = 0;          // non-blocking; false = empty

    virtual bool isConnected() const = 0;
};
