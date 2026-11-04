// SMD DGX — IDA 9.2+ debugger plugin hosting the Genesis Plus GX emulator
// in-process (static link, emulator on its own thread).
//
// This is the phase-2 port of Gensida's ida_debug.cpp: the debugger_t /
// HT_IDD surface is kept 1:1, but every gRPC client call is replaced by a
// direct call into EmuHost/IDebugBackend (valid while the emulation thread is
// paused) and events arrive through EmuHost's EventSink instead of a gRPC
// server.
//
// Marked VERIFY-9.3 where the API must be checked against the 9.3 SDK once it
// is installed (written against the 9.x idd.hpp surface Gensida used).

#include <deque>
#include <map>
#include <mutex>
#include <string>

// IDA SDK
#include <ida.hpp>
#include <idp.hpp>
#include <idd.hpp>
#include <dbg.hpp>
#include <auto.hpp>
#include <loader.hpp>
#include <segment.hpp>
#include <expr.hpp>

// emulator (NO Qt here)
#include "debugger/EmuHost.h"
#include "debugger/BridgeServer.h"
#include "debugger/RemoteBackend.h"
#include "platform/AudioOutput.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

#include "ida_registers.h"

// ROM patching actions (ida_patch.cpp — IDA side, no Qt)
void smd_dgx_register_patches();
void smd_dgx_unregister_patches();

// Constant decoding for the listing (ida_asm.cpp)
void smd_dgx_register_asm();
void smd_dgx_unregister_asm();

// Z80-database actions: dump the live driver RAM (ida_z80_loader.cpp)
void smd_dgx_register_z80_actions();
void smd_dgx_unregister_z80_actions();

#ifdef SMD_DGX_IDA_VIEWS
// Qt-side view builder + IDA-side dock glue. Plain declarations only: this TU
// must not see Qt headers (see ida_views_shared.h).
#include "ida_views_shared.h"
#endif

#define PLUGIN_NAME "SMD DGX"
#define BREAKPOINTS_BASE 0x00D00000u   // VDP pseudo-segments: VRAM +0x00000, CRAM +0x10000, VSRAM +0x20000

// ---------------------------------------------------------------------------
// Event queue (Gensida's eventlist_t + a mutex; events are produced on the
// emulation thread and drained by IDA's debthread via ev_get_debug_event)
// ---------------------------------------------------------------------------
namespace {

struct EventList {
    std::mutex mx;
    std::deque<debug_event_t> q;

    void enqueue(const debug_event_t& ev) {
        std::lock_guard<std::mutex> lk(mx);
        q.push_back(ev);
    }
    bool retrieve(debug_event_t* out, bool* more) {
        std::lock_guard<std::mutex> lk(mx);
        if (q.empty()) return false;
        *out = q.front();
        q.pop_front();
        *more = !q.empty();
        return true;
    }
};

EventList g_events;
EmuHost*  g_host = nullptr;

// Attach mode: this database drives an emulator that is already running
// somewhere else (the standalone app, or another IDA), the way the Z80
// database always has. Exactly one of g_host / g_remote68 is non-null while
// a session is live; everything below reaches the emulator through
// cur_backend() and does not care which mode it is in.
RemoteBackend*    g_remote68 = nullptr;
std::atomic<bool> g_polling68{ false };
std::thread       g_pollThread68;

// breakpoint identity (type,start,end,is_vdp) -> backend id, needed because
// IDebugBackend deletes by id while IDA deletes by address/type
struct BpKey {
    uint8_t type; uint8_t vdp; uint32_t start; uint32_t end;
    bool operator<(const BpKey& o) const {
        return std::tie(type, vdp, start, end) < std::tie(o.type, o.vdp, o.start, o.end);
    }
};
std::map<BpKey, int> g_bpIds;

// Audio device, alive for as long as the emulator runs. Written to from the
// emulation thread only (via the audio sink), created/destroyed around it.
AudioOutput* g_audio = nullptr;

// Control socket for external agents (the MCP server). Loopback only.
BridgeServer* g_bridge = nullptr;

// Cleared on every start_process; see region_rw.
uint32_t g_romSize = 0;

// ---------------------------------------------------------------------------
// codemap -> auto_make_code on the main thread
// ---------------------------------------------------------------------------
struct apply_codemap_req : public exec_request_t {
    std::map<uint32_t, uint32_t> changed;
    explicit apply_codemap_req(std::map<uint32_t, uint32_t> c) : changed(std::move(c)) {}
    ssize_t idaapi execute() override {
        for (const auto& kv : changed) {
            auto_make_code((ea_t)kv.first);
            plan_ea((ea_t)kv.first);
        }
        return 0;
    }
};

void apply_codemap(std::map<uint32_t, uint32_t> changed)
{
    if (changed.empty()) return;
    apply_codemap_req req(std::move(changed));
    execute_sync(req, MFF_WRITE);
}

// ---------------------------------------------------------------------------
// EmuHost event -> IDA debug_event_t
// ---------------------------------------------------------------------------
void on_emu_event(const DebugEvent& ev)
{
    debug_event_t ida_ev;               // VERIFY-9.3: field/ctor surface
    ida_ev.pid = 1;
    ida_ev.tid = 1;
    ida_ev.handled = true;

    switch (ev.type) {
    case DebugEvent::Type::Started: {
        modinfo_t& mi = ida_ev.set_modinfo(PROCESS_STARTED);   // VERIFY-9.3
        ida_ev.ea = BADADDR;
        mi.name = "GPGX";
        mi.base = 0;
        mi.size = 0;
        mi.rebase_to = BADADDR;
        g_events.enqueue(ida_ev);
        // No synthetic suspend here: start_process() requests a real pause, so
        // the emulation thread reports PROCESS_SUSPENDED at the actual PC a
        // moment later. Enqueueing one as well made IDA stop twice on startup.
        break;
    }
    case DebugEvent::Type::Paused:
        // Nothing refills the device while suspended; silence it instead of
        // letting the last buffers drone on.
        if (g_audio) g_audio->pause(true);
        apply_codemap(ev.changed);
        ida_ev.set_eid(PROCESS_SUSPENDED);
        ida_ev.ea = ev.pc;
        g_events.enqueue(ida_ev);
        break;
    case DebugEvent::Type::Stopped:
        apply_codemap(ev.changed);
        ida_ev.set_exit_code(PROCESS_EXITED, 0);               // VERIFY-9.3
        ida_ev.ea = BADADDR;
        g_events.enqueue(ida_ev);
        break;
    case DebugEvent::Type::Resumed:
        if (g_audio) g_audio->pause(false);
        break;
    }
}

// ---------------------------------------------------------------------------
// Breakpoint conditions
//
// The expression is IDA's (IDC or Python), so IDA must evaluate it — and only
// on its own thread. We are called from the emulation thread while it is
// stopped at the breakpoint, so execute_sync is safe here: the emulator is not
// holding anything IDA needs.
// ---------------------------------------------------------------------------
struct eval_cond_req : public exec_request_t {
    uint32_t    elang;
    std::string expr;
    bool        result = true;      // on any failure, stop rather than skip
    eval_cond_req(uint32_t l, std::string e) : elang(l), expr(std::move(e)) {}

