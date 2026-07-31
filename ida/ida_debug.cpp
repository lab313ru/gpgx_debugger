#include <QWidget>

#include <ida.hpp>
#include <idd.hpp>
#include <auto.hpp>
#include <funcs.hpp>
#include <idp.hpp>
#include <dbg.hpp>

#include "ida_plugin.h"
#include "ida_debmod.h"

#include "debug_wrap.h"

extern TWidget* bpts_w;
extern const char* bpts_w_name;

dbg_request_t *dbg_req = NULL;

static void pause_execution()
{
    send_dbg_request(dbg_req, request_type_t::REQ_PAUSE, 0);
}

static void continue_execution()
{
    send_dbg_request(dbg_req, request_type_t::REQ_RESUME, 0);
}

static void stop_debugging()
{
    send_dbg_request(dbg_req, request_type_t::REQ_STOP, 0);
}

eventlist_t g_events;
static qthread_t events_thread = NULL;

static const char *const SRReg[] =
{
    "C",
    "V",
    "Z",
    "N",
    "X",
    NULL,
    NULL,
    NULL,
    "I",
    "I",
    "I",
    NULL,
    NULL,
    "S",
    NULL,
    "T"
};

#define RC_GENERAL (1 << 0)
#define RC_VDP (1 << 1)
#define RC_Z80 (1 << 2)


register_info_t registers[] =
{
    { "D0", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "D1", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "D2", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "D3", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "D4", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "D5", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "D6", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "D7", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },

    { "A0", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "A1", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "A2", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "A3", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "A4", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "A5", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "A6", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "A7", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },

    { "PC", REGISTER_ADDRESS | REGISTER_IP, RC_GENERAL, dt_dword, NULL, 0 },
    { "SR", NULL, RC_GENERAL, dt_word, SRReg, 0xFFFF },

    { "SP", REGISTER_ADDRESS | REGISTER_SP, RC_GENERAL, dt_dword, NULL, 0 },
    { "USP", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },
    { "ISP", REGISTER_ADDRESS, RC_GENERAL, dt_dword, NULL, 0 },

    { "PPC", REGISTER_ADDRESS | REGISTER_READONLY, RC_GENERAL, dt_dword, NULL, 0 },
    { "IR", NULL, RC_GENERAL, dt_dword, NULL, 0 },

    // VDP Registers
    { "v00", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v01", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v02", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v03", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v04", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v05", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v06", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v07", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v08", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v09", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v0A", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v0B", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v0C", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v0D", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v0E", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v0F", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v10", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v11", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v12", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v13", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v14", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v15", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v16", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v17", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v18", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v19", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v1A", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v1B", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v1C", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v1D", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v1E", NULL, RC_VDP, dt_byte, NULL, 0 },
    { "v1F", NULL, RC_VDP, dt_byte, NULL, 0 },

    { "DMA_LEN", REGISTER_READONLY, RC_VDP, dt_word, NULL, 0 },
    { "DMA_SRC", REGISTER_ADDRESS | REGISTER_READONLY, RC_VDP, dt_dword, NULL, 0 },
    { "VDP_DST", REGISTER_ADDRESS | REGISTER_READONLY, RC_VDP, dt_dword, NULL, 0 },

    // Z80 regs
    { "zPC", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zSP", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zAF", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zBC", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zDE", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zHL", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zIX", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zIY", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zWZ", NULL, RC_Z80, dt_dword, NULL, 0 },

    { "zAF2", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zBC2", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zDE2", NULL, RC_Z80, dt_dword, NULL, 0 },
    { "zHL2", NULL, RC_Z80, dt_dword, NULL, 0 },

    { "zR", NULL, RC_Z80, dt_byte, NULL, 0 },
    { "zR2", NULL, RC_Z80, dt_byte, NULL, 0 },
    { "zIFFI1", NULL, RC_Z80, dt_byte, NULL, 0 },
    { "zIFFI2", NULL, RC_Z80, dt_byte, NULL, 0 },
    { "zHALT", NULL, RC_Z80, dt_byte, NULL, 0 },
    { "zIM", NULL, RC_Z80, dt_byte, NULL, 0 },
    { "zI", NULL, RC_Z80, dt_byte, NULL, 0 },
};

