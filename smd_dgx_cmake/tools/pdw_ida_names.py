"""Apply the reverse-engineered names to the Pirates of Dark Water IDB.

Run from inside IDA (File > Script file). Safe to re-run: it only sets names
and comments, never deletes code, and skips anything already named by hand.

Everything here is documented in F:\\Projects\\sega\\RE_NOTES.md.
"""
import ida_bytes, ida_name, ida_funcs, ida_ua, ida_auto, idc

CODE = {
    0x28A2: ("unpack_reverse_lz", "a0=packed src, a1=dst. Backwards bitstream LZ77."),
    0x2944: ("unpack_copy_run", None),
    0x295A: ("unpack_sized_field", "width from table at 0x2990, indexed by selector"),
    0x2960: ("unpack_bits", "n+1 bits, MSB first, walking down"),
    0x297A: ("unpack_bit", "also latches the field-width selector"),
    0x29FE: ("fade_step_colour", "one colour toward target; steps HALF units - "
                                 "the VDP ignores each channel's low bit"),
    0x2DB4: ("cram_upload", "FFD7D4 shadow -> CRAM, every frame"),
    0x2DD4: ("vram_upload", "a0=src, d0=words, d1=VDP dest"),
    0x2DF0: ("vram_upload_cmd", "d1 = full VDP address-and-command long"),
    0x4016: ("vram_upload_container", "a0=container ((a0)=tile offset), "
                                      "d0=tiles, d1=VDP dest"),
    0x4022: ("nametable_fill", "a0=container, d0=VDP dest, d2=map index, "
                               "d4=base attribute, d6=cols-1, d7=rows-1"),
    0x405C: ("nametable_fill_wide", "same, 0x100 row stride"),
    0x4304: ("vdp_set_registers", "19 registers from a0"),
    0x4342: ("vdp_queue_transfer", "12-byte ring at FFD862"),
    0x4B4F4: ("scene_hero_select", None),
    0x4D2C4: ("cycle_menu_highlight", "16-step table at 0x4D87E -> colour 15 of "
                                      "palette lines 0-2. Not a fade."),
    0x4D3CC: ("textobj_clear", "10 slots x 0x12 bytes at FF1F94"),
    0x4D3FE: ("textobj_add", None),
    0x4D4C6: ("draw_box_9patch", "tiles 0x5CE.., via cell_write_box"),
    0x4D51E: ("cell_write_box", "d0=col d1=row d2=piece; tile = 0x5CE + d2"),
    0x4D5C8: ("text_draw", "a0 = {u16 col, u16 row, chars}, d3 = palette bits"),
    0x4D5D4: ("text_draw_indented", None),
    0x4D676: ("cell_write_glyph", "tile = 0x573 + (c - 0x21), space = 0"),
    0x4D6D6: ("load_portrait", "d0 = portrait index * 4. Unpacks via table 0x4D73E "
                               "to VRAM 0x8D80, draws 8x6 at nametable 0xC082, "
                               "palette line 3."),
    0x4D792: ("load_screen_gfx", "walks the VRAM descriptor list at 0x5C272"),
    0x4D84C: ("palette_clear", "both buffers, 64 entries, immediate"),
    0x4D868: ("palette_load_line", "d0 = line index, a2 = 16-colour source"),
}

DATA = {
    0x2990: ("unpack_width_table", "{3, 7, 15, 0} - field is entry+1 bits wide"),
    0x4B4CC: ("scene_table", "10 entries"),
    0x4D73E: ("portrait_table", "21 packed blocks, 1536 bytes each (8x6 tiles)"),
    0x4D87E: ("menu_highlight_cycle", "16 colours, bus format"),
    0x4D94A: ("palette_ui", "fixed palette line 2"),
    0x4D96A: ("portrait_palettes", "21 x 32 bytes, index-matched to portrait_table"),
    0x4DD4A: ("dialogue_handler_table", "12 entries"),
    0x4DDAA: ("scene_handler_table", "12 entries"),
    0x4DE0C: ("vdp_regs_menu", "19 registers: plane A C000, plane B E000, "
                               "sprites F400, hscroll F000, 64x32"),
    0x4DE20: ("text_niddler", None),
    0x4DC0A: ("hero_accent_palettes", "palette line 1, 32 bytes per hero"),
    0x4DCCA: ("palette_menu_bg", "palette line 0"),
    0xE2774: ("font_tiles", "91 raw tiles, '!'..'{', -> VRAM 0xAE60 (tile 0x573)"),
    0x52D02: ("box_tiles", "raw container -> VRAM 0xB9C0 (tile 0x5CE)"),
    0x527B5: ("text_menu_options", "NUL-separated: START LEVEL / TALK TO NIDDLER "
                                   "/ MAP SCREEN / CHOOSE A HERO / REN / TULA / IOZ"),
    0x52798: ("text_hero_select_title", "{col 12, row 3} CHOOSE YOUR HERO WISELY."),
    0x52804: ("text_portrait_lines", "one string per portrait, indices 15..20"),
    0x5C272: ("vram_desc_main", "{u32 src, u16 words, u16 dest}, neg-long terminated"),
    0xFF0D20: ("palette_target", "64 entries, bus format - what the scene wants"),
    0xFF0EB0: ("selected_hero", "index into portrait_table (scaled by 4)"),
    0xFFC9A8: ("sprite_table_staging", "640 bytes, DMA'd by 0x21CA"),
    0xFFCF14: ("hscroll_staging", "224 lines x 4 bytes, DMA'd by 0x2220"),
    0xFFD7D4: ("palette_shadow", "64 entries, uploaded to CRAM every frame"),
}


def apply(ea, name, comment, make_func):
    # has_user_name is the real test: auto-generated sub_/loc_/unk_ names are
    # dummy names and fair game, anything the user typed is not.
    if ida_bytes.has_user_name(ida_bytes.get_flags(ea)):
        existing = ida_name.get_name(ea)
        if existing != name:
            print(f"  {ea:06X} already named {existing!r} by hand, skipped")
            return
    if make_func and not ida_funcs.get_func(ea):
        ida_ua.create_insn(ea)
        ida_funcs.add_func(ea)
    if ida_name.set_name(ea, name, ida_name.SN_NOCHECK | ida_name.SN_FORCE):
        print(f"  {ea:06X}  {name}")
    else:
        print(f"  {ea:06X}  FAILED to name as {name}")
    if comment:
        idc.set_cmt(ea, comment, 1)


print("Naming code:")
for ea, (name, cmt) in sorted(CODE.items()):
    apply(ea, name, cmt, True)

print("Naming data:")
for ea, (name, cmt) in sorted(DATA.items()):
    apply(ea, name, cmt, False)

# The portrait table is 21 longs; typing it makes the xrefs show up.
for i in range(21):
    ea = 0x4D73E + 4 * i
    ida_bytes.del_items(ea, ida_bytes.DELIT_SIMPLE, 4)
    ida_bytes.create_data(ea, ida_bytes.FF_DWORD, 4, 0)
    idc.op_plain_offset(ea, 0, 0)

ida_auto.auto_wait()
print("done")
