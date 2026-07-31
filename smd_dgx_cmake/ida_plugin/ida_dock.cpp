// IDA-side glue: dockable view windows, layout presets and save-state actions.
// Includes IDA headers ONLY — no Qt (see ida_views_shared.h for why the two
// must stay in separate TUs).
//
// VERIFY-9.3 markers: check these APIs against the installed 9.3 SDK.

#include <ida.hpp>
#include <kernwin.hpp>
#include <loader.hpp>
#include <nalt.hpp>

#include "debugger/EmuHost.h"        // no Qt in this header
#include "ida_views_shared.h"

extern EmuHost* smd_dgx_host();      // smd_dgx_ida.cpp

namespace {

// View indices, in the order of smd_dgx_view_titles.
enum {
    V_SCREEN = 0, V_VDPRAM, V_VDPREG, V_SCROLL, V_STATES, V_SPRITES, V_PLANE,
    V_SOUND, V_HEX, V_SEARCH, V_WATCH,
};

void open_view(int idx)
{
    const char* title = smd_dgx_view_titles[idx];
    if (TWidget* existing = find_widget(title)) {
        activate_widget(existing, true);               // VERIFY-9.3
        return;
    }
    TWidget* tw = create_empty_widget(title);          // VERIFY-9.3
    if (!tw) return;
    // The empty widget's TWidget* is its QWidget* in IDA's Qt build.
    smd_dgx_view_attach(idx, (void*)tw);
    display_widget(tw, WOPN_DP_RIGHT | WOPN_PERSIST | WOPN_RESTORE);  // VERIFY-9.3
}

struct open_view_ah_t : public action_handler_t {
    int idx;
    explicit open_view_ah_t(int i) : idx(i) {}
    int idaapi activate(action_activation_ctx_t*) override { open_view(idx); return 1; }
    action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
};

// ---------------------------------------------------------------------------
// Layout presets
//
// Nine docks is a lot to arrange by hand, and the useful groupings are known:
// a preset opens exactly the views one job needs and tabs the related ones
// together. `tabbed` views share a tab bar with the first one; `bottom` goes
// underneath it.
// ---------------------------------------------------------------------------
struct Preset {
    const char* name;
    int  anchor;              // docked right of the disassembly, gets focus
    int  tabbed[4];           // tabbed with anchor, -1 terminated
    int  bottom;              // docked below anchor, or -1
};

const Preset kPresets[] = {
    { "Graphics && VDP", V_VDPRAM, { V_VDPREG, V_SPRITES, V_PLANE, -1 }, V_SCREEN },
    { "Scrolling",       V_SCROLL, { V_VDPREG, V_PLANE, -1, -1 },      V_SCREEN },
    { "Sound",           V_SOUND,  { -1, -1, -1, -1 },                   V_SCREEN },
    { "Memory",          V_HEX,    { V_SEARCH, V_WATCH, -1, -1 },        V_SCREEN },
    { "Everything",      V_VDPRAM, { V_VDPREG, V_SCROLL, V_PLANE, V_SOUND }, V_HEX },
};
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

void apply_preset(const Preset& p)
{
    open_view(p.anchor);
    const char* anchorTitle = smd_dgx_view_titles[p.anchor];
    set_dock_pos(anchorTitle, "IDA View-A", DP_RIGHT);          // VERIFY-9.3

    for (int i = 0; i < 4 && p.tabbed[i] >= 0; ++i) {
        open_view(p.tabbed[i]);
        set_dock_pos(smd_dgx_view_titles[p.tabbed[i]], anchorTitle, DP_TAB);
    }
    if (p.bottom >= 0) {
        open_view(p.bottom);
        set_dock_pos(smd_dgx_view_titles[p.bottom], anchorTitle, DP_BOTTOM);
    }
    // IDA's own register window belongs next to the VDP state — that pairing is
    // most of what one looks at while stepping. It only exists while debugging,
    // and set_dock_pos on a missing widget is a no-op, so this is safe either way.
    set_dock_pos("General registers", anchorTitle, DP_TAB);
    // Leave the anchor in front rather than whichever tab was added last.
    if (TWidget* w = find_widget(anchorTitle))
        activate_widget(w, true);
    // The screen is the one that needs keyboard focus to take controller input.
    if (TWidget* w = find_widget(smd_dgx_view_titles[V_SCREEN]))
        activate_widget(w, true);
}

struct preset_ah_t : public action_handler_t {
    int idx;
    explicit preset_ah_t(int i) : idx(i) {}
    int idaapi activate(action_activation_ctx_t*) override { apply_preset(kPresets[idx]); return 1; }
    action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
};

// ---------------------------------------------------------------------------
// Save states
//
// Slots live next to the database as <idb-base>.gp<N>, so they follow the
// project around and never collide between ROMs.
// ---------------------------------------------------------------------------
int g_slot = 0;

qstring state_path(int slot)
{
    qstring base = get_path(PATH_TYPE_IDB);
    if (base.empty()) {
        char buf[QMAXPATH] = { 0 };
        get_input_file_path(buf, sizeof buf);
        base = buf;
    }
    if (base.empty()) return qstring();

    // strip the extension, if any, from the last path component
    const size_t slash = base.rfind('/');
    const size_t bslash = base.rfind('\\');
    const size_t cut = (slash == qstring::npos) ? bslash
                     : (bslash == qstring::npos) ? slash
                     : (slash > bslash ? slash : bslash);
    const size_t dot = base.rfind('.');
    if (dot != qstring::npos && (cut == qstring::npos || dot > cut))
        base.resize(dot);

    qstring out;
    out.sprnt("%s.gp%d", base.c_str(), slot);
    return out;
}

void do_state(bool save)
{
    EmuHost* host = smd_dgx_host();
    if (!host || !host->isRunning()) {
        warning("SMD DGX: the emulator is not running.");
        return;
    }
    const qstring path = state_path(g_slot);
    if (path.empty()) { warning("SMD DGX: cannot determine a save-state path."); return; }

    bool ok = false;
    // Snapshotting alongside a running core would tear; invoke() runs it on
    // the emulation thread (it drains while paused too).
    if (!host->invoke([&] {
            ok = save ? host->backend()->saveState(path.c_str())
                      : host->backend()->loadState(path.c_str());
        })) {
        warning("SMD DGX: the emulator did not respond.");
        return;
    }
    if (ok)
        msg("SMD DGX: %s slot %d (%s)\n", save ? "saved" : "loaded", g_slot, path.c_str());
    else
        warning("SMD DGX: failed to %s slot %d.\n%s",
                save ? "save" : "load", g_slot, path.c_str());
}

struct state_ah_t : public action_handler_t {
    bool save;
    explicit state_ah_t(bool s) : save(s) {}
    int idaapi activate(action_activation_ctx_t*) override { do_state(save); return 1; }
    action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
};

struct slot_ah_t : public action_handler_t {
    int slot;
    explicit slot_ah_t(int s) : slot(s) {}
    int idaapi activate(action_activation_ctx_t*) override {
        g_slot = slot;
        msg("SMD DGX: save-state slot %d selected\n", slot);
        return 1;
    }
    action_state_t idaapi update(action_update_ctx_t*) override { return AST_ENABLE_ALWAYS; }
};

// ---------------------------------------------------------------------------
constexpr int kSlotCount = 10;

open_view_ah_t* g_view_handlers[SMD_DGX_VIEW_COUNT] = {};
preset_ah_t*    g_preset_handlers[kPresetCount] = {};
state_ah_t*     g_save_handler = nullptr;
state_ah_t*     g_load_handler = nullptr;
slot_ah_t*      g_slot_handlers[kSlotCount] = {};

char g_view_names[SMD_DGX_VIEW_COUNT][32];
char g_preset_names[kPresetCount][32];
char g_slot_names[kSlotCount][32];
char g_slot_labels[kSlotCount][16];

void add_action(const char* name, const char* label, action_handler_t* h,
                const char* menu, const char* shortcut = nullptr)
{
    action_desc_t desc = ACTION_DESC_LITERAL_PLUGMOD(      // VERIFY-9.3
        name, label, h, nullptr, shortcut, nullptr, -1);
    register_action(desc);
    attach_action_to_menu(menu, name, SETMENU_APP);
}

} // namespace