static const char *register_classes[] =
{
    "General Registers",
    "VDP Registers",
    "Z80 Registers",
    NULL
};

static void finish_execution()
{
    if (events_thread != NULL)
    {
        qthread_join(events_thread);
        qthread_free(events_thread);
        qthread_kill(events_thread);
        events_thread = NULL;
    }
}

static drc_t idaapi init_debugger(const char* hostname, int portnum, const char* password, qstring *errbuf)
{
    set_processor_type(ph.psnames[0], SETPROC_LOADER); // reset proc to "M68000"

    TWidget* widget = find_widget(bpts_w_name);

    if (widget == nullptr) {
        bpts_w = create_empty_widget(bpts_w_name);
        display_widget(bpts_w, WOPN_DP_TAB_BEFORE | WOPN_RESTORE | WOPN_PERSIST);
    }

    return DRC_OK;
}

static drc_t idaapi term_debugger(void)
{
    if (dbg_req) {
        close_shared_mem(&dbg_req, 0);
    }

    close_widget(bpts_w, WCLS_SAVE);
    bpts_w = nullptr;

    return DRC_OK;
}

static int idaapi check_debugger_events(void *ud)
{
    while (dbg_req && (dbg_req->dbg_active == 1 || dbg_req->dbg_events_count > 0))
    {
        int event_index = recv_dbg_event_ida(dbg_req, 0);
        if (event_index == -1)
        {
            qsleep(10);
            continue;
        }

        debugger_event_t *dbg_event = &dbg_req->dbg_events[event_index];

        debug_event_t ev;
        switch (dbg_event->type)
        {
        case dbg_event_type_t::DBG_EVT_STARTED: {
            ev.pid = 1;
            ev.tid = 1;
            ev.ea = dbg_event->pc;
            ev.handled = true;

            ev.set_modinfo(PROCESS_STARTED).name.sprnt("GPGX");
            ev.set_modinfo(PROCESS_STARTED).base = 0;
            ev.set_modinfo(PROCESS_STARTED).size = 0;
            ev.set_modinfo(PROCESS_STARTED).rebase_to = BADADDR;

            g_events.enqueue(ev, IN_FRONT);
        } break;
        case dbg_event_type_t::DBG_EVT_PAUSED: {
            ev.pid = 1;
            ev.tid = 1;
            ev.ea = dbg_event->pc;
            ev.handled = true;
            ev.set_eid(PROCESS_SUSPENDED);
            g_events.enqueue(ev, IN_BACK);
        } break;
        case dbg_event_type_t::DBG_EVT_BREAK: {
            ev.pid = 1;
            ev.tid = 1;
            ev.ea = dbg_event->pc;
            ev.handled = true;
            ev.set_eid(BREAKPOINT);
            ev.set_bpt().hea = ev.set_bpt().kea = ev.ea;
            g_events.enqueue(ev, IN_BACK);
        } break;
        case dbg_event_type_t::DBG_EVT_STEP: {
            ev.pid = 1;
            ev.tid = 1;
            ev.ea = dbg_event->pc;
            ev.handled = true;
            ev.set_eid(STEP);
            g_events.enqueue(ev, IN_BACK);
        } break;
        case dbg_event_type_t::DBG_EVT_STOPPED: {
            ev.pid = 1;
            ev.handled = true;
            ev.set_exit_code(PROCESS_EXITED, 0);

            g_events.enqueue(ev, IN_BACK);
        } break;
        default:
            break;
        }

        dbg_event->type = dbg_event_type_t::DBG_EVT_NO_EVENT;
        qsleep(10);
    }

    return 0;
}

