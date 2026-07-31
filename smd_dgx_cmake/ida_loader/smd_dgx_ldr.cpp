// Sega Genesis / Mega Drive ROM loader for IDA 9.2+.
// Ported from smd_ida_tools2 (sega_roms_ldr, DrMefistO [Lab 313]).
//
// Builds the database the SMD DGX debugger plugin expects: the 68k bus
// segment layout, the ROM header, the 64-entry vector table (with each
// handler marked as a procedure) and the hardware register names.

#include <ida.hpp>
#include <loader.hpp>
#include <idp.hpp>
#include <diskio.hpp>
#include <name.hpp>
#include <typeinf.hpp>
#include <auto.hpp>
#include <segment.hpp>
#include <bytes.hpp>
#include <kernwin.hpp>

#include "smd_dgx_ldr.h"

#ifndef _MSC_VER
#define _countof(a) (sizeof(a)/sizeof(*(a)))
#endif

static gen_hdr  _hdr;
static gen_vect _vect;

static const char* VECTOR_NAMES[] = {
  "SSP", "Reset", "BusErr", "AdrErr", "InvOpCode", "DivBy0", "Check", "TrapV", "GPF", "Trace",
  "Reserv0", "Reserv1", "Reserv2", "Reserv3", "Reserv4", "BadInt", "Reserv10", "Reserv11",
  "Reserv12", "Reserv13", "Reserv14", "Reserv15", "Reserv16", "Reserv17", "BadIRQ", "IRQ1",
  "EXT", "IRQ3", "HBLANK", "IRQ5", "VBLANK", "IRQ7", "Trap0", "Trap1", "Trap2", "Trap3",
  "Trap4", "Trap5", "Trap6", "Trap7", "Trap8", "Trap9", "Trap10", "Trap11", "Trap12",
  "Trap13", "Trap14", "Trap15", "Reserv30", "Reserv31", "Reserv32", "Reserv33", "Reserv34",
  "Reserv35", "Reserv36", "Reserv37", "Reserv38", "Reserv39", "Reserv3A", "Reserv3B",
  "Reserv3C", "Reserv3D", "Reserv3E", "Reserv3F"
};

struct reg_t {
  asize_t     size;
  ea_t        addr;
  const char* name;
};

static const reg_t SPEC_REGS[] = {
  { 4, 0xA04000, "Z80_YM2612" }, { 2, 0xA10000, "IO_PCBVER" },   { 2, 0xA10002, "IO_CT1_DATA" },
  { 2, 0xA10004, "IO_CT2_DATA" },{ 2, 0xA10006, "IO_EXT_DATA" }, { 2, 0xA10008, "IO_CT1_CTRL" },
  { 2, 0xA1000A, "IO_CT2_CTRL" },{ 2, 0xA1000C, "IO_EXT_CTRL" }, { 2, 0xA1000E, "IO_CT1_RX" },
  { 2, 0xA10010, "IO_CT1_TX" },  { 2, 0xA10012, "IO_CT1_SMODE" },{ 2, 0xA10014, "IO_CT2_RX" },
  { 2, 0xA10016, "IO_CT2_TX" },  { 2, 0xA10018, "IO_CT2_SMODE" },{ 2, 0xA1001A, "IO_EXT_RX" },
  { 2, 0xA1001C, "IO_EXT_TX" },  { 2, 0xA1001E, "IO_EXT_SMODE" },{ 2, 0xA11000, "IO_RAMMODE" },
  { 2, 0xA11100, "IO_Z80BUS" },  { 2, 0xA11200, "IO_Z80RES" },   { 0x100, 0xA12000, "IO_FDC" },
  { 0x100, 0xA13000, "IO_TIME" },{ 4, 0xA14000, "IO_TMSS" },     { 2, 0xC00000, "VDP_DATA" },
  { 2, 0xC00002, "VDP__DATA" },  { 2, 0xC00004, "VDP_CTRL" },    { 2, 0xC00006, "VDP__CTRL" },
  { 2, 0xC00008, "VDP_CNTR" },   { 2, 0xC0000A, "VDP__CNTR" },   { 2, 0xC0000C, "VDP___CNTR" },
  { 2, 0xC0000E, "VDP____CNTR" },{ 2, 0xC00011, "VDP_PSG" },
};

static const char M68K[]    = "68000";
static const char CODE[]    = "CODE";
static const char DATA[]    = "DATA";
static const char XTRN[]    = "XTRN";
static const char ROM[]     = "ROM";
static const char EPA[]     = "EPA";
static const char S32X[]    = "S32X";
static const char Z80[]     = "Z80";
static const char Z80_RAM[] = "Z80_RAM";
static const char REGS[]    = "REGS";
static const char Z80C[]    = "Z80C";
static const char ASSR[]    = "ASSR";
static const char VDP[]     = "VDP";
static const char RAM[]     = "RAM";
static const char M68K_RAM[]= "M68K_RAM";
static const char SRAM[]    = "SRAM";

static unsigned short READ_BE_WORD(const unsigned char* addr) {
  return (unsigned short)((addr[0] << 8) | addr[1]);
}

