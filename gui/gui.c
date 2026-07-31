#include "gui.h"
#include "plane_explorer.h"
#include "vdp_ram_debug.h"
#include "hex_editor.h"
#include "disassembler.h"
#include "debug.h"

HWND dbg_window = NULL;
HINSTANCE dbg_wnd_hinst = NULL;

static ACTCTX actCtx;
static HANDLE hActCtx;
static ULONG_PTR cookie;

HINSTANCE GetHInstance()
{
    MEMORY_BASIC_INFORMATION mbi;
    SetLastError(ERROR_SUCCESS);
    VirtualQuery(GetHInstance, &mbi, sizeof(mbi));

    return (HINSTANCE)mbi.AllocationBase;
}

static void enable_visual_styles()
{
    ZeroMemory(&actCtx, sizeof(actCtx));
    actCtx.cbSize = sizeof(actCtx);
    actCtx.hModule = dbg_wnd_hinst;
    actCtx.lpResourceName = MAKEINTRESOURCE(2);
    actCtx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;

    hActCtx = CreateActCtx(&actCtx);
    if (hActCtx != INVALID_HANDLE_VALUE) {
        ActivateActCtx(hActCtx, &cookie);
    }
}

static void disable_visual_styles()
{
    DeactivateActCtx(0, cookie);
    ReleaseActCtx(hActCtx);
}

const COLORREF normal_pal[] =
{
    0x00000000,
    0x00000011,
    0x00000022,
    0x00000033,
    0x00000044,
    0x00000055,
    0x00000066,
    0x00000077,
    0x00000088,
    0x00000099,
    0x000000AA,
    0x000000BB,
    0x000000CC,
    0x000000DD,
    0x000000EE,
    0x000000FF
};

unsigned short cram_9b_to_16b(unsigned short data)
{
    /* Unpack 9-bit CRAM data (BBBGGGRRR) to 16-bit data (BBB0GGG0RRR0) */
    return ((data & 0x1C0) << 3) | ((data & 0x038) << 2) | ((data & 0x007) << 1);
}

unsigned short cram_16b_to_9b(unsigned short data)
{
    return ((data & 0xE00) >> 3) | ((data & 0x0E0) >> 2) | ((data & 0x00E) >> 1);
}

int select_file_save(char *Dest, const char *Dir, const char *Titre, const char *Filter, const char *Ext, HWND hwnd)
{
    OPENFILENAME ofn;

    if (!strcmp(Dest, ""))
    {
        strcpy(Dest, "default.");
        strcat(Dest, Ext);
    }

    memset(&ofn, 0, sizeof(OPENFILENAME));

    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = hwnd;
    ofn.hInstance = dbg_wnd_hinst;
    ofn.lpstrFile = Dest;
    ofn.nMaxFile = 2047;
    ofn.lpstrFilter = Filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = Dir;
    ofn.lpstrTitle = Titre;
    ofn.lpstrDefExt = Ext;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (GetSaveFileName(&ofn)) return 1;

    return 0;
}

int select_file_load(char *Dest, const char *Dir, const char *Titre, const char *Filter, const char *Ext, HWND hwnd)
{
    OPENFILENAME ofn;

    if (!strcmp(Dest, ""))
    {
        strcpy(Dest, "default.");
        strcat(Dest, Ext);
    }

    memset(&ofn, 0, sizeof(OPENFILENAME));

    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = hwnd;
    ofn.hInstance = dbg_wnd_hinst;
    ofn.lpstrFile = Dest;
    ofn.nMaxFile = 2047;
    ofn.lpstrFilter = Filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = Dir;
    ofn.lpstrTitle = Titre;
    ofn.lpstrDefExt = Ext;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;

    if (GetOpenFileName(&ofn)) return 1;

    return 0;
}

void run_gui()
{
    dbg_wnd_hinst = GetHInstance();

    enable_visual_styles();

    create_plane_explorer();
    create_vdp_ram_debug();
    create_hex_editor();
    //create_disassembler();
}

void update_gui()
{
    update_plane_explorer();
    update_vdp_ram_debug();
    update_hex_editor();
    handle_request();
    //update_disassembler();
}

void stop_gui()
{
    destroy_plane_explorer();
    destroy_vdp_ram_debug();
    destroy_hex_editor();
    //destroy_disassembler();

    disable_visual_styles();
}