static drc_t idaapi s_start_process(const char *path,
    const char *args,
    const char *startdir,
    uint32 dbg_proc_flags,
    const char *input_path,
    uint32 input_file_crc32,
    qstring* errbuf = NULL)
{
    g_events.clear();

    dbg_req = open_shared_mem();

    if (!dbg_req)
    {
        show_wait_box("Waiting for connection to plugin...");

        while (!dbg_req)
        {
            dbg_req = open_shared_mem();

            if (user_cancelled()) {
                break;
            }

            qsleep(10);
        }

        while (dbg_req && (dbg_req->dbg_active != 1 || dbg_req->dbg_events_count == 0) && !user_cancelled()) {
            qsleep(10);
        }

        hide_wait_box();
    }

    if (dbg_req) {
        events_thread = qthread_create(check_debugger_events, NULL);

        send_dbg_request(dbg_req, request_type_t::REQ_ATTACH, 1);

        return DRC_OK;
    }

    return DRC_FAILED;
}

static drc_t idaapi prepare_to_pause_process(qstring *errbuf)
{
    pause_execution();
    return DRC_OK;
}

static drc_t idaapi emul_exit_process(qstring *errbuf)
{
    stop_debugging();
    finish_execution();

    return DRC_OK;
}

static gdecode_t idaapi get_debug_event(debug_event_t *event, int timeout_ms)
{
    while (true)
    {
        // are there any pending events?
        if (g_events.retrieve(event))
        {
            return g_events.empty() ? GDE_ONE_EVENT : GDE_MANY_EVENTS;
        }
        if (g_events.empty())
            break;
    }
    return GDE_NO_EVENT;
}

static drc_t idaapi continue_after_event(const debug_event_t *event)
{
    dbg_notification_t req = get_running_notification();
    switch (event->eid())
    {
    case STEP:
    case BREAKPOINT:
    case PROCESS_SUSPENDED:
        if (req == dbg_null || req == dbg_run_to)
            continue_execution();
    break;
    }

    return DRC_OK;
}

static drc_t idaapi s_set_resume_mode(thid_t tid, resume_mode_t resmod)
{
    switch (resmod)
    {
    case RESMOD_INTO:
        send_dbg_request(dbg_req, request_type_t::REQ_STEP_INTO, 0);
        break;
    case RESMOD_OVER:
        send_dbg_request(dbg_req, request_type_t::REQ_STEP_OVER, 0);
        break;
    }

    return DRC_OK;
}

