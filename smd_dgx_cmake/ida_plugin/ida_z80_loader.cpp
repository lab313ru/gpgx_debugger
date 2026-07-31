// "Dump Z80 driver" — the bridge between the two databases.
//
// Pulling the driver out of the ROM is the obvious approach and the wrong one:
// drivers are routinely compressed, relocated, or assembled from pieces, so
// what the ROM holds is not what the Z80 executes. The running machine has
// already done all of that work, so we take the 8K of sound RAM as it stands
// and write it out. That file is what the Z80 loader opens.

#include <vector>
#include <string>

#include <ida.hpp>
#include <idp.hpp>
#include <kernwin.hpp>
#include <diskio.hpp>
#include <loader.hpp>

#include "debugger/EmuHost.h"

EmuHost* smd_dgx_host();                                 // smd_dgx_ida.cpp
bool     smd_dgx_z80_dump_ram(std::vector<uint8_t>&);    // smd_dgx_ida_z80.cpp

namespace {

// Works from either side: in the 68000 database the emulator is right here, in
// the Z80 one it is across the socket.
bool grab_z80_ram(std::vector<uint8_t>& out)
{
    if (EmuHost* h = smd_dgx_host()) {
        if (h->isRunning()) {
            out = h->backend()->readZ80Memory(0, 0x2000);
            return out.size() == 0x2000;
        }
    }
    return smd_dgx_z80_dump_ram(out);
}

struct dump_z80_ah_t : public action_handler_t {
    int idaapi activate(action_activation_ctx_t*) override
    {
        std::vector<uint8_t> ram;
        if (!grab_z80_ram(ram)) {
            warning("SMD DGX: no running emulator to dump the Z80 driver from.\n\n"
                    "Start the game first — the driver is only assembled in RAM\n"
                    "once it is running.");
            return 0;
        }

        // An all-zero (or all-FF) dump means the Z80 was never handed a driver:
        // saying so beats handing the user 8K of nothing to disassemble.
        bool blank = true;
        for (size_t i = 1; i < ram.size(); ++i)
            if (ram[i] != ram[0]) { blank = false; break; }
        if (blank) {
            warning("SMD DGX: Z80 RAM is uniformly $%02X — no driver has been\n"
                    "uploaded yet. Let the game reach its sound init and retry.",
                    ram[0]);
            return 0;
        }

        qstring def;
        def.sprnt("%s_z80.bin", get_path(PATH_TYPE_IDB));
        char* path = ask_file(true, def.c_str(), "Save Z80 driver dump");
        if (path == nullptr || path[0] == '\0') return 0;

        FILE* f = qfopen(path, "wb");
        if (f == nullptr) {
            warning("SMD DGX: cannot write %s", path);
            return 0;
        }
        const bool ok = qfwrite(f, ram.data(), ram.size()) == (ssize_t)ram.size();
        qfclose(f);

        if (!ok) {
            warning("SMD DGX: short write to %s", path);
            return 0;
        }
        msg("SMD DGX: wrote 8192 bytes of Z80 RAM to %s\n"
            "         open it in a second IDA instance with the Z80 driver loader,\n"
            "         then start the debugger there to attach to this emulator.\n", path);
        return 1;
    }

    action_state_t idaapi update(action_update_ctx_t*) override
    {
        return AST_ENABLE_ALWAYS;
    }
};

dump_z80_ah_t g_dump_z80_ah;
const char kDumpAction[] = "smd_dgx:dump_z80";
bool g_registered = false;

} // namespace

void smd_dgx_register_z80_dump()
{
    if (g_registered) return;
    action_desc_t desc = ACTION_DESC_LITERAL_PLUGMOD(
        kDumpAction, "Dump Z80 driver...", &g_dump_z80_ah, nullptr, nullptr,
        "Write the running sound driver out of Z80 RAM", -1);
    register_action(desc);
    attach_action_to_menu("Debugger/SMD patches/", kDumpAction, SETMENU_APP);
    g_registered = true;
}

void smd_dgx_unregister_z80_dump()
{
    if (!g_registered) return;
    detach_action_from_menu("Debugger/SMD patches/", kDumpAction);
    unregister_action(kDumpAction);
    g_registered = false;
}

// The Z80 database gets the same action: re-dumping after the driver swaps
// banks or loads a new song is the normal way to catch overlaid code.
void smd_dgx_register_z80_actions()   { smd_dgx_register_z80_dump(); }
void smd_dgx_unregister_z80_actions() { smd_dgx_unregister_z80_dump(); }