void smd_dgx_register_views()
{
#ifdef SMD_DGX_IDA_VIEWS
    // Same base name the numbered slots use, but a directory: the manager keeps
    // several files plus its index there.
    {
        qstring base = state_path(0);          // <idb-base>.gp0
        const size_t dot = base.rfind('.');
        if (dot != qstring::npos) base.resize(dot);
        base.append("_states");
        smd_dgx_set_states_dir(base.c_str());
    }
#endif
    for (int i = 0; i < SMD_DGX_VIEW_COUNT; ++i) {
        qsnprintf(g_view_names[i], sizeof(g_view_names[i]), "smd_dgx:view%d", i);
        g_view_handlers[i] = new open_view_ah_t(i);
        add_action(g_view_names[i], smd_dgx_view_titles[i], g_view_handlers[i],
                   "Debugger/Debugger windows/");
    }

    for (int i = 0; i < kPresetCount; ++i) {
        qsnprintf(g_preset_names[i], sizeof(g_preset_names[i]), "smd_dgx:preset%d", i);
        g_preset_handlers[i] = new preset_ah_t(i);
        add_action(g_preset_names[i], kPresets[i].name, g_preset_handlers[i],
                   "Debugger/SMD layouts/");
    }

    g_save_handler = new state_ah_t(true);
    g_load_handler = new state_ah_t(false);
    add_action("smd_dgx:savestate", "Save state", g_save_handler,
               "Debugger/SMD save states/", "Ctrl-Alt-S");
    add_action("smd_dgx:loadstate", "Load state", g_load_handler,
               "Debugger/SMD save states/", "Ctrl-Alt-L");

    for (int i = 0; i < kSlotCount; ++i) {
        qsnprintf(g_slot_names[i], sizeof(g_slot_names[i]), "smd_dgx:slot%d", i);
        qsnprintf(g_slot_labels[i], sizeof(g_slot_labels[i]), "Slot %d", i);
        g_slot_handlers[i] = new slot_ah_t(i);
        add_action(g_slot_names[i], g_slot_labels[i], g_slot_handlers[i],
                   "Debugger/SMD save states/Select slot/");
    }
}

void smd_dgx_unregister_views()
{
    smd_dgx_view_detach_all();

    for (int i = 0; i < SMD_DGX_VIEW_COUNT; ++i) {
        unregister_action(g_view_names[i]);
        delete g_view_handlers[i];
        g_view_handlers[i] = nullptr;
    }
    for (int i = 0; i < kPresetCount; ++i) {
        unregister_action(g_preset_names[i]);
        delete g_preset_handlers[i];
        g_preset_handlers[i] = nullptr;
    }
    for (int i = 0; i < kSlotCount; ++i) {
        unregister_action(g_slot_names[i]);
        delete g_slot_handlers[i];
        g_slot_handlers[i] = nullptr;
    }
    unregister_action("smd_dgx:savestate");
    unregister_action("smd_dgx:loadstate");
    delete g_save_handler; g_save_handler = nullptr;
    delete g_load_handler; g_load_handler = nullptr;
}