static drc_t idaapi read_registers(thid_t tid, int clsmask, regval_t *values, qstring *errbuf)
{
    if (!dbg_req)
        return DRC_FAILED;

    if (clsmask & RC_GENERAL)
    {
        dbg_req->regs_data.type = register_type_t::REG_TYPE_M68K;
        send_dbg_request(dbg_req, request_type_t::REQ_GET_REGS, 0);

        regs_68k_data_t *reg_vals = &dbg_req->regs_data.regs_68k;

        values[(int)regs_all_t::REG_68K_D0].ival = reg_vals->d0;
        values[(int)regs_all_t::REG_68K_D1].ival = reg_vals->d1;
        values[(int)regs_all_t::REG_68K_D2].ival = reg_vals->d2;
        values[(int)regs_all_t::REG_68K_D3].ival = reg_vals->d3;
        values[(int)regs_all_t::REG_68K_D4].ival = reg_vals->d4;
        values[(int)regs_all_t::REG_68K_D5].ival = reg_vals->d5;
        values[(int)regs_all_t::REG_68K_D6].ival = reg_vals->d6;
        values[(int)regs_all_t::REG_68K_D7].ival = reg_vals->d7;

		values[(int)regs_all_t::REG_68K_A0].ival = reg_vals->a0;
		values[(int)regs_all_t::REG_68K_A1].ival = reg_vals->a1;
		values[(int)regs_all_t::REG_68K_A2].ival = reg_vals->a2;
		values[(int)regs_all_t::REG_68K_A3].ival = reg_vals->a3;
		values[(int)regs_all_t::REG_68K_A4].ival = reg_vals->a4;
		values[(int)regs_all_t::REG_68K_A5].ival = reg_vals->a5;
		values[(int)regs_all_t::REG_68K_A6].ival = reg_vals->a6;
		values[(int)regs_all_t::REG_68K_A7].ival = reg_vals->a7;

        values[(int)regs_all_t::REG_68K_PC].ival = reg_vals->pc & 0xFFFFFF;
        values[(int)regs_all_t::REG_68K_SR].ival = reg_vals->sr;
        values[(int)regs_all_t::REG_68K_SP].ival = reg_vals->sp & 0xFFFFFF;
        values[(int)regs_all_t::REG_68K_USP].ival = reg_vals->usp & 0xFFFFFF;
        values[(int)regs_all_t::REG_68K_ISP].ival = reg_vals->isp & 0xFFFFFF;
        values[(int)regs_all_t::REG_68K_PPC].ival = reg_vals->ppc & 0xFFFFFF;
        values[(int)regs_all_t::REG_68K_IR].ival = reg_vals->ir;
    }

    if (clsmask & RC_VDP)
    {
        dbg_req->regs_data.type = register_type_t::REG_TYPE_VDP;
        send_dbg_request(dbg_req, request_type_t::REQ_GET_REGS, 0);

        vdp_regs_t *vdp_regs = &dbg_req->regs_data.vdp_regs;

        for (int i = 0; i < sizeof(vdp_regs->regs_vdp) / sizeof(vdp_regs->regs_vdp[0]); ++i)
        {
            values[(int)regs_all_t::REG_VDP_00 + i].ival = vdp_regs->regs_vdp[i];
        }

        values[(int)regs_all_t::REG_VDP_DMA_LEN].ival = vdp_regs->dma_len;
        values[(int)regs_all_t::REG_VDP_DMA_SRC].ival = vdp_regs->dma_src;
        values[(int)regs_all_t::REG_VDP_DMA_DST].ival = vdp_regs->dma_dst;
    }

    if (clsmask & RC_Z80)
    {
        dbg_req->regs_data.type = register_type_t::REG_TYPE_Z80;
        send_dbg_request(dbg_req, request_type_t::REQ_GET_REGS, 0);

        regs_z80_data_t *z80_regs = &dbg_req->regs_data.regs_z80;

        for (int i = 0; i < ((int)regs_all_t::REG_Z80_I - (int)regs_all_t::REG_Z80_PC + 1); ++i)
        {
            if (i >= 0 && i <= 12) // PC <-> HL2
            {
                values[(int)regs_all_t::REG_Z80_PC + i].ival = ((unsigned int *)&z80_regs->pc)[i];
            }
            else if (i >= 13 && i <= 19) // R <-> I
            {
                values[(int)regs_all_t::REG_Z80_PC + i].ival = ((unsigned char *)&z80_regs->r)[i - 13];
            }
        }
    }

    return DRC_OK;
}

static void set_reg(register_type_t type, int reg_index, unsigned int value)
{
    dbg_req->regs_data.type = type;
    dbg_req->regs_data.any_reg.index = reg_index;
    dbg_req->regs_data.any_reg.val = value;
    send_dbg_request(dbg_req, request_type_t::REQ_SET_REG, 0);
}

