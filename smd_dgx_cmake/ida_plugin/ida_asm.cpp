// Listing readability: decode the magic constants Mega Drive code is full of.
//
// A VDP access looks like `move.l #$40000003,(a5)` — the constant encodes a
// target (VRAM/CRAM/VSRAM), a direction, a DMA flag and a 16-bit address, all
// bit-packed across two words in a deliberately awkward order. Register writes
// (`#$8144`), Z80 bus handshakes and SR masks are the same story. Reading them
// by hand is where most of the time goes when reversing a Mega Drive ROM, so
// put the meaning in a comment.
//
// Manual (hotkey J on the instruction, or over a selection) rather than
// automatic: an immediate only *looks* like a VDP command, and silently
// commenting every 32-bit constant in a ROM would bury real analysis in noise.
//
// IDA headers only — no Qt.

#include <ida.hpp>
#include <bytes.hpp>
#include <idp.hpp>
#include <kernwin.hpp>
#include <lines.hpp>
#include <ua.hpp>
#include <xref.hpp>

#include "ida_registers.h"      // M68K_linea / M68K_linef

namespace {

// VDP registers 0x00-0x17, same short names the register view uses.
const char* const kVdpRegNames[24] = {
    "Mode1", "Mode2", "PlaneA", "Window", "PlaneB", "SpriteTable", "SpritePatGen",
    "BgColor", "Reg8", "Reg9", "HInt", "Mode3", "Mode4", "HScrollTable", "Reg14",
    "AutoInc", "ScrollSize", "WindowX", "WindowY", "DmaLenLo", "DmaLenHi",
    "DmaSrcLo", "DmaSrcMid", "DmaSrcHi",
};

// CD5..CD0 -> what the access actually does. CD1-CD0 come from the first word,
// CD5-CD2 from the second; CD5 alone means DMA.
const char* cd_target(uint32_t cd, bool* isDma)
{
    *isDma = (cd & 0x20) != 0;
    switch (cd & 0x0F) {
    case 0x0: return "VRAM read";
    case 0x1: return "VRAM write";
    case 0x3: return "CRAM write";
    case 0x4: return "VSRAM read";
    case 0x5: return "VSRAM write";
    case 0x8: return "CRAM read";
    case 0x6: return "VRAM read (8-bit)";
    default:  return nullptr;
    }
}

// A VDP control long: first word CD1 CD0 A13..A0, second word CD5..CD2 in bits
// 7-4 and A15..A14 in bits 1-0.
bool decode_vdp_access(uint32_t value, qstring* out)
{
    const uint16_t hi = uint16_t(value >> 16);
    const uint16_t lo = uint16_t(value & 0xFFFF);

    // register writes have 100RRRRR in the top bits; not an address/command
    if ((hi & 0xE000) == 0x8000) return false;

    const uint32_t cd   = (((lo >> 4) & 0x0F) << 2) | ((hi >> 14) & 0x03);
    const uint32_t addr = (hi & 0x3FFF) | ((lo & 0x0003) << 14);

    bool dma = false;
    const char* what = cd_target(cd, &dma);
    if (what == nullptr) return false;

    out->sprnt("VDP: %s%s at $%04X", dma ? "DMA " : "", what, addr);
    return true;
}

// 100RRRRR DDDDDDDD — set VDP register RRRRR to DDDDDDDD.
bool decode_vdp_reg(uint16_t word, qstring* out)
{
    if ((word & 0xE000) != 0x8000) return false;
    const uint32_t reg = (word >> 8) & 0x1F;
    const uint32_t val = word & 0xFF;
    if (reg >= 24) return false;
    out->sprnt("VDP reg %02X (%s) = $%02X", reg, kVdpRegNames[reg], val);
    return true;
}

bool decode_z80_bus(ea_t dest, uint32_t value, qstring* out)
{
    const bool asserted = (value & 0x0100) != 0;
    if (dest == 0xA11100)
        out->sprnt("Z80 bus: %s", asserted ? "request (68k takes the bus)"
                                           : "release (Z80 runs)");
    else if (dest == 0xA11200)
        out->sprnt("Z80 reset: %s", asserted ? "released (Z80 runs)"
                                             : "asserted (Z80 held in reset)");
    else
        return false;
    return true;
}

// SR: T1 T0 S M . I2 I1 I0 | . . . X N Z V C
void decode_sr(uint32_t value, qstring* out)
{
    qstring flags;
    auto add = [&](const char* n) { if (!flags.empty()) flags.append(' '); flags.append(n); };
    if (value & 0x8000) add("T1");
    if (value & 0x4000) add("T0");
    if (value & 0x2000) add("S(supervisor)");
    if (value & 0x1000) add("M");
    if (value & 0x0010) add("X");
    if (value & 0x0008) add("N");
    if (value & 0x0004) add("Z");
    if (value & 0x0002) add("V");
    if (value & 0x0001) add("C");
    out->sprnt("SR: int mask %u%s%s", unsigned((value >> 8) & 7),
               flags.empty() ? "" : ", ", flags.c_str());
}

// Where an instruction's memory operand points, if it is a plain address.
ea_t mem_operand(const insn_t& insn)
{
    for (int i = 0; i < UA_MAXOP; ++i) {
        const op_t& op = insn.ops[i];
        if (op.type == o_void) break;
        if (op.type == o_mem) return op.addr & 0xFFFFFF;
    }
    return BADADDR;
}

bool immediate_of(const insn_t& insn, uint32_t* value, int* size)
{
    for (int i = 0; i < UA_MAXOP; ++i) {
        const op_t& op = insn.ops[i];
        if (op.type == o_void) break;
        if (op.type == o_imm) {
            *value = uint32_t(op.value);
            *size  = get_dtype_size(op.dtype);
            return true;
        }
    }
    return false;
}

// Builds the comment for one instruction; empty result = nothing recognised.
qstring describe(ea_t ea)
{
    qstring out;
    insn_t insn;
    if (decode_insn(&insn, ea) <= 0) return out;

    uint32_t value = 0;
    int size = 0;
    if (!immediate_of(insn, &value, &size)) return out;

    const ea_t dest = mem_operand(insn);

    if (dest == 0xA11100 || dest == 0xA11200) {
        if (decode_z80_bus(dest, value, &out)) return out;
    }

    // move to sr/ccr — the destination operand is the tell, not the mnemonic
    // (that is just "move.w"), so read the operand text the way IDA prints it
    for (int i = 0; i < UA_MAXOP; ++i) {
        if (insn.ops[i].type == o_void) break;
        if (insn.ops[i].type != o_reg && insn.ops[i].type < o_idpspec0) continue;
        qstring opnd;
        if (print_operand(&opnd, ea, i) <= 0) continue;
        tag_remove(&opnd, opnd);
        if (opnd == "sr" || opnd == "ccr") {
            decode_sr(value, &out);
            return out;
        }
    }

    if (size == 4) {
        // a long constant is either one address/command or two register writes
        if (decode_vdp_access(value, &out)) return out;
        qstring a, b;
        if (decode_vdp_reg(uint16_t(value >> 16), &a)
            && decode_vdp_reg(uint16_t(value & 0xFFFF), &b)) {
            out.sprnt("%s; %s", a.c_str(), b.c_str());
            return out;
        }
    } else if (size == 2) {
        if (decode_vdp_reg(uint16_t(value), &out)) return out;
    }
    return out;
}

// One instruction, or every instruction in the selection. Re-running on an
// already commented line clears it, so J toggles.
int annotate()
{
    ea_t ea1 = BADADDR, ea2 = BADADDR;
    const bool haveSel = read_range_selection(nullptr, &ea1, &ea2);
    if (!haveSel) { ea1 = get_screen_ea(); ea2 = ea1 + 1; }
    if (ea1 == BADADDR) return 0;

    int done = 0;
    for (ea_t ea = ea1; ea < ea2; ) {
        insn_t insn;
        const int len = decode_insn(&insn, ea);
        const ea_t next = (len > 0) ? ea + len : ea + 2;

        if (!haveSel) {
            qstring existing;
            if (get_cmt(&existing, ea, false) > 0) {   // toggle off
                set_cmt(ea, "", false);
                return 1;
            }
        }
        const qstring text = describe(ea);
        if (!text.empty()) { set_cmt(ea, text.c_str(), false); ++done; }
        ea = next;
    }

    if (done == 0 && !haveSel)
        msg("SMD DGX: nothing recognisable in the immediate at %a\n", ea1);
    else if (done > 0)
        msg("SMD DGX: annotated %d instruction%s\n", done, done == 1 ? "" : "s");
    return 1;
}

struct annotate_ah_t : public action_handler_t {
    int idaapi activate(action_activation_ctx_t*) override { return annotate(); }
    action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
};

annotate_ah_t* g_handler = nullptr;
const char* const kName = "smd_dgx:identify_const";

// ---------------------------------------------------------------------------
// A-line / F-line traps
//
// $Axxx and $Fxxx are illegal on a 68000 and vector through 10 and 11. Games
// use them as syscalls — the trap handler reads the low byte as a routine
// number — but the processor module just sees an invalid instruction and stops
// following the code there, which strands everything after the call. Decoding
// them as two-byte instructions that flow on (and reference the handler)
// keeps the analysis going.
//
// Only $A0xx/$F0xx, matching Gensida: every $Axxx is technically a trap, but
// data misidentified as code is full of them, and widening this turns stray
// bytes into fake instructions.
// ---------------------------------------------------------------------------
constexpr ea_t kVectorLineA = 0x0A * 4;   // vector 10
constexpr ea_t kVectorLineF = 0x0B * 4;   // vector 11

struct idp_listener_t : public event_listener_t {
    ssize_t idaapi on_event(ssize_t code, va_list va) override
    {
        switch (code) {
        case processor_t::ev_ana_insn: {
            insn_t* insn = va_arg(va, insn_t*);
            const uint8_t hi = get_byte(insn->ea);
            if (hi != 0xA0 && hi != 0xF0) break;

            insn->itype = (hi == 0xA0) ? M68K_linea : M68K_linef;
            insn->size  = 2;

            // operand 1: the handler this traps into
            insn->Op1.type  = o_near;
            insn->Op1.dtype = dt_dword;
            insn->Op1.offb  = 1;
            insn->Op1.addr  = get_dword((hi == 0xA0) ? kVectorLineA : kVectorLineF) & 0xFFFFFF;

            // operand 2: the routine number the handler dispatches on
            insn->Op2.type  = o_imm;
            insn->Op2.dtype = dt_byte;
            insn->Op2.offb  = 1;
            insn->Op2.value = get_byte(insn->ea + 1);
            return insn->size;
        }
        case processor_t::ev_emu_insn: {
            const insn_t* insn = va_arg(va, const insn_t*);
            if (insn->itype != M68K_linea && insn->itype != M68K_linef) break;
            insn->add_cref(insn->Op1.addr, 0, fl_CN);              // into the handler
            insn->add_cref(insn->ea + insn->size, insn->Op1.offb, fl_F);  // and onwards
            return 1;
        }
        case processor_t::ev_out_mnem: {
            outctx_t* ctx = va_arg(va, outctx_t*);
            if (ctx->insn.itype != M68K_linea && ctx->insn.itype != M68K_linef) break;
            ctx->out_custom_mnem(ctx->insn.itype == M68K_linea ? "line_a" : "line_f");
            return 1;
        }
        default:
            break;
        }
        return 0;
    }
};

idp_listener_t g_idp;

} // namespace

void smd_dgx_register_asm()
{
    g_handler = new annotate_ah_t;
    action_desc_t desc = ACTION_DESC_LITERAL_PLUGMOD(
        kName, "Identify SMD constant", g_handler, nullptr, "J", nullptr, -1);
    register_action(desc);
    attach_action_to_menu("Edit/Other/", kName, SETMENU_APP);

    hook_event_listener(HT_IDP, &g_idp, nullptr);
}

void smd_dgx_unregister_asm()
{
    unhook_event_listener(HT_IDP, &g_idp);

    unregister_action(kName);
    delete g_handler;
    g_handler = nullptr;
}