    ssize_t idaapi execute() override {
        const extlang_object_t el = find_extlang_by_index(int(elang));
        qstring errbuf;
        idc_value_t rv;
        bool ok = false;
        if (el != nullptr)
            ok = el->eval_expr(&rv, BADADDR, expr.c_str(), &errbuf);
        else
            ok = eval_idc_expr(&rv, BADADDR, expr.c_str(), &errbuf);   // default: IDC

        if (!ok) {
            msg(PLUGIN_NAME ": breakpoint condition failed to evaluate: %s\n",
                errbuf.empty() ? "unknown error" : errbuf.c_str());
            result = true;          // a broken condition must not hide the hit
            return 0;
        }
        result = (rv.num != 0);
        return 0;
    }
};

bool evaluate_condition(uint32_t elang, const std::string& expr)
{
    eval_cond_req req(elang, expr);
    execute_sync(req, MFF_WRITE);
    return req.result;
}


// ---------------------------------------------------------------------------
// attach mode
// ---------------------------------------------------------------------------
IDebugBackend* cur_backend()
{
    if (g_remote68) return g_remote68;
    return g_host ? g_host->backend() : nullptr;
}

// The poll thread must never wait on the main thread: the stop event is queued
// only after this returns, and IDA's main thread may itself be waiting for that
// event or for detach to join the poll thread. So pcs collect here and at most
// one no-wait request applies them on the next UI loop.
struct pending_codemap_req;
std::mutex                   g_cmMx;
std::map<uint32_t, uint32_t> g_cmPending;
pending_codemap_req*         g_cmReq = nullptr;
int                          g_cmReqId = 0;
uint64_t                     g_cmGen = 0;

struct pending_codemap_req : public exec_request_t {
    ssize_t idaapi execute() override {
        std::map<uint32_t, uint32_t> changed;
        {
            std::lock_guard<std::mutex> lk(g_cmMx);
            changed.swap(g_cmPending);
            g_cmReq = nullptr;
        }
        for (const auto& kv : changed) {
            auto_make_code((ea_t)kv.first);
            plan_ea((ea_t)kv.first);
        }
        delete this;            // a no-wait request has no other owner
        return 0;
    }
};

void post_codemap68(const std::map<uint32_t, uint32_t>& changed)
{
    if (changed.empty()) return;
    pending_codemap_req* req;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(g_cmMx);
        g_cmPending.insert(changed.begin(), changed.end());
        if (g_cmReq) return;
        g_cmReq = req = new pending_codemap_req();
        gen = ++g_cmGen;
    }
    // Outside the lock: even with MFF_NOWAIT this call waits for the main thread,
    // which may already be inside execute() wanting the same lock. req may be
    // gone by the time it returns, hence the generation rather than the pointer.
    const int id = (int)execute_sync(*req, MFF_WRITE | MFF_NOWAIT);
    std::lock_guard<std::mutex> lk(g_cmMx);
    if (g_cmReq && g_cmGen == gen) g_cmReqId = id;
}

// Unloading the plugin with a request still queued would leave IDA calling a
// vtable in a freed DLL.
void cancel_codemap68()
{
    pending_codemap_req* req;
    int id;
    {
        std::lock_guard<std::mutex> lk(g_cmMx);
        g_cmPending.clear();
        req = g_cmReq;
        id = g_cmReqId;
        g_cmReq = nullptr;
    }
    if (req && cancel_exec_request(id)) delete req;
}