static drc_t idaapi write_register(thid_t tid, int regidx, const regval_t *value, qstring *errbuf)
{
    if (regidx >= (int)regs_all_t::REG_68K_D0 && regidx <= (int)regs_all_t::REG_68K_D7)
    {
        set_reg(register_type_t::REG_TYPE_M68K, regidx - (int)regs_all_t::REG_68K_D0, (uint32)value->ival);
    }
    else if (regidx >= (int)regs_all_t::REG_68K_A0 && regidx <= (int)regs_all_t::REG_68K_A7)
    {
        set_reg(register_type_t::REG_TYPE_M68K, regidx - (int)regs_all_t::REG_68K_A0, (uint32)value->ival);
    }
    else if (regidx == (int)regs_all_t::REG_68K_PC)
    {
        set_reg(register_type_t::REG_TYPE_M68K, (int)regs_all_t::REG_68K_PC, (uint32)value->ival & 0xFFFFFF);
    }
    else if (regidx == (int)regs_all_t::REG_68K_SR)
    {
        set_reg(register_type_t::REG_TYPE_M68K, (int)regs_all_t::REG_68K_SR, (uint16)value->ival);
    }
    else if (regidx == (int)regs_all_t::REG_68K_SP)
    {
        set_reg(register_type_t::REG_TYPE_M68K, (int)regs_all_t::REG_68K_SP, (uint32)value->ival & 0xFFFFFF);
    }
    else if (regidx == (int)regs_all_t::REG_68K_USP)
    {
        set_reg(register_type_t::REG_TYPE_M68K, (int)regs_all_t::REG_68K_USP, (uint32)value->ival & 0xFFFFFF);
    }
    else if (regidx == (int)regs_all_t::REG_68K_ISP)
    {
        set_reg(register_type_t::REG_TYPE_M68K, (int)regs_all_t::REG_68K_ISP, (uint32)value->ival & 0xFFFFFF);
    }
    else if (regidx >= (int)regs_all_t::REG_VDP_00 && regidx <= (int)regs_all_t::REG_VDP_1F)
    {
        set_reg(register_type_t::REG_TYPE_VDP, regidx - (int)regs_all_t::REG_VDP_00, value->ival & 0xFF);
    }
    else if (regidx >= (int)regs_all_t::REG_Z80_PC && regidx <= (int)regs_all_t::REG_Z80_I)
    {
        set_reg(register_type_t::REG_TYPE_Z80, regidx - (int)regs_all_t::REG_Z80_PC, value->ival);
    }

    return DRC_OK;
}

static drc_t idaapi get_memory_info(meminfo_vec_t &areas, qstring *errbuf)
{
    memory_info_t info;

    // Don't remove this loop
    for (int i = 0; i < get_segm_qty(); ++i)
    {
        segment_t *segm = getnseg(i);

        info.start_ea = segm->start_ea;
        info.end_ea = segm->end_ea;

        qstring buf;
        get_segm_name(&buf, segm);
        info.name = buf;

        get_segm_class(&buf, segm);
        info.sclass = buf;

        info.sbase = 0;
        info.perm = SEGPERM_READ | SEGPERM_WRITE;
        info.bitness = 1;
        areas.push_back(info);
    }
    // Don't remove this loop

    return DRC_OK;
}

static ssize_t idaapi read_memory(ea_t ea, void *buffer, size_t size, qstring *errbuf)
{
    if ((ea >= 0xA00000 && ea < 0xA0FFFF))
    {
        dbg_req->mem_data.address = ea;
        dbg_req->mem_data.size = (int)size;
        send_dbg_request(dbg_req, request_type_t::REQ_READ_Z80, 0);

        memcpy(buffer, &dbg_req->mem_data.z80_ram[ea & 0x1FFF], size);
        // Z80
    }
    else if (ea < MAXROMSIZE)
    {
        dbg_req->mem_data.address = ea;
        dbg_req->mem_data.size = (int)size;
        send_dbg_request(dbg_req, request_type_t::REQ_READ_68K_ROM, 0);

        memcpy(buffer, &dbg_req->mem_data.m68k_rom[ea], size);
    }
    else if ((ea >= 0xFF0000 && ea < 0x1000000))
    {
        dbg_req->mem_data.address = ea;
        dbg_req->mem_data.size = (int)size;
        send_dbg_request(dbg_req, request_type_t::REQ_READ_68K_RAM, 0);

        memcpy(buffer, &dbg_req->mem_data.m68k_ram[ea & 0xFFFF], size);
        // RAM
    }

    return size;
}

static ssize_t idaapi write_memory(ea_t ea, const void *buffer, size_t size, qstring *errbuf)
{
    return 0;
}

static int idaapi is_ok_bpt(bpttype_t type, ea_t ea, int len)
{
    switch (type)
    {
        //case BPT_SOFT:
    case BPT_EXEC:
    case BPT_READ: // there is no such constant in sdk61
    case BPT_WRITE:
    case BPT_RDWR:
        return BPT_OK;
    }

    return BPT_BAD_TYPE;
}

