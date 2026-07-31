// SMD DGX — the Z80 half.
//
// One IDB holds one processor module, so a database disassembling the sound
// driver cannot also disassemble the 68000 game. The two therefore live in two
// IDA instances: the 68000 one *runs* the emulator, this one *attaches* to it
// over the bridge socket. Same machine, same frame, two disassemblies — which
// is what Mega Drive sound work actually needs, because the music is usually
// sequenced by the main CPU and only played by the Z80.
//
// The debugger_t surface below is the same shape as the 68000 one; everything
// underneath goes through RemoteBackend instead of EmuHost.

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>

#include <ida.hpp>
#include <idp.hpp>
#include <idd.hpp>
#include <dbg.hpp>
#include <auto.hpp>
#include <kernwin.hpp>
#include <segment.hpp>

#include "debugger/RemoteBackend.h"

#define Z80_PLUGIN_NAME "SMD DGX Z80"

namespace {

// --- register indices; must match z80_registers[] below --------------------
#define RCZ_GENERAL 1

enum z80_register_t {
    RZ_AF, RZ_BC, RZ_DE, RZ_HL,
    RZ_IX, RZ_IY,
    RZ_A, RZ_B, RZ_C, RZ_D, RZ_E, RZ_H, RZ_L,
    RZ_IXH, RZ_IXL, RZ_IYH, RZ_IYL,
    RZ_AF2, RZ_BC2, RZ_DE2, RZ_HL2,
    RZ_I, RZ_R,
    RZ_SP, RZ_PC,
    RZ_BANK,
};

const char* const Z80FlagBits[] = {
    "CF", "NF", "PF", "B3", "HF", "B5", "ZF", "SF",
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
};

// IDA's own z80 module resolves register names case-insensitively against a
// fixed list (af/bc/de/hl/…/sp/pc) with no fallback, so the instruction
// pointer must be called "PC". Gensida called it "IP", which quietly broke
// every register-name expression and the module's own step-over analysis.
register_info_t z80_registers[] = {
    { "AF", 0,                RCZ_GENERAL, dt_word, Z80FlagBits, 0x00FF },
    { "BC", REGISTER_ADDRESS, RCZ_GENERAL, dt_word, nullptr, 0 },
    { "DE", REGISTER_ADDRESS, RCZ_GENERAL, dt_word, nullptr, 0 },
    { "HL", REGISTER_ADDRESS, RCZ_GENERAL, dt_word, nullptr, 0 },

    { "IX", REGISTER_ADDRESS, RCZ_GENERAL, dt_word, nullptr, 0 },
    { "IY", REGISTER_ADDRESS, RCZ_GENERAL, dt_word, nullptr, 0 },

    { "A", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "B", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "C", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "D", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "E", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "H", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "L", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },

    { "IXH", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "IXL", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "IYH", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "IYL", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },

    { "AF'", 0,                RCZ_GENERAL, dt_word, Z80FlagBits, 0x00FF },
    { "BC'", REGISTER_ADDRESS, RCZ_GENERAL, dt_word, nullptr, 0 },
    { "DE'", REGISTER_ADDRESS, RCZ_GENERAL, dt_word, nullptr, 0 },
    { "HL'", REGISTER_ADDRESS, RCZ_GENERAL, dt_word, nullptr, 0 },

    { "I", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },
    { "R", 0, RCZ_GENERAL, dt_byte, nullptr, 0 },

    { "SP", REGISTER_ADDRESS | REGISTER_SP, RCZ_GENERAL, dt_word, nullptr, 0 },
    { "PC", REGISTER_ADDRESS | REGISTER_IP, RCZ_GENERAL, dt_word, nullptr, 0 },

    // Not a CPU register: the 68000-space base of the $8000 window. Read-only
    // because it is shifted in one bit at a time through $6000 and poking it
    // from here would desynchronise the driver's own idea of the bank.
    { "BANK", REGISTER_ADDRESS | REGISTER_READONLY, RCZ_GENERAL, dt_dword, nullptr, 0 },
};

const char* z80_register_classes[] = { "General Registers", nullptr };

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
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
    void clear() { std::lock_guard<std::mutex> lk(mx); q.clear(); }
};