static unsigned int READ_BE_UINT(const unsigned char* addr) {
  return (unsigned int)((READ_BE_WORD(&addr[0]) << 16) | READ_BE_WORD(&addr[2]));
}

static unsigned int SWAP_BYTES_32(unsigned int a) {
  return ((a >> 24) & 0x000000FF) | ((a >> 8) & 0x0000FF00)
       | ((a << 8)  & 0x00FF0000) | ((a << 24) & 0xFF000000);
}

static void get_vector_addrs(gen_vect* table) {
  for (size_t i = 0; i < _countof(table->vectors); ++i)
    table->vectors[i] = SWAP_BYTES_32(table->vectors[i]);
}

static void add_segment(ea_t start, ea_t end, const char* name, const char* class_name,
                        const char* cmnt, uchar perm) {
  segment_t s;
  s.sel      = 0;
  s.start_ea = start;
  s.end_ea   = end;
  s.align    = saAbs;
  s.comb     = scPub;
  s.bitness  = 1;   // 32-bit
  s.perm     = perm;
  s.set_loader_segm(true);

  const int flags = ADDSEG_NOSREG | ADDSEG_NOTRUNC | ADDSEG_QUIET;
  if (!add_segm_ex(&s, name, class_name, flags))
    loader_failure();

  segment_t* segm = getseg(start);
  set_segment_cmt(segm, cmnt, false);
  create_byte(start, 1);
  segm->update();
}

static void make_array(ea_t addr, int datatype, const char* name, asize_t size) {
  const array_parameters_t array_params = { AP_ARRAY, 0, 0 };
  switch (datatype) {
    case 1: create_byte(addr, size);  break;
    case 2: create_word(addr, size);  break;
    case 4: create_dword(addr, size); break;
  }
  set_array_parameters(addr, &array_params);
  set_name(addr, name);
}

static void make_segments(size_t romsize) {
  add_segment(0x00000000, qmin(romsize, 0x003FFFFF + 1), ROM, CODE,
              "ROM segment", SEGPERM_EXEC | SEGPERM_READ);

  if (ASKBTN_YES == ask_yn(ASKBTN_NO, "Create Sega CD segment?"))
    add_segment(0x00400000, 0x007FFFFF + 1, EPA, DATA,
                "Expansion Port Area (used by the Sega CD)", SEGPERM_READ | SEGPERM_WRITE);

  if (ASKBTN_YES == ask_yn(ASKBTN_NO, "Create Sega 32X segment?"))
    add_segment(0x00800000, 0x009FFFFF + 1, S32X, DATA,
                "Unallocated (used by the Sega 32X)", SEGPERM_READ | SEGPERM_WRITE);

  add_segment(0x00A00000, 0x00A0FFFF + 1, Z80,  DATA, "Z80 Memory", SEGPERM_READ | SEGPERM_WRITE);
  add_segment(0x00A10000, 0x00A10FFF + 1, REGS, XTRN, "System registers", SEGPERM_WRITE);
  add_segment(0x00A11000, 0x00A11FFF + 1, Z80C, XTRN,
              "Z80 control (/BUSREQ and /RESET lines)", SEGPERM_WRITE);

  if (ASKBTN_YES == ask_yn(ASKBTN_NO, "Create FDC, TIME segment?"))
    add_segment(0x00A12000, 0x00AFFFFF + 1, ASSR, XTRN,
                "Assorted registers", SEGPERM_READ | SEGPERM_WRITE);
  else
    add_segment(0x00A14000, 0x00A14003 + 1, "TMSS", XTRN,
                "TMSS register", SEGPERM_READ | SEGPERM_WRITE);

  add_segment(0x00C00000, 0x00C0001F + 1, VDP, XTRN, "VDP Registers", SEGPERM_WRITE);
  add_segment(0x00FF0000, 0x00FFFFFF + 1, RAM, CODE, "RAM segment", SEGPERM_MAXVAL);

  set_name(0x00A00000, Z80_RAM);
  set_name(0x00FF0000, M68K_RAM);

  // SRAM, when the header advertises one
  if ((READ_BE_WORD(&_hdr.SramCode[0]) == 0x5241) && (_hdr.SramCode[2] == 0x20)) {
    const unsigned int sram_s = READ_BE_UINT(&_hdr.SramCode[4]);
    const unsigned int sram_e = READ_BE_UINT(&_hdr.SramCode[8]);
    if ((sram_s >= 0x400000) && (sram_e <= 0x9FFFFF) && (sram_s < sram_e))
      add_segment(sram_s, sram_e, SRAM, DATA, "SRAM memory", SEGPERM_READ | SEGPERM_WRITE);
  }
}