static drc_t idaapi update_bpts(int* nbpts, update_bpt_info_t *bpts, int nadd, int ndel, qstring *errbuf)
{
    for (int i = 0; i < nadd; ++i)
    {
        ea_t start = bpts[i].ea;
        ea_t end = bpts[i].ea + bpts[i].size - 1;
        
        bpt_data_t *bpt_data = &dbg_req->bpt_data;
        bpt_data->type = idaBptToGx(bpts[i].type);

        bpt_data->address = start;
        bpt_data->width = bpts[i].size;
        send_dbg_request(dbg_req, request_type_t::REQ_ADD_BREAK, 0);

        bpts[i].code = BPT_OK;
    }

    for (int i = 0; i < ndel; ++i)
    {
        ea_t start = bpts[nadd + i].ea;
        ea_t end = bpts[nadd + i].ea + bpts[nadd + i].size - 1;

        bpt_data_t *bpt_data = &dbg_req->bpt_data;

        switch (bpts[nadd + i].type)
        {
        case BPT_EXEC:
            bpt_data->type = bpt_type_t::BPT_M68K_E;
            break;
        case BPT_READ:
            bpt_data->type = bpt_type_t::BPT_M68K_R;
            break;
        case BPT_WRITE:
            bpt_data->type = bpt_type_t::BPT_M68K_W;
            break;
        case BPT_RDWR:
            bpt_data->type = bpt_type_t::BPT_M68K_RW;
            break;
        }

        bpt_data->address = start;
        send_dbg_request(dbg_req, request_type_t::REQ_DEL_BREAK, 0);

        bpts[nadd + i].code = BPT_OK;
    }

    *nbpts = (ndel + nadd);
    return DRC_OK;
}

static drc_t s_get_processes(procinfo_vec_t* procs, qstring* errbuf) {
    process_info_t info;
    info.name.sprnt("gpgx");
    info.pid = 1;
    procs->add(info);

    return DRC_OK;
}