EventList        g_zevents;
RemoteBackend*   g_remote = nullptr;
std::thread      g_pollThread;
std::atomic<bool> g_polling { false };

struct BpKey {
    uint8_t type; uint32_t start; uint32_t end;
    bool operator<(const BpKey& o) const {
        return std::tie(type, start, end) < std::tie(o.type, o.start, o.end);
    }
};
std::map<BpKey, int> g_zbpIds;

// ---------------------------------------------------------------------------
// event polling
//
// The line protocol has no way to push, so the far side queues and we ask.
// 20 ms is well under human reaction time and costs one tiny round trip.
// ---------------------------------------------------------------------------
void poll_loop()
{
    while (g_polling.load()) {
        RemoteBackend::RemoteEvent ev;
        bool any = false;
        while (g_remote && g_remote->pollEvent(ev)) {
            any = true;
            debug_event_t ida_ev;
            ida_ev.pid = 1;
            ida_ev.tid = 1;
            ida_ev.handled = true;

            switch (ev.type) {
            case DebugEvent::Type::Paused:
                // Report *our* PC regardless of which CPU tripped the
                // breakpoint: when the machine stops, the Z80 stops too, and
                // this database can only meaningfully point at Z80 code.
                ida_ev.set_eid(PROCESS_SUSPENDED);
                ida_ev.ea = g_remote->getZ80Regs().pc;
                g_zevents.enqueue(ida_ev);
                break;
            case DebugEvent::Type::Stopped:
                ida_ev.set_exit_code(PROCESS_EXITED, 0);
                ida_ev.ea = BADADDR;
                g_zevents.enqueue(ida_ev);
                break;
            default:
                break;      // Resumed/Started need no IDA event here
            }
        }
        if (!any)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void start_polling()
{
    if (g_polling.load()) return;
    g_polling.store(true);
    g_pollThread = std::thread(poll_loop);
}

void stop_polling()
{
    if (!g_polling.load()) return;
    g_polling.store(false);
    if (g_pollThread.joinable()) g_pollThread.join();
}

// ---------------------------------------------------------------------------
// attach
// ---------------------------------------------------------------------------
// `as_attach` picks which event IDA's state machine is waiting for. With
// DBG_FLAG_FAKE_ATTACH set, "attach to process" is delivered to us as
// ev_start_process, and answering that with PROCESS_ATTACHED leaves debthread
// waiting for a start that never comes.
drc_t attach_process(bool as_attach)
{
    // Before the delete, always: the poll thread dereferences g_remote on every
    // tick. Detaching stops it, but attaching twice without detaching in
    // between does not, and then this frees the object out from under it.
    stop_polling();

    delete g_remote;
    g_remote = new RemoteBackend();
    g_zbpIds.clear();
    g_zevents.clear();

    if (!g_remote->connect()) {
        delete g_remote;
        g_remote = nullptr;
        warning("SMD DGX: no emulator on 127.0.0.1:27042.\n\n"
                "Start the game in the 68000 database (or the standalone app)\n"
                "first — this database attaches to it, it does not run its own.");
        return DRC_FAILED;
    }

    debug_event_t ev;
    ev.pid = 1;
    ev.tid = 1;
    ev.handled = true;
    modinfo_t& mi = ev.set_modinfo(as_attach ? PROCESS_ATTACHED : PROCESS_STARTED);
    ev.ea = BADADDR;
    mi.name = "GPGX Z80";
    mi.base = 0;
    mi.size = 0x10000;
    mi.rebase_to = BADADDR;
    g_zevents.enqueue(ev);

    start_polling();
    g_remote->pause();          // land on a Z80 instruction straight away
    msg(Z80_PLUGIN_NAME ": attached to the emulator on 127.0.0.1:27042\n");
    return DRC_OK;
}

// ---------------------------------------------------------------------------
// registers
// ---------------------------------------------------------------------------
drc_t z80_read_registers(int clsmask, regval_t* values)
{
    if (!g_remote) return DRC_FAILED;
    if (!(clsmask & RCZ_GENERAL)) return DRC_OK;

    const Z80Regs r = g_remote->getZ80Regs();
    values[RZ_AF].ival = r.af; values[RZ_BC].ival = r.bc;
    values[RZ_DE].ival = r.de; values[RZ_HL].ival = r.hl;
    values[RZ_IX].ival = r.ix; values[RZ_IY].ival = r.iy;

    // IDA wants AF as one word with A high and F low, which is how the pair
    // is laid out; the byte views below are just conveniences.
    values[RZ_A].ival = (r.af >> 8) & 0xFF;
    values[RZ_B].ival = (r.bc >> 8) & 0xFF; values[RZ_C].ival = r.bc & 0xFF;
    values[RZ_D].ival = (r.de >> 8) & 0xFF; values[RZ_E].ival = r.de & 0xFF;
    values[RZ_H].ival = (r.hl >> 8) & 0xFF; values[RZ_L].ival = r.hl & 0xFF;
    values[RZ_IXH].ival = (r.ix >> 8) & 0xFF; values[RZ_IXL].ival = r.ix & 0xFF;
    values[RZ_IYH].ival = (r.iy >> 8) & 0xFF; values[RZ_IYL].ival = r.iy & 0xFF;

    values[RZ_AF2].ival = r.af2; values[RZ_BC2].ival = r.bc2;
    values[RZ_DE2].ival = r.de2; values[RZ_HL2].ival = r.hl2;

    values[RZ_I].ival = r.i; values[RZ_R].ival = r.r;
    values[RZ_SP].ival = r.sp; values[RZ_PC].ival = r.pc;
    values[RZ_BANK].ival = r.bank;
    return DRC_OK;
}

drc_t z80_write_register(int regidx, const regval_t* value)
{
    if (!g_remote) return DRC_FAILED;
    Z80Regs r = g_remote->getZ80Regs();
    const uint32_t v = uint32_t(value->ival);
    const uint16_t w = uint16_t(v);
    const uint8_t  b = uint8_t(v);

    switch (regidx) {
    case RZ_AF: r.af = w; break;  case RZ_BC: r.bc = w; break;
    case RZ_DE: r.de = w; break;  case RZ_HL: r.hl = w; break;
    case RZ_IX: r.ix = w; break;  case RZ_IY: r.iy = w; break;
    case RZ_A:  r.af = uint16_t((r.af & 0x00FF) | (b << 8)); break;
    case RZ_B:  r.bc = uint16_t((r.bc & 0x00FF) | (b << 8)); break;
    case RZ_C:  r.bc = uint16_t((r.bc & 0xFF00) | b); break;
    case RZ_D:  r.de = uint16_t((r.de & 0x00FF) | (b << 8)); break;
    case RZ_E:  r.de = uint16_t((r.de & 0xFF00) | b); break;
    case RZ_H:  r.hl = uint16_t((r.hl & 0x00FF) | (b << 8)); break;
    case RZ_L:  r.hl = uint16_t((r.hl & 0xFF00) | b); break;
    case RZ_IXH: r.ix = uint16_t((r.ix & 0x00FF) | (b << 8)); break;
    case RZ_IXL: r.ix = uint16_t((r.ix & 0xFF00) | b); break;
    case RZ_IYH: r.iy = uint16_t((r.iy & 0x00FF) | (b << 8)); break;
    case RZ_IYL: r.iy = uint16_t((r.iy & 0xFF00) | b); break;
    case RZ_AF2: r.af2 = w; break; case RZ_BC2: r.bc2 = w; break;
    case RZ_DE2: r.de2 = w; break; case RZ_HL2: r.hl2 = w; break;
    case RZ_I:  r.i = b; break;    case RZ_R:  r.r = b; break;
    case RZ_SP: r.sp = w; break;   case RZ_PC: r.pc = w; break;
    default: return DRC_OK;        // BANK is read-only
    }
    g_remote->setZ80Regs(r);
    return DRC_OK;
}

// ---------------------------------------------------------------------------
// memory — the Z80's own 16-bit space, served by the far side
// ---------------------------------------------------------------------------
ssize_t z80_read(ea_t ea, void* buf, size_t size)
{
    if (!g_remote || size == 0) return 0;
    const auto d = g_remote->readZ80Memory(uint16_t(ea), uint16_t(size));
    const size_t n = d.size() < size ? d.size() : size;
    memcpy(buf, d.data(), n);
    return ssize_t(n);
}

ssize_t z80_write(ea_t ea, const void* buf, size_t size)
{
    if (!g_remote || size == 0) return 0;
    // Only RAM is writable, and it is region 2 on the far side. Anything above
    // $2000 is hardware or the bank window; writing there through a debugger
    // would be a side effect, not an edit.
    const uint32_t off = uint32_t(ea) & 0x1FFF;
    if (uint32_t(ea) >= 0x2000) return 0;
    const size_t n = (off + size > 0x2000) ? (0x2000 - off) : size;
    g_remote->writeRegion(2, off, (const uint8_t*)buf, uint32_t(n));
    return ssize_t(n);
}

// ---------------------------------------------------------------------------
// breakpoints — always cpu=z80, so a 68000 breakpoint at the same numeric
// address in the other database never collides with ours
// ---------------------------------------------------------------------------
drc_t z80_update_bpts(int* nbpts, update_bpt_info_t* bpts, int nadd, int ndel)
{
    if (!g_remote) return DRC_FAILED;
    int ok = 0;

    auto kind = [](bpttype_t t) -> uint8_t {
        switch (t) {
        case BPT_READ:  return (uint8_t)BpType::Read;
        case BPT_WRITE: return (uint8_t)BpType::Write;
        default:        return (uint8_t)BpType::PC;
        }
    };

    for (int i = 0; i < nadd; ++i) {
        if (bpts[i].code == BPT_SKIP) continue;
        const uint32_t s = uint32_t(bpts[i].ea) & 0xFFFF;
        const uint32_t e = uint32_t(bpts[i].ea + (bpts[i].size ? bpts[i].size : 1) - 1) & 0xFFFF;
        Breakpoint bp;
        bp.type   = (BpType)kind(bpts[i].type);
        bp.cpu    = Cpu::Z80;
        bp.is_vdp = false;
        bp.start  = s;
        bp.end    = e;
        // No condition: the far side cannot call back into this IDA to
        // evaluate one (see RemoteBackend::setConditionEvaluator).
        const int id = g_remote->addBreakpoint(bp);
        g_zbpIds[BpKey{ (uint8_t)bp.type, s, e }] = id;
        bpts[i].code = BPT_OK;
        ++ok;
    }
    for (int i = 0; i < ndel; ++i) {
        update_bpt_info_t& d = bpts[nadd + i];
        if (d.code == BPT_SKIP) continue;
        const uint32_t s = uint32_t(d.ea) & 0xFFFF;
        const uint32_t e = uint32_t(d.ea + (d.size ? d.size : 1) - 1) & 0xFFFF;
        auto it = g_zbpIds.find(BpKey{ kind(d.type), s, e });
        if (it != g_zbpIds.end()) {
            g_remote->removeBreakpoint(it->second);
            g_zbpIds.erase(it);
        }
        d.code = BPT_OK;
        ++ok;
    }
    *nbpts = ok;
    return DRC_OK;
}

// ---------------------------------------------------------------------------
// HT_IDD dispatch
// ---------------------------------------------------------------------------
ssize_t idaapi z80_debugger_callback(void*, int msgid, va_list va)
{
    drc_t retcode = DRC_NONE;

    switch (msgid) {
    case debugger_t::ev_init_debugger:
    case debugger_t::ev_term_debugger:
        retcode = DRC_OK;
        break;

    case debugger_t::ev_get_processes: {
        procinfo_vec_t* procs = va_arg(va, procinfo_vec_t*);
        process_info_t& pi = procs->push_back();
        pi.pid = 1;
        pi.name = "GPGX Z80 (attach)";
        retcode = DRC_OK;
        break;
    }

    // Both paths attach: this database never owns an emulator.
    case debugger_t::ev_attach_process:
        retcode = attach_process(true);
        break;
    case debugger_t::ev_start_process:
        retcode = attach_process(false);
        break;

    case debugger_t::ev_get_debapp_attrs: {
        debapp_attrs_t* attrs = va_arg(va, debapp_attrs_t*);
        attrs->addrsize = 2;            // 64K
        attrs->is_be = false;           // Z80 is little-endian
        attrs->platform = "sega_md_z80";
        attrs->cbsize = sizeof(debapp_attrs_t);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_request_pause:
    case debugger_t::ev_thread_suspend:
        if (g_remote) g_remote->pause();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_thread_continue:
        if (g_remote) g_remote->resume();
        retcode = DRC_OK;
        break;

    case debugger_t::ev_detach_process:
    case debugger_t::ev_exit_process:
        // Detach only: the emulator belongs to the other database and must
        // keep running. Drop our breakpoints so the game is not left stopping
        // at addresses nobody is watching any more.
        stop_polling();
        if (g_remote) {
            for (const auto& kv : g_zbpIds) g_remote->removeBreakpoint(kv.second);
            g_zbpIds.clear();
            g_remote->resume();
            g_remote->disconnect();
        }
        {
            debug_event_t ev;
            ev.pid = 1; ev.tid = 1; ev.handled = true;
            ev.set_exit_code(PROCESS_EXITED, 0);
            ev.ea = BADADDR;
            g_zevents.enqueue(ev);
        }
        retcode = DRC_OK;
        break;

    case debugger_t::ev_get_debug_event: {
        gdecode_t* code = va_arg(va, gdecode_t*);
        debug_event_t* event = va_arg(va, debug_event_t*);
        bool more = false;
        *code = g_zevents.retrieve(event, &more)
                    ? (more ? GDE_MANY_EVENTS : GDE_ONE_EVENT)
                    : GDE_NO_EVENT;
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_resume: {
        debug_event_t* event = va_arg(va, debug_event_t*);
        const dbg_notification_t req = get_running_notification();
        switch (event->eid()) {
        case STEP:
        case PROCESS_ATTACHED:
        case PROCESS_SUSPENDED:
            if (req == dbg_null || req == dbg_run_to)
                if (g_remote) g_remote->resume();
            break;
        default: break;
        }
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_set_resume_mode: {
        thid_t tid = va_argi(va, thid_t);           (void)tid;
        const resume_mode_t resmod = va_argi(va, resume_mode_t);
        if (!g_remote) { retcode = DRC_FAILED; break; }
        switch (resmod) {
        case RESMOD_INTO: g_remote->stepInto(Cpu::Z80); retcode = DRC_OK; break;
        case RESMOD_OVER: g_remote->stepOver(Cpu::Z80); retcode = DRC_OK; break;
        default:          retcode = DRC_FAILED; break;
        }
        break;
    }

    case debugger_t::ev_read_registers: {
        thid_t tid  = va_argi(va, thid_t);          (void)tid;
        const int clsmask = va_arg(va, int);
        regval_t* values = va_arg(va, regval_t*);
        retcode = z80_read_registers(clsmask, values);
        break;
    }

    case debugger_t::ev_write_register: {
        thid_t tid = va_argi(va, thid_t);           (void)tid;
        const int regidx = va_arg(va, int);
        const regval_t* value = va_arg(va, const regval_t*);
        retcode = z80_write_register(regidx, value);
        break;
    }

    case debugger_t::ev_get_memory_info: {
        meminfo_vec_t* ranges = va_arg(va, meminfo_vec_t*);
        memory_info_t info;
        info.start_ea = 0;
        info.end_ea   = 0x2000;
        info.name     = "Z80_RAM";
        info.sclass   = "CODE";
        info.sbase    = 0;
        info.perm     = SEGPERM_READ | SEGPERM_WRITE | SEGPERM_EXEC;
        info.bitness  = 0;                  // 16-bit
        ranges->push_back(info);

        // The window is real address space and often holds the sample banks;
        // read-only here because a write would land in 68000 memory.
        info.start_ea = 0x8000;
        info.end_ea   = 0x10000;
        info.name     = "Z80_BANK";
        info.sclass   = "DATA";
        info.perm     = SEGPERM_READ;
        ranges->push_back(info);

        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_read_memory: {
        size_t* nbytes = va_arg(va, size_t*);
        const ea_t ea = va_arg(va, ea_t);
        void* buffer = va_arg(va, void*);
        const size_t size = va_arg(va, size_t);
        *nbytes = (size_t)z80_read(ea, buffer, size);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_write_memory: {
        size_t* nbytes = va_arg(va, size_t*);
        const ea_t ea = va_arg(va, ea_t);
        void* buffer = va_arg(va, void*);
        const size_t size = va_arg(va, size_t);
        *nbytes = (size_t)z80_write(ea, buffer, size);
        retcode = DRC_OK;
        break;
    }

    case debugger_t::ev_check_bpt: {
        int* bptvc = va_arg(va, int*);
        const bpttype_t type = va_argi(va, bpttype_t);
        switch (type) {
        case BPT_EXEC: case BPT_READ: case BPT_WRITE:
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
        const int nadd = va_arg(va, int);
        const int ndel = va_arg(va, int);
        retcode = z80_update_bpts(nbpts, bpts, nadd, ndel);
        break;
    }

    case debugger_t::ev_update_call_stack: {
        thid_t tid = va_argi(va, thid_t);   (void)tid;
        call_stack_t* trace = va_arg(va, call_stack_t*);
        if (!g_remote || !trace) { retcode = DRC_NONE; break; }
        trace->clear();
        for (uint32_t ea : g_remote->getCallstack(Cpu::Z80)) {
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
// the Z80 debugger description block
// ---------------------------------------------------------------------------
debugger_t z80_debugger = {
    IDD_INTERFACE_VERSION,
    Z80_PLUGIN_NAME,
    0x8000 + 2,
    "z80",

    DBG_FLAG_NOHOST | DBG_FLAG_CAN_CONT_BPT | DBG_FLAG_SAFE | DBG_FLAG_FAKE_ATTACH
        | DBG_FLAG_NOPASSWORD | DBG_FLAG_NOSTARTDIR | DBG_FLAG_NOPARAMETERS
        | DBG_FLAG_ANYSIZE_HWBPT | DBG_FLAG_DEBTHREAD | DBG_FLAG_PREFER_SWBPTS
        | DBG_HAS_GET_PROCESSES | DBG_HAS_REQUEST_PAUSE | DBG_HAS_SET_RESUME_MODE
        | DBG_HAS_CHECK_BPT | DBG_HAS_THREAD_SUSPEND | DBG_HAS_THREAD_CONTINUE,

    z80_register_classes,
    RCZ_GENERAL,
    z80_registers,
    qnumber(z80_registers),

    0x100,          // memory page size — the whole space is 64K

    nullptr, 0, 0,  // bpt bytes

    DBG_RESMOD_STEP_INTO | DBG_RESMOD_STEP_OVER,
};

// Called by the shared plugin entry point (smd_dgx_ida.cpp) when the database
// turns out to be a Z80 one.
ssize_t idaapi smd_dgx_z80_event(int msgid, va_list va)
{
    return z80_debugger_callback(nullptr, msgid, va);
}

void smd_dgx_z80_shutdown()
{
    stop_polling();
    delete g_remote;
    g_remote = nullptr;
}

// Live 8K dump for the loader, so a driver that ships compressed in the ROM is
// still readable: by the time it is running, it has unpacked itself.
bool smd_dgx_z80_dump_ram(std::vector<uint8_t>& out)
{
    if (!g_remote || !g_remote->isConnected()) {
        RemoteBackend tmp;
        if (!tmp.connect()) return false;
        out = tmp.readZ80Memory(0, 0x2000);
        return out.size() == 0x2000;
    }
    out = g_remote->readZ80Memory(0, 0x2000);
    return out.size() == 0x2000;
}