static void define_header() {
  make_array(0x100, 1, "CopyRights",   0x20);
  make_array(0x120, 1, "DomesticName", 0x30);
  make_array(0x150, 1, "OverseasName", 0x30);
  make_array(0x180, 1, "ProductCode",  0x0E);
  make_array(0x18E, 2, "Checksum",     0x02);
  make_array(0x190, 1, "Peripherials", 0x10);
  make_array(0x1A0, 4, "RomStart",     0x04);
  make_array(0x1A4, 4, "RomEnd",       0x04);
  make_array(0x1A8, 4, "RamStart",     0x04);
  make_array(0x1AC, 4, "RamEnd",       0x04);
  make_array(0x1B0, 1, "SramCode",     0x0C);
  make_array(0x1BC, 1, "ModemCode",    0x0C);
  make_array(0x1C8, 1, "Reserved",     0x28);
  make_array(0x1F0, 1, "CountryCode",  0x10);
}

static void set_spec_register_names() {
  for (size_t i = 0; i < _countof(SPEC_REGS); ++i) {
    if (SPEC_REGS[i].size == 2)
      create_word(SPEC_REGS[i].addr, 2);
    else if (SPEC_REGS[i].size == 4)
      create_dword(SPEC_REGS[i].addr, 4);
    else
      create_byte(SPEC_REGS[i].addr, SPEC_REGS[i].size);
    set_name(SPEC_REGS[i].addr, SPEC_REGS[i].name);
  }
}

static void add_vector_subs(gen_vect* table) {
  create_dword(0, 4);
  for (size_t i = 1; i < _countof(VECTOR_NAMES); ++i) {   // vector 0 is the SSP, not code
    const ea_t ea = table->vectors[i];
    auto_make_proc(ea);
    set_name(ea, VECTOR_NAMES[i]);
    create_dword(i * 4, 4);
  }
}

static void add_vdp_status_enum() {
  enum_type_data_t en;
  en.add_constant("FIFO_EMPTY",       9);
  en.add_constant("FIFO_FULL",        8);
  en.add_constant("VBLANK_PENDING",   7);
  en.add_constant("SPRITE_OVERFLOW",  6);
  en.add_constant("SPRITE_COLLISION", 5);
  en.add_constant("ODD_FRAME",        4);
  en.add_constant("VBLANKING",        3);
  en.add_constant("HBLANKING",        2);
  en.add_constant("DMA_IN_PROGRESS",  1);
  en.add_constant("PAL_MODE",         0);
  create_enum_type("vdp_status", en, 1, type_unsigned, false);
}

static int idaapi accept_file(qstring* fileformatname, qstring* processor,
                              linput_t* li, const char* filename) {
  qlseek(li, 0, SEEK_SET);
  if (qlread(li, &_vect, sizeof(_vect)) != sizeof(_vect)) return 0;
  if (qlread(li, &_hdr,  sizeof(_hdr))  != sizeof(_hdr))  return 0;
  if (!strneq((const char*)_hdr.CopyRights, "SEGA", 4))   return 0;

  fileformatname->sprnt("%s", "Sega Genesis/MegaDrive ROM (SMD DGX)");
  return 1;
}

static void idaapi load_file(linput_t* li, ushort neflags, const char* fileformatname) {
  if (PH.id != PLFM_68K) {
    set_processor_type("68020", SETPROC_LOADER);
    set_target_assembler(0);
  }
  inf_set_app_bitness(32);

  inf_set_af(0
      | AF_UNK        // delete instructions with no xrefs
      | AF_CODE       // trace execution flow
      | AF_PROC       // create functions if call is present
      | AF_USED       // analyze and create all xrefs
      | AF_JFUNC      // rename jump functions as j_...
      | AF_NULLSUB    // rename empty functions as nullsub_...
      | AF_IMMOFF     // convert 32-bit instruction operand to offset
      | AF_JUMPTBL    // locate and create jump tables
      | AF_STKARG     // propagate stack argument information
      | AF_REGARG     // propagate register argument information
      | AF_SIGMLT     // recognize several copies of the same function
      | AF_DATOFF     // automatically convert data to offsets
      | AF_TRFUNC     // truncate functions upon code deletion
      | AF_PURDAT     // control flow to data segment is ignored
  );
  inf_set_af2(0);

  const unsigned int size = (unsigned int)qlsize(li);

  qlseek(li, 0, SEEK_SET);
  if (qlread(li, &_vect, sizeof(_vect)) != sizeof(_vect)) loader_failure();
  if (qlread(li, &_hdr,  sizeof(_hdr))  != sizeof(_hdr))  loader_failure();

  file2base(li, 0, 0x0000000, size, FILEREG_PATCHABLE);

  make_segments(size);
  get_vector_addrs(&_vect);
  define_header();
  set_spec_register_names();
  add_vector_subs(&_vect);
  add_vdp_status_enum();

  inf_set_baseaddr(0);
  inf_set_start_cs(0);
  inf_set_start_ip(_vect.Reset);
  inf_set_start_ea(_vect.Reset);
  inf_set_main(_vect.Reset);
  inf_set_start_sp(_vect.SSP);
  inf_set_lowoff(0x200);

  msg("SMD DGX loader v" LDR_VERSION " (based on sega_roms_ldr by DrMefistO)\n");
}

loader_t LDSC = {
  IDP_INTERFACE_VERSION,
  0,
  accept_file,
  load_file,
  nullptr,
  nullptr,
};