static ssize_t idaapi idd_notify(void* , int msgid, va_list va) {
    drc_t retcode = DRC_NONE;
    qstring* errbuf;

    switch (msgid)
    {
    case debugger_t::ev_init_debugger:
    {
        const char* hostname = va_arg(va, const char*);

        int portnum = va_arg(va, int);
        const char* password = va_arg(va, const char*);
        errbuf = va_arg(va, qstring*);
        QASSERT(1522, errbuf != NULL);
        retcode = init_debugger(hostname, portnum, password, errbuf);
    }
    break;

    case debugger_t::ev_term_debugger:
        retcode = term_debugger();
        break;

    case debugger_t::ev_get_processes:
    {
        procinfo_vec_t* procs = va_arg(va, procinfo_vec_t*);
        errbuf = va_arg(va, qstring*);
        retcode = s_get_processes(procs, errbuf);
    }
    break;

    case debugger_t::ev_start_process:
    {
        const char* path = va_arg(va, const char*);
        const char* args = va_arg(va, const char*);
        const char* startdir = va_arg(va, const char*);
        uint32 dbg_proc_flags = va_arg(va, uint32);
        const char* input_path = va_arg(va, const char*);
        uint32 input_file_crc32 = va_arg(va, uint32);
        errbuf = va_arg(va, qstring*);
        retcode = s_start_process(path,
            args,
            startdir,
            dbg_proc_flags,
            input_path,
            input_file_crc32,
            errbuf);
    }
    break;

    //case debugger_t::ev_attach_process:
    //{
    //    pid_t pid = va_argi(va, pid_t);
    //    int event_id = va_arg(va, int);
    //    uint32 dbg_proc_flags = va_arg(va, uint32);
    //    errbuf = va_arg(va, qstring*);
    //    retcode = s_attach_process(pid, event_id, dbg_proc_flags, errbuf);
    //}
    //break;

    //case debugger_t::ev_detach_process:
    //    retcode = g_dbgmod.dbg_detach_process();
    //    break;

    case debugger_t::ev_get_debapp_attrs:
    {
        debapp_attrs_t* out_pattrs = va_arg(va, debapp_attrs_t*);
        out_pattrs->addrsize = 4;
        out_pattrs->is_be = true;
        out_pattrs->platform = "sega_md";
        out_pattrs->cbsize = sizeof(debapp_attrs_t);
        retcode = DRC_OK;
    }
    break;

    //case debugger_t::ev_rebase_if_required_to:
    //{
    //    ea_t new_base = va_arg(va, ea_t);
    //    retcode = DRC_OK;
    //}
    //break;

    case debugger_t::ev_request_pause:
        errbuf = va_arg(va, qstring*);
        retcode = prepare_to_pause_process(errbuf);
        break;

    case debugger_t::ev_exit_process:
        errbuf = va_arg(va, qstring*);
        retcode = emul_exit_process(errbuf);
        break;

    case debugger_t::ev_get_debug_event:
    {
        gdecode_t* code = va_arg(va, gdecode_t*);
        debug_event_t* event = va_arg(va, debug_event_t*);
        int timeout_ms = va_arg(va, int);
        *code = get_debug_event(event, timeout_ms);
        retcode = DRC_OK;
    }
    break;

    case debugger_t::ev_resume:
    {
        debug_event_t* event = va_arg(va, debug_event_t*);
        retcode = continue_after_event(event);
    }
    break;

    //case debugger_t::ev_set_exception_info:
    //{
    //    exception_info_t* info = va_arg(va, exception_info_t*);
    //    int qty = va_arg(va, int);
    //    g_dbgmod.dbg_set_exception_info(info, qty);
    //    retcode = DRC_OK;
    //}
    //break;

    //case debugger_t::ev_suspended:
    //{
    //    bool dlls_added = va_argi(va, bool);
    //    thread_name_vec_t* thr_names = va_arg(va, thread_name_vec_t*);
    //    retcode = DRC_OK;
    //}
    //break;

    case debugger_t::ev_thread_suspend:
    {
        thid_t tid = va_argi(va, thid_t);
        pause_execution();
        retcode = DRC_OK;
    }
    break;

    case debugger_t::ev_thread_continue:
    {
        thid_t tid = va_argi(va, thid_t);
        continue_execution();
        retcode = DRC_OK;
    }
    break;

    case debugger_t::ev_set_resume_mode:
    {
        thid_t tid = va_argi(va, thid_t);
        resume_mode_t resmod = va_argi(va, resume_mode_t);
        retcode = s_set_resume_mode(tid, resmod);
    }
    break;

    case debugger_t::ev_read_registers:
    {
        thid_t tid = va_argi(va, thid_t);
        int clsmask = va_arg(va, int);
        regval_t* values = va_arg(va, regval_t*);
        errbuf = va_arg(va, qstring*);
        retcode = read_registers(tid, clsmask, values, errbuf);
    }
    break;

    case debugger_t::ev_write_register:
    {
        thid_t tid = va_argi(va, thid_t);
        int regidx = va_arg(va, int);
        const regval_t* value = va_arg(va, const regval_t*);
        errbuf = va_arg(va, qstring*);
        retcode = write_register(tid, regidx, value, errbuf);
    }
    break;

    case debugger_t::ev_get_memory_info:
    {
        meminfo_vec_t* ranges = va_arg(va, meminfo_vec_t*);
        errbuf = va_arg(va, qstring*);
        retcode = get_memory_info(*ranges, errbuf);
    }
    break;

    case debugger_t::ev_read_memory:
    {
        size_t* nbytes = va_arg(va, size_t*);
        ea_t ea = va_arg(va, ea_t);
        void* buffer = va_arg(va, void*);
        size_t size = va_arg(va, size_t);
        errbuf = va_arg(va, qstring*);
        ssize_t code = read_memory(ea, buffer, size, errbuf);
        *nbytes = code >= 0 ? code : 0;
        retcode = code >= 0 ? DRC_OK : DRC_NOPROC;
    }
    break;

    case debugger_t::ev_write_memory:
    {
        size_t* nbytes = va_arg(va, size_t*);
        ea_t ea = va_arg(va, ea_t);
        const void* buffer = va_arg(va, void*);
        size_t size = va_arg(va, size_t);
        errbuf = va_arg(va, qstring*);
        ssize_t code = write_memory(ea, buffer, size, errbuf);
        *nbytes = code >= 0 ? code : 0;
        retcode = code >= 0 ? DRC_OK : DRC_NOPROC;
    }
    break;

    case debugger_t::ev_check_bpt:
    {
        int* bptvc = va_arg(va, int*);
        bpttype_t type = va_argi(va, bpttype_t);
        ea_t ea = va_arg(va, ea_t);
        int len = va_arg(va, int);
        *bptvc = is_ok_bpt(type, ea, len);
        retcode = DRC_OK;
    }
    break;

    case debugger_t::ev_update_bpts:
    {
        int* nbpts = va_arg(va, int*);
        update_bpt_info_t* bpts = va_arg(va, update_bpt_info_t*);
        int nadd = va_arg(va, int);
        int ndel = va_arg(va, int);
        errbuf = va_arg(va, qstring*);
        retcode = update_bpts(nbpts, bpts, nadd, ndel, errbuf);
    }
    break;

    //case debugger_t::ev_update_lowcnds:
    //{
    //    int* nupdated = va_arg(va, int*);
    //    const lowcnd_t* lowcnds = va_arg(va, const lowcnd_t*);
    //    int nlowcnds = va_arg(va, int);
    //    errbuf = va_arg(va, qstring*);
    //    retcode = g_dbgmod.dbg_update_lowcnds(nupdated, lowcnds, nlowcnds, errbuf);
    //}
    //break;

#ifdef HAVE_UPDATE_CALL_STACK
    case debugger_t::ev_update_call_stack:
    {
        thid_t tid = va_argi(va, thid_t);
        call_stack_t* trace = va_arg(va, call_stack_t*);
        retcode = g_dbgmod.dbg_update_call_stack(tid, trace);
    }
    break;
#endif

    //case debugger_t::ev_eval_lowcnd:
    //{
    //    thid_t tid = va_argi(va, thid_t);
    //    ea_t ea = va_arg(va, ea_t);
    //    errbuf = va_arg(va, qstring*);
    //    retcode = g_dbgmod.dbg_eval_lowcnd(tid, ea, errbuf);
    //}
    //break;

    //case debugger_t::ev_bin_search:
    //{
    //    ea_t* ea = va_arg(va, ea_t*);
    //    ea_t start_ea = va_arg(va, ea_t);
    //    ea_t end_ea = va_arg(va, ea_t);
    //    const compiled_binpat_vec_t* ptns = va_arg(va, const compiled_binpat_vec_t*);
    //    int srch_flags = va_arg(va, int);
    //    errbuf = va_arg(va, qstring*);
    //    if (ptns != NULL)
    //        retcode = g_dbgmod.dbg_bin_search(ea, start_ea, end_ea, *ptns, srch_flags, errbuf);
    //}
    //break;
    default:
        retcode = DRC_NONE;
    }

    return retcode;
}