// Same shape as the Z80 database's poll loop: the line protocol cannot push,
// so the far side queues and we ask every 20 ms. The one 68k-specific duty is
// the codemap — in-process it rides DebugEvent::changed, over the wire it has
// to be fetched; a pause is the only moment it can have grown.
void poll_loop68()
{
    while (g_polling68.load()) {
        RemoteBackend::RemoteEvent ev;
        bool any = false;
        while (g_remote68 && g_remote68->pollEvent(ev)) {
            any = true;
            debug_event_t ida_ev;
            ida_ev.pid = 1;
            ida_ev.tid = 1;
            ida_ev.handled = true;

            switch (ev.type) {
            case DebugEvent::Type::Paused: {
                std::map<uint32_t, uint32_t> cm;
                if (g_remote68->fetchCodemap(cm)) post_codemap68(cm);
                ida_ev.set_eid(PROCESS_SUSPENDED);
                // This database points at 68k code: on a Z80-side stop report
                // our own PC, exactly as the Z80 database reports its own.
                ida_ev.ea = (ev.cpu == Cpu::M68K) ? ev.pc
                                                  : g_remote68->getM68kRegs().pc;
                g_events.enqueue(ida_ev);
                break;
            }
            case DebugEvent::Type::Stopped:
                ida_ev.set_exit_code(PROCESS_EXITED, 0);
                ida_ev.ea = BADADDR;
                g_events.enqueue(ida_ev);
                break;
            default:
                break;          // Resumed/Started need no IDA event here
            }
        }
        if (!any)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void start_polling68()
{
    if (g_polling68.load()) return;
    g_polling68.store(true);
    g_pollThread68 = std::thread(poll_loop68);
}

void stop_polling68()
{
    if (!g_polling68.load()) return;
    g_polling68.store(false);
    if (g_pollThread68.joinable()) g_pollThread68.join();
}

// Undo an attach. Our breakpoints are removed (they are this database's, not
// the emulator's), and the game is resumed: detaching must leave the
// standalone playable, not frozen at wherever we last stopped it.
void detach68()
{
    stop_polling68();
    if (g_remote68) {
        for (const auto& kv : g_bpIds) g_remote68->removeBreakpoint(kv.second);
        if (g_remote68->isConnected()) g_remote68->resume();
        g_remote68->disconnect();
    }
    delete g_remote68;
    g_remote68 = nullptr;
    g_bpIds.clear();
}

// target: "" = the only/lowest session on the scan range; digits = a port;
// anything else = a case-insensitive substring of the running game's name.
// With several emulators running an ambiguous name is refused, not guessed —
// silently picking the first is how a session ends up debugging the wrong game.
drc_t attach68(const std::string& target, bool as_attach)
{
    stop_polling68();           // before the delete: the poll thread uses g_remote68
    delete g_remote68;
    g_remote68 = new RemoteBackend();
    g_romSize = 0;
    g_bpIds.clear();
    { std::lock_guard<std::mutex> lk(g_events.mx); g_events.q.clear(); }

    unsigned short port = 0;
    if (!target.empty()) {
        bool digits = true;
        for (char ch : target) if (ch < '0' || ch > '9') { digits = false; break; }
        if (digits) {
            port = (unsigned short)strtoul(target.c_str(), nullptr, 10);
        } else {
            auto lower = [](std::string s) {
                for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch += 32;
                return s;
            };
            const std::string want = lower(target);
            std::string seen;
            int hits = 0;
            for (const auto& s : RemoteBackend::discover()) {
                if (!seen.empty()) seen += ", ";
                seen += s.name;
                if (lower(s.name).find(want) != std::string::npos) {
                    port = s.port;
                    ++hits;
                }
            }
            if (hits != 1) {
                delete g_remote68; g_remote68 = nullptr;
                warning("SMD DGX: \"%s\" matches %d running emulators.\n\nRunning now: %s",
                        target.c_str(), hits, seen.empty() ? "none" : seen.c_str());
                return DRC_FAILED;
            }
        }
    }

    if (!g_remote68->connect("127.0.0.1", port)) {
        delete g_remote68; g_remote68 = nullptr;
        warning("SMD DGX: no emulator to attach to on 127.0.0.1:%u.\n\n"
                "Start the game in the standalone app (or another IDA) first —\n"
                "in attach mode this database does not run its own.",
                port ? port : RemoteBackend::kBasePort);
        return DRC_FAILED;
    }

    // Park the emulator before telling IDA anything, and consume its Paused
    // event here: IDA gets exactly one stop for this, whether the game was
    // running or an agent had already parked it (a parked emulator emits none).
    if (!g_remote68->isPaused()) {
        g_remote68->pause();    // marks itself paused at once: wait for the event
        bool parked = false;
        RemoteBackend::RemoteEvent drained;
        for (int i = 0; i < 100 && !parked; ++i) {
            if (g_remote68->pollEvent(drained))
                parked = drained.type == DebugEvent::Type::Paused;
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (!parked) {
            const unsigned shown = g_remote68->session().port;
            delete g_remote68; g_remote68 = nullptr;
            warning("SMD DGX: the emulator on 127.0.0.1:%u did not stop when asked.", shown);
            return DRC_FAILED;
        }
    }
    const ea_t stop_pc = g_remote68->getM68kRegs().pc & 0xFFFFFF;

    auto enqueue_module_event = [&](event_id_t id, ea_t ea) {
        debug_event_t ev;
        ev.pid = 1;
        ev.tid = 1;
        ev.handled = true;
        modinfo_t& mi = ev.set_modinfo(id);
        ev.ea = ea;
        mi.name = "GPGX";
        mi.base = 0;
        mi.size = 0;
        mi.rebase_to = BADADDR;
        g_events.enqueue(ev);
    };
    // IDA learns the process from PROCESS_STARTED in both cases, as the SDK's own
    // modules do. PROCESS_ATTACHED is itself a stop (this debugger is not
    // FAKE_ATTACH); a plain start needs the suspend spelled out.
    enqueue_module_event(PROCESS_STARTED, BADADDR);
    if (as_attach) {
        enqueue_module_event(PROCESS_ATTACHED, stop_pc);
    } else {
        debug_event_t sus;
        sus.pid = 1; sus.tid = 1; sus.handled = true;
        sus.set_eid(PROCESS_SUSPENDED);
        sus.ea = stop_pc;
        g_events.enqueue(sus);
    }

    std::map<uint32_t, uint32_t> history;
    if (g_remote68->fetchCodemap(history)) post_codemap68(history);

    start_polling68();
    msg(PLUGIN_NAME ": attached to \"%s\" on 127.0.0.1:%u\n",
        g_remote68->session().name.c_str(), g_remote68->session().port);
    return DRC_OK;
}

// ---------------------------------------------------------------------------
// Bridge mutations, performed the way IDA performs them
//
// An external client (the MCP server, the Z80 database) must not reach into the
// backend behind IDA's back: IDA owns the Breakpoints window and the run state,
// and there is no debug event that means "resumed" (idd.hpp event_id_t), so it
// would have no way to notice. Instead the bridge asks us, we do it through
// IDA's own API on IDA's own thread, and IDA arrives at the backend by its
// normal path — ev_resume, ev_update_bpts. One road, so nothing goes stale and
// nothing loops.
//
// These run on a bridge client thread and marshal with execute_sync. They must
// never call EmuHost::invoke: the emulation thread can itself be waiting on
// IDA (apply_codemap, eval_cond_req), and the two waits would deadlock. None of
// the IDA calls below touch the emulation thread.
// ---------------------------------------------------------------------------
struct run_ctl_req : public exec_request_t {
    bool wantRun;
    bool ok = false;
    explicit run_ctl_req(bool run) : wantRun(run) {}
    ssize_t idaapi execute() override {
        const int st = get_process_state();
        if (st == DSTATE_NOTASK) return 0;          // nothing being debugged
        // Only correct a real mismatch. Asking IDA to continue a process it
        // already believes is running would be a second, spurious resume.
        if (wantRun && st == DSTATE_SUSP)  ok = continue_process();
        else if (!wantRun && st == DSTATE_RUN) ok = suspend_process();
        else ok = true;                             // already where we want it
        return 0;
    }
};

bool host_resume() { run_ctl_req r(true);  execute_sync(r, MFF_WRITE); return r.ok; }
bool host_pause()  { run_ctl_req r(false); execute_sync(r, MFF_WRITE); return r.ok; }

struct add_bpt_req : public exec_request_t {
    Breakpoint bp;
    bool ok = false;
    explicit add_bpt_req(const Breakpoint& b) : bp(b) {}
    ssize_t idaapi execute() override {
        ea_t ea = bp.start;
        if (bp.is_vdp) ea += BREAKPOINTS_BASE;
        const asize_t size = bp.end >= bp.start ? (bp.end - bp.start + 1) : 1;
        const bpttype_t t = bp.type == BpType::Read  ? BPT_READ
                          : bp.type == BpType::Write ? BPT_WRITE
                                                     : BPT_EXEC;
        // add_bpt makes IDA call our ev_update_bpts, which is what actually
        // creates the backend breakpoint — so we deliberately do NOT create it
        // here as well.
        ok = add_bpt(ea, t == BPT_EXEC ? 0 : size, t);
        return 0;
    }
};

// A Z80 breakpoint has no place in a 68000 database — its addresses belong to
// a different machine — so it goes straight to the backend and IDA is not
// involved. The Z80 database keeps its own list for those.
bool host_add_bpt(const Breakpoint& bp, int* idOut)
{
    if (!g_host || bp.cpu != Cpu::M68K) return false;

    add_bpt_req req(bp);
    execute_sync(req, MFF_WRITE);
    if (!req.ok) return false;

    // ev_update_bpts has run by now and recorded the id under this key.
    uint8_t vdp = bp.is_vdp ? 1 : 0;
    auto it = g_bpIds.find(BpKey{ (uint8_t)bp.type, vdp, bp.start, bp.end });
    if (idOut) *idOut = (it != g_bpIds.end()) ? it->second : -1;
    return true;
}

struct del_bpt_req : public exec_request_t {
    ea_t ea;
    explicit del_bpt_req(ea_t a) : ea(a) {}
    ssize_t idaapi execute() override { del_bpt(ea); return 0; }
};

bool host_del_bpt(int id)
{
    if (!g_host) return false;
    // Find the address IDA knows this backend id by.
    for (const auto& kv : g_bpIds) {
        if (kv.second != id) continue;
        ea_t ea = kv.first.start;
        if (kv.first.vdp) ea += BREAKPOINTS_BASE;
        del_bpt_req req(ea);
        execute_sync(req, MFF_WRITE);
        return true;
    }
    return false;   // not one of ours (a Z80 breakpoint): let the backend do it
}

struct clear_bpts_req : public exec_request_t {
    ssize_t idaapi execute() override {
        // Walk backwards: deleting renumbers the ones after it.
        for (int i = get_bpt_qty() - 1; i >= 0; --i) {
            bpt_t b;
            if (getn_bpt(i, &b)) del_bpt(b.ea);
        }
        return 0;
    }
};

bool host_clear_bpts()
{
    if (!g_host) return false;
    clear_bpts_req req;
    execute_sync(req, MFF_WRITE);
    // IDA only knows about the 68000 ones; anything else (Z80, added straight
    // to the backend) still has to go.
    g_host->backend()->clearBreakpoints();
    return true;
}

// ---------------------------------------------------------------------------
// memory routing: side-effect-free reads via typed regions
// (never touches IO read handlers; unmapped bytes read as 0, like Gens)
// ---------------------------------------------------------------------------
ssize_t region_rw(ea_t ea, void* buf, size_t size, bool write)
{
    IDebugBackend* be = cur_backend();
    if (!be) return 0;
    uint8_t* p = (uint8_t*)buf;

    // ROM size from the region table. Session state, not a one-shot static:
    // a second ROM loaded into the same IDA session is a different size, and a
    // cached one from the previous game makes the new one read as zeros past
    // the old end — or shadow RAM that lives beyond it.
    if (!g_romSize)
        for (const auto& r : be->getMemRegions())
            if (r.id == 0) g_romSize = r.size;
    const uint32_t romSize = g_romSize;

    // Address -> (region, offset, bytes left in that region). Whole runs, not
    // single bytes: in attach mode every readRegion is a socket round trip,
    // and a hex page read byte-by-byte is a thousand of them.
    auto map_addr = [&](uint32_t a, int& region, uint32_t& off, uint32_t& left) {
        region = -1; off = 0; left = 1;
        if (a < romSize)                         { region = 0; off = a;            left = romSize - a; }
        else if (a >= 0xFF0000 && a <= 0xFFFFFF) { region = 1; off = a - 0xFF0000; left = 0x1000000 - a; }
        else if (a >= 0xA00000 && a <  0xA02000) { region = 2; off = a - 0xA00000; left = 0xA02000 - a; }
        else if (a >= BREAKPOINTS_BASE && a < BREAKPOINTS_BASE + 0x30000) {
            const uint32_t v = a - BREAKPOINTS_BASE;
            region = 3 + (int)(v >> 16);
            off    = v & 0xFFFF;
            left   = 0x10000 - off;
        }
    };

    size_t i = 0;
    while (i < size) {
        const uint32_t a = (uint32_t)(ea + i);
        int region; uint32_t off, left;
        map_addr(a, region, off, left);

        if (region < 0) {       // unmapped reads as 0, like Gens; writes vanish
            if (!write) p[i] = 0;
            ++i;
            continue;
        }
        const uint32_t run = (uint32_t)std::min<size_t>(size - i, left);
        if (write) {
            be->writeRegion(region, off, &p[i], run);
        } else {
            auto b = be->readRegion(region, off, run);
            if (b.size() < run) b.resize(run, 0);
            memcpy(&p[i], b.data(), run);
        }
        i += run;
    }
    return (ssize_t)size;
}

// ---------------------------------------------------------------------------
// registers
// ---------------------------------------------------------------------------
const char* const SRReg[] = {
    "C", "V", "Z", "N", "X", nullptr, nullptr, nullptr,
    "I", "I", "I", nullptr, nullptr, "S", nullptr, "T",
};

register_info_t registers[] = {
    { "D0", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D1", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D2", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D3", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D4", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D5", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D6", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },
    { "D7", REGISTER_ADDRESS, RC_GENERAL, dt_dword, nullptr, 0 },

    { "A0", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A0_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A1", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A1_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A2", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A2_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A3", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A3_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A4", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A4_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A5", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A5_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A6", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A6_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A7", 0, RC_GENERAL, dt_dword, nullptr, 0 },
    { "A7_ADDR", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },

    { "PC", REGISTER_ADDRESS | REGISTER_IP | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "SP", REGISTER_ADDRESS | REGISTER_SP, RC_GENERAL, dt_dword, nullptr, 0 },
    { "SR", 0, RC_GENERAL, dt_word, SRReg, 0xFFFF },

    { "DMA_LEN", REGISTER_READONLY, RC_GENERAL, dt_word, nullptr, 0 },
    { "DMA_SRC", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },
    { "VDP_DST", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, nullptr, 0 },

    { "Set1",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "Set2",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "PlaneA", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "Window", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "PlaneB", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "Sprite", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "Reg6",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "BgClr",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "Reg8",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "Reg9",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "HInt",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "Set3",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "Set4",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "HScrl",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "Reg14",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "WrInc",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "ScrSz",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "WinX",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "WinY",   0, RC_VDP, dt_byte, nullptr, 0 },
    { "LenLo",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "LenHi",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "SrcLo",  0, RC_VDP, dt_byte, nullptr, 0 },
    { "SrcMid", 0, RC_VDP, dt_byte, nullptr, 0 },
    { "SrcHi",  0, RC_VDP, dt_byte, nullptr, 0 },
};

const char* register_classes[] = {
    "General Registers",
    "VDP Registers",
    nullptr,
};

drc_t read_registers(int clsmask, regval_t* values)
{
    IDebugBackend* be = cur_backend();
    if (!be) return DRC_FAILED;

    if (clsmask & RC_GENERAL) {
        M68kRegs r = be->getM68kRegs();
        for (int i = 0; i < 8; ++i) values[R_D0 + i].ival = r.d[i];
        for (int i = 0; i < 8; ++i) {
            values[R_A0 + i * 2].ival     = r.a[i];
            values[R_A0_ADDR + i * 2].ival = r.a[i] & 0xFFFFFF;
        }
        values[R_PC].ival = r.pc & 0xFFFFFF;
        values[R_SP].ival = r.a[7] & 0xFFFFFF;
        values[R_SR].ival = r.sr;

        VdpState v = be->getVdpState();
        values[R_VDP_DMA_LEN].ival = v.dma_len;
        values[R_VDP_DMA_SRC].ival = v.dma_src;      // already a byte address
        // Where the next data-port write lands, as an address IDA can follow
        // into the VDP pseudo-segments.
        uint32_t dst = BREAKPOINTS_BASE;
        switch (v.vdp_code & 0x0F) {
        case 0x1: dst += VDP_BP_VRAM  + v.vdp_addr; break;   // VRAM write
        case 0x3: dst += VDP_BP_CRAM  + v.vdp_addr; break;   // CRAM write
        case 0x5: dst += VDP_BP_VSRAM + v.vdp_addr; break;   // VSRAM write
        default:  break;                                     // reads/idle: base
        }
        values[R_VDP_WRITE_ADDR].ival = dst;
    }
    if (clsmask & RC_VDP) {
        VdpState v = be->getVdpState();
        for (int i = 0; i < 24; ++i)
            values[R_V00 + i].ival = v.reg[i];
    }
    return DRC_OK;
}

drc_t write_register(int regidx, const regval_t* value)
{
    IDebugBackend* be = cur_backend();
    if (!be) return DRC_FAILED;

    if (regidx >= R_D0 && regidx <= R_SR) {
        M68kRegs r = be->getM68kRegs();
        if (regidx <= R_D7)          r.d[regidx - R_D0] = (uint32_t)value->ival;
        else if (regidx < R_PC && ((regidx - R_A0) % 2) == 0)
                                     r.a[(regidx - R_A0) / 2] = (uint32_t)value->ival;
        else if (regidx == R_SP)     r.a[7] = (uint32_t)value->ival;
        else if (regidx == R_SR)     r.sr = (uint32_t)value->ival;
        else return DRC_OK;          // PC/ADDR pseudo-regs are read-only
        be->setM68kRegs(r);
        return DRC_OK;
    }
    if (regidx >= R_V00 && regidx <= R_V23) {
        be->setVdpReg(regidx - R_V00, (uint8_t)(value->ival & 0xFF));
        return DRC_OK;
    }
    return DRC_OK;
}

// ---------------------------------------------------------------------------
// breakpoints
// ---------------------------------------------------------------------------
void translate_bpt(ea_t ea, int size, bpttype_t itype,
                   uint8_t& t1, uint8_t& t2, uint8_t& vdp, uint32_t& start, uint32_t& end)
{
    start = (uint32_t)ea;
    end   = (uint32_t)(ea + (size ? size : 1) - 1);
    t2 = 0; vdp = 0;
    switch (itype) {
    case BPT_EXEC:  t1 = (uint8_t)BpType::PC;    break;
    case BPT_READ:  t1 = (uint8_t)BpType::Read;  break;
    case BPT_WRITE: t1 = (uint8_t)BpType::Write; break;
    case BPT_RDWR:  t1 = (uint8_t)BpType::Read; t2 = (uint8_t)BpType::Write; break;
    default:        t1 = (uint8_t)BpType::PC;    break;
    }
    if (start >= BREAKPOINTS_BASE && end < BREAKPOINTS_BASE + 0x30000) {
        start -= BREAKPOINTS_BASE;
        end   -= BREAKPOINTS_BASE;
        vdp = 1;
    }
    start &= 0xFFFFFF;
    end   &= 0xFFFFFF;
}

void add_one_bpt(uint8_t type, uint8_t vdp, uint32_t start, uint32_t end, ea_t ida_ea)
{
    Breakpoint bp;
    bp.type   = (BpType)type;
    bp.is_vdp = vdp != 0;
    bp.start  = start;
    bp.end    = end;
    // Conditions stay in IDA's language; we only carry them, and ask IDA to
    // judge them when the breakpoint is hit.
    bpt_t ibp;
    if (get_bpt(ida_ea, &ibp) && !ibp.cndbody.empty()) {
        bp.condition = ibp.cndbody.c_str();
        bp.elang     = uint32_t(ibp.get_cnd_elang_idx());
    }
    int id = cur_backend()->addBreakpoint(bp);
    g_bpIds[BpKey{type, vdp, start, end}] = id;
}

void del_one_bpt(uint8_t type, uint8_t vdp, uint32_t start, uint32_t end)
{
    auto it = g_bpIds.find(BpKey{type, vdp, start, end});
    if (it != g_bpIds.end()) {
        cur_backend()->removeBreakpoint(it->second);
        g_bpIds.erase(it);
    }
}

drc_t update_bpts(int* nbpts, update_bpt_info_t* bpts, int nadd, int ndel)
{
    if (!cur_backend()) return DRC_FAILED;
    int ok = 0;
    for (int i = 0; i < nadd; ++i) {
        if (bpts[i].code == BPT_SKIP) continue;
        uint8_t t1, t2, vdp; uint32_t s, e;
        translate_bpt(bpts[i].ea, (int)bpts[i].size, bpts[i].type, t1, t2, vdp, s, e);
        add_one_bpt(t1, vdp, s, e, bpts[i].ea);
        if (t2) add_one_bpt(t2, vdp, s, e, bpts[i].ea);
        bpts[i].code = BPT_OK;
        ++ok;
    }
    for (int i = 0; i < ndel; ++i) {
        if (bpts[nadd + i].code == BPT_SKIP) continue;
        uint8_t t1, t2, vdp; uint32_t s, e;
        translate_bpt(bpts[nadd + i].ea, (int)bpts[nadd + i].size, bpts[nadd + i].type, t1, t2, vdp, s, e);
        del_one_bpt(t1, vdp, s, e);
        if (t2) del_one_bpt(t2, vdp, s, e);
        bpts[nadd + i].code = BPT_OK;
        ++ok;
    }
    *nbpts = ok;
    return DRC_OK;
}

// ---------------------------------------------------------------------------
// process lifecycle
// ---------------------------------------------------------------------------
void stop_audio()
{
    delete g_audio;
    g_audio = nullptr;
}

drc_t start_process(const char* path, const char* input_path)
{
    // The bridge first: its handlers hold this EmuHost*, and its event sink is
    // registered on it. Deleting the host underneath a live BridgeServer left
    // both dangling for as long as a client stayed connected.
    delete g_bridge; g_bridge = nullptr;

    delete g_host;
    g_host = new EmuHost();
    g_romSize = 0;
    g_bpIds.clear();
    stop_audio();
    { std::lock_guard<std::mutex> lk(g_events.mx); g_events.q.clear(); }

    g_host->addEventSink(on_emu_event);
    g_host->backend()->setConditionEvaluator(evaluate_condition);

    // Bridge clients must not mutate run state or breakpoints behind IDA's
    // back; route them through IDA instead. See the block above.
    EmuHost::HostMutations hm;
    hm.resume           = host_resume;
    hm.pause            = host_pause;
    hm.addBreakpoint    = host_add_bpt;
    hm.removeBreakpoint = host_del_bpt;
    hm.clearBreakpoints = host_clear_bpts;
    g_host->setHostMutations(hm);
#ifdef SMD_DGX_IDA_VIEWS
    g_host->setFrameSink(smd_dgx_push_frame);   // no-op unless the Screen dock is open
#endif
    g_audio = new AudioOutput();
    if (!g_audio->isOpen())
        msg(PLUGIN_NAME ": no audio device — the emulator will run silently\n");
    g_host->setAudioSink([](const int16_t* stereo, int frames) {
        if (g_audio) g_audio->write(stereo, frames);
    });

    const char* rom = (input_path && input_path[0]) ? input_path : path;
    if (!g_host->start(rom ? rom : "")) {
        delete g_host;
        g_host = nullptr;
        stop_audio();
        return DRC_FAILED;
    }
    // start paused at the entry: request a pause right away
    g_host->backend()->pause();

    g_bridge = new BridgeServer(g_host);
    if (g_bridge->start())
        msg(PLUGIN_NAME ": control socket on 127.0.0.1:%u\n", g_bridge->port());
    else
        msg(PLUGIN_NAME ": control socket unavailable (port busy?)\n");

    return DRC_OK;
}

// ---------------------------------------------------------------------------
// HT_IDD callback — same dispatch table as Gensida
// ---------------------------------------------------------------------------
ssize_t idaapi debugger_callback(void*, int msgid, va_list va)
{
    drc_t retcode = DRC_NONE;

    switch (msgid) {
    case debugger_t::ev_init_debugger:
    case debugger_t::ev_term_debugger:
        retcode = DRC_OK;
        break;

    case debugger_t::ev_get_processes: {
        procinfo_vec_t* procs = va_arg(va, procinfo_vec_t*);
        // The Attach dialog lists every emulator on the scan range; the pid
        // doubles as the port so ev_attach_process knows who was picked.
        for (const auto& s : RemoteBackend::discover()) {
            process_info_t& pi = procs->push_back();
            pi.pid = s.port;
            pi.name.sprnt("%s (port %u)", s.name.c_str(), (unsigned)s.port);
        }
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_attach_process: {
        pid_t pid = va_argi(va, pid_t);
        retcode = attach68(std::to_string((unsigned)pid), true);
        break;
    }

    case debugger_t::ev_start_process: {
        const char* path      = va_arg(va, const char*);
        const char* args      = va_arg(va, const char*);
        const char* startdir  = va_arg(va, const char*);      (void)startdir;
        uint32 flags          = va_arg(va, uint32);           (void)flags;
        const char* input     = va_arg(va, const char*);
        uint32 crc            = va_arg(va, uint32);           (void)crc;
        // "attach" (or "attach:27043", "attach:sonic") in Process options ->
        // Parameters drives a running emulator instead of starting one here.
        std::string a = args ? args : "";
        if (a.rfind("attach", 0) == 0) {
            const size_t colon = a.find(':');
            retcode = attach68(colon == std::string::npos ? "" : a.substr(colon + 1),
                               /*as_attach=*/false);
            break;
        }
        detach68();             // a leftover attach must not shadow a real run
        retcode = start_process(path, input);
        break;
    }

    case debugger_t::ev_detach_process:
        if (!g_remote68) { retcode = DRC_FAILED; break; }   // in-process: nothing to let go of
        detach68();
        {
            // IDA keeps polling get_debug_event until exactly this event (idd.hpp).
            debug_event_t ev;
            ev.pid = 1; ev.tid = 1; ev.handled = true;
            ev.set_eid(PROCESS_DETACHED);
            ev.ea = BADADDR;
            g_events.enqueue(ev);
        }
        retcode = DRC_OK;
        break;

    case debugger_t::ev_get_debapp_attrs: {
        debapp_attrs_t* attrs = va_arg(va, debapp_attrs_t*);
        attrs->addrsize = 4;
        attrs->is_be = true;
        attrs->platform = "sega_md";
        attrs->cbsize = sizeof(debapp_attrs_t);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_request_pause:
        if (cur_backend()) cur_backend()->pause();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_exit_process:
        if (g_remote68) {
            // The emulator is someone else's session: let go of it, do not
            // stop it. detach68 resumes so the standalone stays playable.
            detach68();
            debug_event_t ev;
            ev.pid = 1; ev.tid = 1; ev.handled = true;
            ev.set_exit_code(PROCESS_EXITED, 0);
            ev.ea = BADADDR;
            g_events.enqueue(ev);
            retcode = DRC_OK;
            break;
        }
        // Close the socket before the host: its handler holds an EmuHost*.
        delete g_bridge; g_bridge = nullptr;
        if (g_host) { g_host->stop(); }
        stop_audio();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_get_debug_event: {
        gdecode_t* code = va_arg(va, gdecode_t*);
        debug_event_t* event = va_arg(va, debug_event_t*);
        bool more = false;
        *code = g_events.retrieve(event, &more)
                    ? (more ? GDE_MANY_EVENTS : GDE_ONE_EVENT)
                    : GDE_NO_EVENT;
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_resume: {
        debug_event_t* event = va_arg(va, debug_event_t*);
        dbg_notification_t req = get_running_notification();
        switch (event->eid()) {
        case STEP:
        case PROCESS_ATTACHED:
        case PROCESS_SUSPENDED:
            if (req == dbg_null || req == dbg_run_to)
                if (cur_backend()) cur_backend()->resume();
            break;
        case PROCESS_EXITED:
            break;
        default: break;
        }
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_thread_suspend:
        if (cur_backend()) cur_backend()->pause();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_thread_continue:
        if (cur_backend()) cur_backend()->resume();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_set_resume_mode: {
        thid_t tid = va_argi(va, thid_t);           (void)tid;
        resume_mode_t resmod = va_argi(va, resume_mode_t);
        IDebugBackend* be = cur_backend();
        if (!be) { retcode = DRC_FAILED; break; }
        switch (resmod) {
        case RESMOD_INTO: be->stepInto(Cpu::M68K); retcode = DRC_OK; break;
        case RESMOD_OVER: be->stepOver(Cpu::M68K); retcode = DRC_OK; break;
        default:          retcode = DRC_FAILED; break;
        }
        break;
    }

    case debugger_t::ev_read_registers: {
        thid_t tid  = va_argi(va, thid_t);          (void)tid;
        int clsmask = va_arg(va, int);
        regval_t* values = va_arg(va, regval_t*);
        retcode = read_registers(clsmask, values);
        break;
    }

    case debugger_t::ev_write_register: {
        thid_t tid = va_argi(va, thid_t);           (void)tid;
        int regidx = va_arg(va, int);
        const regval_t* value = va_arg(va, const regval_t*);
        retcode = write_register(regidx, value);
        break;
    }

    case debugger_t::ev_get_memory_info: {
        meminfo_vec_t* ranges = va_arg(va, meminfo_vec_t*);
        memory_info_t info;
        for (int i = 0; i < get_segm_qty(); ++i) {
            segment_t* s = getnseg(i);
            info.start_ea = s->start_ea;
            info.end_ea   = s->end_ea;
            qstring buf;
            get_segm_name(&buf, s);  info.name = buf;
            get_segm_class(&buf, s); info.sclass = buf;
            info.sbase = 0;
            info.perm = SEGPERM_READ | SEGPERM_WRITE;
            info.bitness = 1;
            ranges->push_back(info);
        }
        static const char* const vdp_names[] = { "DBG_VDP_VRAM", "DBG_VDP_CRAM", "DBG_VDP_VSRAM" };
        for (int i = 0; i < 3; ++i) {
            info.name = vdp_names[i];
            info.start_ea = BREAKPOINTS_BASE + 0x10000u * i;
            info.end_ea   = info.start_ea + 0x10000;
            info.bitness = 1;
            ranges->push_back(info);
        }
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_read_memory: {
        size_t* nbytes = va_arg(va, size_t*);
        ea_t ea = va_arg(va, ea_t);
        void* buffer = va_arg(va, void*);
        size_t size = va_arg(va, size_t);
        *nbytes = (size_t)region_rw(ea, buffer, size, false);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_write_memory: {
        size_t* nbytes = va_arg(va, size_t*);
        ea_t ea = va_arg(va, ea_t);
        void* buffer = va_arg(va, void*);
        size_t size = va_arg(va, size_t);
        *nbytes = (size_t)region_rw(ea, buffer, size, true);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_check_bpt: {
        int* bptvc = va_arg(va, int*);
        bpttype_t type = va_argi(va, bpttype_t);
        switch (type) {
        case BPT_EXEC: case BPT_READ: case BPT_WRITE: case BPT_RDWR:
            *bptvc = BPT_OK; break;
        default:
            *bptvc = BPT_BAD_TYPE; break;
        }
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_update_bpts: {
        int* nbpts = va_arg(va, int*);
        update_bpt_info_t* bpts = va_arg(va, update_bpt_info_t*);
        int nadd = va_arg(va, int);
        int ndel = va_arg(va, int);
        retcode = update_bpts(nbpts, bpts, nadd, ndel);
        break;
    }

    // Callstack: deferred (backend does not track it yet) — DRC_NONE keeps
    // IDA's default behavior.
    case debugger_t::ev_update_call_stack: {
        thid_t tid = va_argi(va, thid_t);   (void)tid;
        call_stack_t* trace = va_arg(va, call_stack_t*);
        if (!cur_backend() || !trace) { retcode = DRC_NONE; break; }
        trace->clear();
        // Backend order is outermost-first, which is what IDA expects.
        for (uint32_t ea : cur_backend()->getCallstack(Cpu::M68K)) {
            call_stack_info_t& f = trace->push_back();
            f.callea = ea;
            f.funcea = BADADDR;
            f.fp     = BADADDR;
            f.funcok = true;
        }
        retcode = DRC_OK;
        break;
    }

    default:
        retcode = DRC_NONE;
        break;
    }
    return retcode;
}

} // namespace

// ---------------------------------------------------------------------------
// DEBUGGER DESCRIPTION BLOCK — layout kept from Gensida  (VERIFY-9.3)
// ---------------------------------------------------------------------------
debugger_t debugger = {
    IDD_INTERFACE_VERSION,
    PLUGIN_NAME,
    0x8000 + 1,
    "m68k",

    // No DBG_FLAG_NOPARAMETERS: Process options -> Parameters is the attach
    // switch ("attach", "attach:27043", "attach:sonic"). Empty = run in-process.
    DBG_FLAG_NOHOST | DBG_FLAG_CAN_CONT_BPT | DBG_FLAG_SAFE
        | DBG_FLAG_NOPASSWORD | DBG_FLAG_NOSTARTDIR
        | DBG_FLAG_ANYSIZE_HWBPT | DBG_FLAG_DEBTHREAD | DBG_FLAG_PREFER_SWBPTS
        | DBG_HAS_GET_PROCESSES | DBG_HAS_REQUEST_PAUSE | DBG_HAS_SET_RESUME_MODE
        | DBG_HAS_CHECK_BPT | DBG_HAS_THREAD_SUSPEND | DBG_HAS_THREAD_CONTINUE
        | DBG_HAS_ATTACH_PROCESS | DBG_HAS_DETACH_PROCESS,

    register_classes,
    RC_GENERAL,
    registers,
    qnumber(registers),

    0x1000,        // memory page size

    nullptr, 0, 0, // bpt bytes

    DBG_RESMOD_STEP_INTO | DBG_RESMOD_STEP_OVER,
};

// ---------------------------------------------------------------------------
// plugin — modern 9.x idiom: PLUGIN_MULTI + plugmod_t that listens on HT_IDD
// (same structure as the SDK's own dbg modules, src/dbg/common_local_impl.cpp)
// ---------------------------------------------------------------------------

// accessor for the Qt-side view builder (this TU has no Qt, so the pointer
// crosses the boundary as-is)
EmuHost* smd_dgx_host() { return g_host; }


// --- Z80 half (smd_dgx_ida_z80.cpp) ---------------------------------------
// Same binary, different debugger_t: which one is registered depends on the
// database's processor, and the Z80 one attaches to the emulator this one runs.
extern debugger_t z80_debugger;
ssize_t idaapi smd_dgx_z80_event(int msgid, va_list va);
void smd_dgx_z80_shutdown();

struct smd_dgx_z80_plugmod_t : public plugmod_t, public event_listener_t {
    smd_dgx_z80_plugmod_t()
    {
        hook_event_listener(HT_IDD, this);
        dbg = &z80_debugger;
        smd_dgx_register_z80_actions();
        msg(PLUGIN_NAME ": Z80 debugger loaded (attaches to a running emulator)\n");
    }

    ~smd_dgx_z80_plugmod_t() override
    {
        smd_dgx_unregister_z80_actions();
        smd_dgx_z80_shutdown();
        if (dbg == &z80_debugger)
            dbg = nullptr;
    }

    // Same guard as the 68000 half: ::dbg is process-wide, and this table has
    // 26 registers where the other has 54.
    ssize_t idaapi on_event(ssize_t code, va_list va) override
    {
        if (dbg != &z80_debugger) return DRC_NONE;
        return smd_dgx_z80_event((int)code, va);
    }

    bool idaapi run(size_t) override { return false; }
};

struct smd_dgx_plugmod_t : public plugmod_t, public event_listener_t {
    smd_dgx_plugmod_t()
    {
        hook_event_listener(HT_IDD, this);
        dbg = &debugger;
#ifdef SMD_DGX_IDA_VIEWS
        smd_dgx_register_views();
#endif
        smd_dgx_register_patches();
        smd_dgx_register_asm();
        smd_dgx_register_z80_actions();
        msg(PLUGIN_NAME ": in-process GPGX debugger loaded\n");
    }

    ~smd_dgx_plugmod_t() override
    {
#ifdef SMD_DGX_IDA_VIEWS
        smd_dgx_unregister_views();
#endif
        smd_dgx_unregister_patches();
        smd_dgx_unregister_asm();
        smd_dgx_unregister_z80_actions();
        detach68();                     // attach mode: let the far emulator go
        cancel_codemap68();
        delete g_bridge; g_bridge = nullptr;
        if (g_host) { g_host->stop(); delete g_host; g_host = nullptr; }
        stop_audio();
        if (dbg == &debugger)
            dbg = nullptr;
        // event listeners hooked via plugmod_t are auto-unhooked on delete
    }

    // Only answer for the debugger IDA currently has selected.
    //
    // `dbg` is one slot for the whole process, and every debugger plugin
    // assigns to it — including this DLL's other half. IDA sizes its register
    // buffer from ::dbg (idd.hpp), and the two tables here are 54 registers and
    // 26, so answering an event meant for the other one writes past the end of
    // a buffer someone else allocated. This is not multi-database exotica: IDA
    // loads several debugger plugins for one database as a matter of course.
    bool is_ours() const { return dbg == &debugger; }

    ssize_t idaapi on_event(ssize_t code, va_list va) override
    {
        if (!is_ours()) return DRC_NONE;
        return debugger_callback(nullptr, (int)code, va);
    }

    bool idaapi run(size_t) override { return false; }
};

static plugmod_t* idaapi init()
{
    // One binary, two debuggers. A database is either the game (68000, runs
    // the emulator) or the sound driver (Z80, attaches to it); anything else
    // is none of our business.
    if (PH.id == PLFM_68K)
        return new smd_dgx_plugmod_t;
    if (PH.id == PLFM_Z80)
        return new smd_dgx_z80_plugmod_t;
    return nullptr;
}

plugin_t PLUGIN = {
    IDP_INTERFACE_VERSION,
    PLUGIN_MULTI | PLUGIN_HIDE | PLUGIN_DBG,
    init,
    nullptr,   // term: unused with PLUGIN_MULTI
    nullptr,   // run:  unused with PLUGIN_MULTI
    PLUGIN_NAME " debugger plugin (in-process Genesis Plus GX)",
    nullptr,
    PLUGIN_NAME " debugger",
    nullptr,
};
