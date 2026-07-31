#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <Windows.h>

typedef struct {
    char Name[12];
    unsigned char*  Array;
    int Offset;
    int Size;
    int Active;
    unsigned char   Swap;
} HexRegion;

typedef enum {
    NO,
    CELL,
    TEXT
} MousePos;

typedef struct {
    HWND Hwnd;
    HDC  DC;
    char InputDigit;
    int
        MouseButtonHeld, SecondDigitPrompted, Running,
        TextView, DrawLines, FontBold;
    int
        FontHeight, FontWidth, FontWeight,
        Gap, GapHeaderX, GapHeaderY,
        CellHeight, CellWidth,
        DialogPosX, DialogPosY,
        OffsetVisibleFirst, OffsetVisibleTotal,
        AddressSelectedFirst, AddressSelectedTotal, AddressSelectedLast;
    COLORREF
        ColorFont, ColorBG;
    HexRegion CurrentRegion;
    MousePos MouseArea;
    SCROLLINFO SI;
} HexParams;

typedef struct {
    unsigned char*  Array;
    UINT Start;
    UINT Size;
    char*Name;
} SymbolName;

typedef struct {
    unsigned char*  Array;
    UINT Address;
    UINT Value;
    int Active;
} HardPatch;

void HexCreateDialog();
void HexDestroyDialog(HexParams *Hex);
void HexUpdateDialog(HexParams *Hex, int ClearBG);
extern HexParams HexCommon;

void create_hex_editor();
void destroy_hex_editor();
void update_hex_editor();

HexParams *get_hexeditor(int index);

#ifdef __cplusplus
}
#endif