//--------------------------------------------------------------------------
//
//	  DEBUGGER DESCRIPTION BLOCK
//
//--------------------------------------------------------------------------

debugger_t debugger =
{
    IDD_INTERFACE_VERSION,
    "GXIDA",
    0x8000 + 1,
    "m68k",
    DBG_FLAG_NOHOST | DBG_FLAG_CAN_CONT_BPT | DBG_FLAG_FAKE_ATTACH | DBG_FLAG_SAFE | DBG_FLAG_NOPASSWORD | DBG_FLAG_NOSTARTDIR | DBG_FLAG_NOPARAMETERS | DBG_FLAG_ANYSIZE_HWBPT | DBG_FLAG_DEBTHREAD | DBG_FLAG_PREFER_SWBPTS,
    DBG_HAS_GET_PROCESSES | DBG_HAS_REQUEST_PAUSE | DBG_HAS_SET_RESUME_MODE | DBG_HAS_CHECK_BPT | DBG_HAS_THREAD_SUSPEND | DBG_HAS_THREAD_CONTINUE,

    register_classes,
    RC_GENERAL,
    registers,
    qnumber(registers),

    0x1000,

    NULL,
    0,
    0,

    DBG_RESMOD_STEP_INTO | DBG_RESMOD_STEP_OVER,

    NULL, // set_dbg_options
    idd_notify
};