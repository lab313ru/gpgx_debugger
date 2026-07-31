// ROM patching: move byte patches between the IDA database and the live
// emulator, and export a patched ROM image.
//
// The point is the edit-test loop. IDA already knows how to patch bytes and
// assemble instructions into the database; what was missing is trying the
// result immediately. Pushing the database patches into the running
// emulator's ROM means a change can be heard and seen without rebuilding a
// file or restarting anything, and reverting puts the original bytes back so
// a change can be A/B'd.
//
// IDA headers only — no Qt (see ida_views_shared.h).

#include <ida.hpp>
#include <bytes.hpp>
#include <fpro.h>
#include <kernwin.hpp>
#include <loader.hpp>

#include <cstdio>
#include <vector>

#include "debugger/EmuHost.h"

extern EmuHost* smd_dgx_host();      // smd_dgx_ida.cpp

namespace {

constexpr int kRomRegion = 0;        // GpgxBackend region table

// The loader maps the cartridge 1:1 from address 0, so an ROM ea *is* its
// offset within the image.
uint32_t rom_size(IDebugBackend* be)
{
    for (const auto& r : be->getMemRegions())
        if (r.id == kRomRegion) return r.size;
    return 0;
}

IDebugBackend* live_backend()
{
    EmuHost* host = smd_dgx_host();
    if (!host || !host->isRunning()) {
        warning("SMD DGX: the emulator is not running.");
        return nullptr;
    }
    return host->backend();
}

struct patch_ctx_t {
    IDebugBackend* be;
    bool  restore;      // write the original byte instead of the patched one
    int   count;
};

int idaapi patch_cb(ea_t ea, qoff64_t /*fpos*/, uint64 orig, uint64 patched, void* ud)
{
    auto* ctx = static_cast<patch_ctx_t*>(ud);
    const uint8_t value = uint8_t((ctx->restore ? orig : patched) & 0xFF);
    if (ctx->be->writeRegion(kRomRegion, uint32_t(ea), &value, 1))
        ++ctx->count;
    return 0;                       // keep enumerating
}

void apply_patches(bool restore)
{
    IDebugBackend* be = live_backend();
    if (!be) return;

    const uint32_t size = rom_size(be);
    if (!size) { warning("SMD DGX: no ROM region."); return; }

    patch_ctx_t ctx{ be, restore, 0 };
    visit_patched_bytes(0, size, patch_cb, &ctx);

    if (ctx.count == 0)
        info("SMD DGX: the database has no patched bytes in the ROM range.");
    else
        msg("SMD DGX: %s %d byte%s %s the emulator ROM\n",
            restore ? "restored" : "applied", ctx.count, ctx.count == 1 ? "" : "s",
            restore ? "in" : "to");
}

// Export what the emulator currently holds, patches and all. Reading it back
// from the emulator rather than rebuilding it from the database means whatever
// else has written to the ROM area is captured too.
void save_rom()
{
    IDebugBackend* be = live_backend();
    if (!be) return;

    const uint32_t size = rom_size(be);
    if (!size) { warning("SMD DGX: no ROM region."); return; }

    char* path = ask_file(true, "patched.gen", "FILTER Mega Drive ROM|*.gen;*.bin;*.md\nSave patched ROM as");
    if (path == nullptr || path[0] == '\0') return;

    const std::vector<uint8_t> rom = be->readRegion(kRomRegion, 0, size);
    if (rom.size() != size) { warning("SMD DGX: could not read the ROM region."); return; }

    FILE* f = qfopen(path, "wb");
    if (f == nullptr) { warning("SMD DGX: cannot open\n%s", path); return; }
    const size_t written = qfwrite(f, rom.data(), rom.size());
    qfclose(f);

    if (written != rom.size())
        warning("SMD DGX: short write to\n%s", path);
    else
        msg("SMD DGX: wrote %u bytes to %s\n", unsigned(rom.size()), path);
}

struct patch_ah_t : public action_handler_t {
    int mode;    // 0 apply, 1 restore, 2 save
    explicit patch_ah_t(int m) : mode(m) {}
    int idaapi activate(action_activation_ctx_t*) override
    {
        if (mode == 2) save_rom();
        else           apply_patches(mode == 1);
        return 1;
    }
    action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
};

patch_ah_t* g_handlers[3] = {};

const char* const kNames[3] = {
    "smd_dgx:patch_apply", "smd_dgx:patch_restore", "smd_dgx:patch_save",
};
const char* const kLabels[3] = {
    "Apply database patches to emulator",
    "Restore original bytes in emulator",
    "Save patched ROM as...",
};
const char* const kKeys[3] = { "Ctrl-Alt-P", nullptr, nullptr };

} // namespace

void smd_dgx_register_patches()
{
    for (int i = 0; i < 3; ++i) {
        g_handlers[i] = new patch_ah_t(i);
        action_desc_t desc = ACTION_DESC_LITERAL_PLUGMOD(
            kNames[i], kLabels[i], g_handlers[i], nullptr, kKeys[i], nullptr, -1);
        register_action(desc);
        attach_action_to_menu("Debugger/SMD patches/", kNames[i], SETMENU_APP);
    }
}

void smd_dgx_unregister_patches()
{
    for (int i = 0; i < 3; ++i) {
        unregister_action(kNames[i]);
        delete g_handlers[i];
        g_handlers[i] = nullptr;
    }
}
