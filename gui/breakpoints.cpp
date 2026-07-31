#include <Windows.h>
#include <CommCtrl.h>
//#include <process.h>
#include <string>
//#include <vector>
//#include <map>
//#include <sstream>
//#include <iomanip>
//#include <algorithm>
//#include <regex>
//#include <list>

#include "gui.h"
#include "breakpoints.h"

#include "edit_fields.h"

#include "resource.h"

#include "debug_wrap.h"


#define DBG_EVENTS_TIMER 1

static HANDLE hThread = NULL;

static HWND bptsHwnd = NULL;

static void update_bpt_list()
{
    send_dbg_request(dbg_req, REQ_LIST_BREAKS);

    ListView_SetItemCount(GetDlgItem(bptsHwnd, IDC_BPT_LIST), dbg_req->bpt_list.count);
}

static void delete_breakpoint_from_list(int index)
{
    if (index != -1)
    {
        bpt_data_t *bpt_item = &dbg_req->bpt_list.breaks[index];

        bpt_data_t *bpt_data = &dbg_req->bpt_data;
        bpt_data->address = bpt_item->address;
        bpt_data->type = bpt_item->type;
        send_dbg_request(dbg_req, REQ_DEL_BREAK);
        update_bpt_list();
    }
}

LRESULT CALLBACK BptListHandler(HWND hWnd, UINT uMsg, WPARAM wParam,
    LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
    switch (uMsg)
    {
    case WM_KEYDOWN:
        if (GetKeyState(VK_CONTROL) & 0x8000) return FALSE;

        switch (wParam)
        {
        case VK_DELETE:
        {
            int index = ListView_GetNextItem(GetDlgItem(bptsHwnd, IDC_BPT_LIST), -1, LVNI_SELECTED);
            delete_breakpoint_from_list(index);
        } break;
        }
        return TRUE;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK BptsWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
    {
        SetFocus(hWnd);

        RECT r;
        GetClientRect(hWnd, &r);
        AdjustWindowRectEx(&r, GetWindowLong(hWnd, GWL_STYLE), (GetMenu(hWnd) > 0), GetWindowLong(hWnd, GWL_EXSTYLE));

        CheckDlgButton(hWnd, IDC_EXEC_BPT, BST_CHECKED);
        CheckDlgButton(hWnd, IDC_BPT_IS_READ, BST_CHECKED);
        CheckDlgButton(hWnd, IDC_BPT_IS_WRITE, BST_CHECKED);

        HWND bp_sizes = GetDlgItem(hWnd, IDC_BPT_SIZE);
        SendMessage(bp_sizes, CB_ADDSTRING, 0, (LPARAM)"1");
        SendMessage(bp_sizes, CB_ADDSTRING, 0, (LPARAM)"2");
        SendMessage(bp_sizes, CB_ADDSTRING, 0, (LPARAM)"4");
        SendMessage(bp_sizes, CB_SETCURSEL, 0, 0);

        HWND bpt_list = GetDlgItem(hWnd, IDC_BPT_LIST);
        ListView_SetExtendedListViewStyle(bpt_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

        SetWindowSubclass(bpt_list, BptListHandler, 0, 0);

        LV_COLUMN column;
        memset(&column, 0, sizeof(column));
        
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = (LPSTR)"";
        column.cx = 25;
        ListView_InsertColumn(bpt_list, 0, &column);
        
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = (LPSTR)"Address";
        column.cx = 0x42;
        column.iSubItem++;
        ListView_InsertColumn(bpt_list, 1, &column);
        
        column.cx = 0x30;
        column.pszText = (LPSTR)"Size";
        column.iSubItem++;
        ListView_InsertColumn(bpt_list, 2, &column);

        column.cx = 0x52;
        column.pszText = (LPSTR)"Type";
        column.iSubItem++;
        ListView_InsertColumn(bpt_list, 3, &column);

        HWND header = ListView_GetHeader(bpt_list);

        HDITEM hdi = { 0 };
        hdi.mask = HDI_FORMAT;
        Header_GetItem(header, 0, &hdi);
        hdi.fmt |= HDF_FIXEDWIDTH;
        Header_SetItem(header, 0, &hdi);

        SetTimer(hWnd, DBG_EVENTS_TIMER, 500, NULL);
    } break;
    case WM_TIMER:
    {
        switch (LOWORD(wParam))
        {
        case DBG_EVENTS_TIMER:
            update_bpt_list();
            break;
        }
        return FALSE;
    } break;
    case WM_NOTIFY:
    {
        switch (((LPNMHDR)lParam)->code)
        {
        case NM_CLICK:
        {
            HWND bpt_list = GetDlgItem(hWnd, IDC_BPT_LIST);
            LPNMLISTVIEW nmlist = (LPNMLISTVIEW)lParam;

            LVHITTESTINFO hitinfo;
            hitinfo.pt = nmlist->ptAction;

            int item = ListView_HitTest(bpt_list, &hitinfo);

            if (item != -1)
            {
                if ((hitinfo.flags == LVHT_ONITEMSTATEICON))
                {
                    bpt_data_t *bpt_data = &dbg_req->bpt_list.breaks[nmlist->iItem];
                    ListView_SetCheckState(bpt_list, nmlist->iItem, !bpt_data->enabled);
                    ListView_RedrawItems(bpt_list, nmlist->iItem, nmlist->iItem);

                    bpt_data_t *bpt_data_new = &dbg_req->bpt_data;
                    bpt_data_new->address = bpt_data->address;
                    bpt_data_new->type = bpt_data->type;
                    bpt_data_new->width = bpt_data->width;

                    send_dbg_request(dbg_req, REQ_TOGGLE_BREAK);
                    update_bpt_list();
                }
            }
            return 0;
        } break;
        case LVN_GETDISPINFO:
        {
            NMLVDISPINFO *plvdi = (NMLVDISPINFO *)lParam;
            NMLISTVIEW *nmList = (NMLISTVIEW *)lParam;
            HWND bpt_list = GetDlgItem(hWnd, IDC_BPT_LIST);

            char tmp[32];

            bpt_data_t *bpt_data = &dbg_req->bpt_list.breaks[plvdi->item.iItem];
            switch (plvdi->item.iSubItem)
            {
            case 0: // Enabled

                if (plvdi->item.mask & LVIF_IMAGE)
                {
                    plvdi->item.mask |= LVIF_STATE;
                    plvdi->item.stateMask = LVIS_STATEIMAGEMASK;
                    plvdi->item.state = INDEXTOSTATEIMAGEMASK(bpt_data->enabled ? 2 : 1);
                    return 0;
                }
                break;
            case 1: // Address
            {
                snprintf(tmp, sizeof(tmp), "%.6X", bpt_data->address & 0xFFFFFF);
                plvdi->item.pszText = tmp;
            } break;
            case 2: // Size
            {
                snprintf(tmp, sizeof(tmp), "%d", bpt_data->width);
                plvdi->item.pszText = tmp;
            } break;
            case 3: // Type
            {
                switch (bpt_data->type)
                {
                case BPT_M68K_E: snprintf(tmp, sizeof(tmp), "%s", "M68K_E"); break;
                case BPT_Z80_E: snprintf(tmp, sizeof(tmp), "%s", "Z80_E"); break;
                case BPT_M68K_R: snprintf(tmp, sizeof(tmp), "%s", "M68K_R"); break;
                case BPT_M68K_W: snprintf(tmp, sizeof(tmp), "%s", "M68K_W"); break;
                case BPT_M68K_RW: snprintf(tmp, sizeof(tmp), "%s", "M68K_RW"); break;

                // VDP
                case BPT_VRAM_R: snprintf(tmp, sizeof(tmp), "%s", "VRAM_R"); break;
                case BPT_VRAM_W: snprintf(tmp, sizeof(tmp), "%s", "VRAM_W"); break;
                case BPT_VRAM_RW: snprintf(tmp, sizeof(tmp), "%s", "VRAM_RW"); break;

                case BPT_CRAM_R: snprintf(tmp, sizeof(tmp), "%s", "CRAM_R"); break;
                case BPT_CRAM_W: snprintf(tmp, sizeof(tmp), "%s", "CRAM_W"); break;
                case BPT_CRAM_RW: snprintf(tmp, sizeof(tmp), "%s", "CRAM_RW"); break;

                case BPT_VSRAM_R: snprintf(tmp, sizeof(tmp), "%s", "VSRAM_R"); break;
                case BPT_VSRAM_W: snprintf(tmp, sizeof(tmp), "%s", "VSRAM_W"); break;
                case BPT_VSRAM_RW: snprintf(tmp, sizeof(tmp), "%s", "VSRAM_RW"); break;

                // Z80
                case BPT_Z80_R: snprintf(tmp, sizeof(tmp), "%s", "Z80_R"); break;
                case BPT_Z80_W: snprintf(tmp, sizeof(tmp), "%s", "Z80_W"); break;
                case BPT_Z80_RW: snprintf(tmp, sizeof(tmp), "%s", "Z80_RW"); break;

                // REGS
                case BPT_VDP_REG:
                case BPT_M68K_REG:
                    break;
                }
                plvdi->item.pszText = tmp;
            } break;
            default:
                break;
            }
            return TRUE;
        } break;
        }
    } break;
    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case IDC_ADD_BREAK:
        {
            bpt_data_t *bpt_data = &dbg_req->bpt_data;
            bpt_data->address = GetDlgItemHex(bptsHwnd, IDC_BPT_ADDR);
            int bpt_type = (int)SendMessage(GetDlgItem(bptsHwnd, IDC_BPT_SIZE), CB_GETCURSEL, 0, 0);

            switch (bpt_type)
            {
            case 1: bpt_data->width = 2; break;
            case 2: bpt_data->width = 4; break;
            default: bpt_data->width = 1; break;
            }

            if (IsDlgButtonChecked(bptsHwnd, IDC_EXEC_BPT))
                bpt_data->type = BPT_M68K_E;
            else if (IsDlgButtonChecked(bptsHwnd, IDC_68K_RAM_BPT))
            {
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_READ))
                    bpt_data->type = BPT_M68K_R;
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_WRITE))
                    bpt_data->type = (bpt_type_t)((int)bpt_data->type | (int)BPT_M68K_W);
            }
            else if (IsDlgButtonChecked(bptsHwnd, IDC_VRAM_BPT))
            {
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_READ))
                    bpt_data->type = BPT_VRAM_R;
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_WRITE))
                    bpt_data->type = (bpt_type_t)((int)bpt_data->type | (int)BPT_VRAM_W);
            }
            else if (IsDlgButtonChecked(bptsHwnd, IDC_CRAM_BPT))
            {
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_READ))
                    bpt_data->type = BPT_CRAM_R;
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_WRITE))
                    bpt_data->type = (bpt_type_t)((int)bpt_data->type | (int)BPT_CRAM_W);
            }
            else if (IsDlgButtonChecked(bptsHwnd, IDC_VSRAM_BPT))
            {
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_READ))
                    bpt_data->type = BPT_VSRAM_R;
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_WRITE))
                    bpt_data->type = (bpt_type_t)((int)bpt_data->type | (int)BPT_VSRAM_W);
            }
            else if (IsDlgButtonChecked(bptsHwnd, IDC_Z80_RAM_BPT))
            {
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_READ))
                    bpt_data->type = BPT_Z80_R;
                if (IsDlgButtonChecked(bptsHwnd, IDC_BPT_IS_WRITE))
                    bpt_data->type = (bpt_type_t)((int)bpt_data->type | (int)BPT_Z80_W);
            }

            bpt_data->enabled = 1;
            send_dbg_request(dbg_req, REQ_ADD_BREAK);
            update_bpt_list();
        } break;
        case IDC_DEL_BREAK:
        {
            int index = ListView_GetNextItem(GetDlgItem(bptsHwnd, IDC_BPT_LIST), -1, LVNI_SELECTED);
            delete_breakpoint_from_list(index);
        } break;
        case IDC_CLEAR_BREAKS:
            send_dbg_request(dbg_req, REQ_CLEAR_BREAKS);
            update_bpt_list();
        } break;

        return TRUE;
    } break;
    case WM_CLOSE:
    {
        KillTimer(bptsHwnd, DBG_EVENTS_TIMER);
        bptsHwnd = NULL;

        PostQuitMessage(0);
        EndDialog(hWnd, 0);

        if (hThread) {
            CloseHandle(hThread);
            hThread = 0;
        }
    } break;
    default:
        return FALSE;
    }

    return TRUE;
}

static DWORD WINAPI ThreadProc(LPVOID lpParam)
{
    MSG msg;

    bptsHwnd = CreateDialog(pinst, MAKEINTRESOURCE(IDD_BPTS), rarch, (DLGPROC)BptsWndProc);
    ShowWindow(bptsHwnd, SW_SHOW);
    UpdateWindow(bptsHwnd);
    SetForegroundWindow(bptsHwnd);

    while (GetMessage(&msg, NULL, 0, 0))
    {
        if (!IsDialogMessage(bptsHwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    return 1;
}

void create_breakpoints_window()
{
    if (bptsHwnd == NULL) {
        if (dbg_req == NULL) {
            dbg_req = open_shared_mem();
        }

        hThread = CreateThread(0, NULL, ThreadProc, NULL, NULL, NULL);
    }
}

void destroy_breakpoints_window()
{
    if (bptsHwnd) {
        SendMessage(bptsHwnd, WM_CLOSE, 0, 0);
    }

    if (hThread) {
        TerminateThread(hThread, 0);
        CloseHandle(hThread);
        hThread = 0;
    }

    close_shared_mem(&dbg_req);
}
