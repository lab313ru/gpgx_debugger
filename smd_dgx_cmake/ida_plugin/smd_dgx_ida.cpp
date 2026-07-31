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
#include "platform/AudioOutput.h"

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
// memory routing: side-effect-free reads via typed regions
// (never touches IO read handlers; unmapped bytes read as 0, like Gens)
// ---------------------------------------------------------------------------
ssize_t region_rw(ea_t ea, void* buf, size_t size, bool write)
{
    if (!g_host) return 0;
    IDebugBackend* be = g_host->backend();
    uint8_t* p = (uint8_t*)buf;

    // ROM size from the region table. Session state, not a one-shot static:
    // a second ROM loaded into the same IDA session is a different size, and a
    // cached one from the previous game makes the new one read as zeros past
    // the old end — or shadow RAM that lives beyond it.
    if (!g_romSize)
        for (const auto& r : be->getMemRegions())
            if (r.id == 0) g_romSize = r.size;
    const uint32_t romSize = g_romSize;

    for (size_t i = 0; i < size; ++i) {
        uint32_t a = (uint32_t)(ea + i);
        int region = -1; uint32_t off = 0;
        if (a < romSize)                                { region = 0; off = a; }
        else if (a >= 0xFF0000 && a <= 0xFFFFFF)        { region = 1; off = a - 0xFF0000; }
        else if (a >= 0xA00000 && a <  0xA02000)        { region = 2; off = a - 0xA00000; }
        else if (a >= BREAKPOINTS_BASE && a < BREAKPOINTS_BASE + 0x10000)
                                                        { region = 3; off = a - BREAKPOINTS_BASE; }
        else if (a >= BREAKPOINTS_BASE + 0x10000 && a < BREAKPOINTS_BASE + 0x20000)
                                                        { region = 4; off = a - BREAKPOINTS_BASE - 0x10000; }
        else if (a >= BREAKPOINTS_BASE + 0x20000 && a < BREAKPOINTS_BASE + 0x30000)
                                                        { region = 5; off = a - BREAKPOINTS_BASE - 0x20000; }

        if (region < 0) { if (!write) p[i] = 0; continue; }
        if (write) {
            be->writeRegion(region, off, &p[i], 1);
        } else {
            auto b = be->readRegion(region, off, 1);
            p[i] = b.empty() ? 0 : b[0];
        }
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
    if (!g_host) return DRC_FAILED;
    IDebugBackend* be = g_host->backend();

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
    if (!g_host) return DRC_FAILED;
    IDebugBackend* be = g_host->backend();

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
    int id = g_host->backend()->addBreakpoint(bp);
    g_bpIds[BpKey{type, vdp, start, end}] = id;
}

void del_one_bpt(uint8_t type, uint8_t vdp, uint32_t start, uint32_t end)
{
    auto it = g_bpIds.find(BpKey{type, vdp, start, end});
    if (it != g_bpIds.end()) {
        g_host->backend()->removeBreakpoint(it->second);
        g_bpIds.erase(it);
    }
}

drc_t update_bpts(int* nbpts, update_bpt_info_t* bpts, int nadd, int ndel)
{
    if (!g_host) return DRC_FAILED;
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
        process_info_t& pi = procs->push_back();   // VERIFY-9.3
        pi.pid = 1;
        pi.name = "GPGX";
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_start_process: {
        const char* path      = va_arg(va, const char*);
        const char* args      = va_arg(va, const char*);      (void)args;
        const char* startdir  = va_arg(va, const char*);      (void)startdir;
        uint32 flags          = va_arg(va, uint32);           (void)flags;
        const char* input     = va_arg(va, const char*);
        uint32 crc            = va_arg(va, uint32);           (void)crc;
        retcode = start_process(path, input);
        break;
    }

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
        if (g_host) g_host->backend()->pause();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_exit_process:
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
        case PROCESS_SUSPENDED:
            if (req == dbg_null || req == dbg_run_to)
                if (g_host) g_host->backend()->resume();
            break;
        case PROCESS_EXITED:
            break;
        default: break;
        }
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_thread_suspend:
        if (g_host) g_host->backend()->pause();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_thread_continue:
        if (g_host) g_host->backend()->resume();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_set_resume_mode: {
        thid_t tid = va_argi(va, thid_t);           (void)tid;
        resume_mode_t resmod = va_argi(va, resume_mode_t);
        if (!g_host) { retcode = DRC_FAILED; break; }
        switch (resmod) {
        case RESMOD_INTO: g_host->backend()->stepInto(Cpu::M68K); retcode = DRC_OK; break;
        case RESMOD_OVER: g_host->backend()->stepOver(Cpu::M68K); retcode = DRC_OK; break;
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
        if (!g_host || !trace) { retcode = DRC_NONE; break; }
        trace->clear();
        // Backend order is outermost-first, which is what IDA expects.
        for (uint32_t ea : g_host->backend()->getCallstack(Cpu::M68K)) {
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

    DBG_FLAG_NOHOST | DBG_FLAG_CAN_CONT_BPT | DBG_FLAG_SAFE | DBG_FLAG_FAKE_ATTACH
        | DBG_FLAG_NOPASSWORD | DBG_FLAG_NOSTARTDIR | DBG_FLAG_NOPARAMETERS
        | DBG_FLAG_ANYSIZE_HWBPT | DBG_FLAG_DEBTHREAD | DBG_FLAG_PREFER_SWBPTS
        | DBG_HAS_GET_PROCESSES | DBG_HAS_REQUEST_PAUSE | DBG_HAS_SET_RESUME_MODE
        | DBG_HAS_CHECK_BPT | DBG_HAS_THREAD_SUSPEND | DBG_HAS_THREAD_CONTINUE,

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
