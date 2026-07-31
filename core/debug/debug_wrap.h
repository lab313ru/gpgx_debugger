#ifndef _DEBUG_WRAP_H_
#define _DEBUG_WRAP_H_

#ifdef __cplusplus
extern "C" {
#endif

#define SHARED_MEM_NAME "GX_PLUS_SHARED_MEM"
#define MAX_BREAKPOINTS 1000
#define MAX_DBG_EVENTS 20

#ifndef MAXROMSIZE
#define MAXROMSIZE ((unsigned int)0xA00000)
#endif

#pragma pack(push, 4)
    // copy from cpuhook.h:: hook_type_t
typedef enum {
    BPT_ANY = (0 << 0),
    // M68K
    BPT_M68K_E = (1 << 0),
    BPT_M68K_R = (1 << 1),
    BPT_M68K_W = (1 << 2),
    BPT_M68K_RW = BPT_M68K_R | BPT_M68K_W,
    BPT_M68K_RE = BPT_M68K_R | BPT_M68K_E,
    BPT_M68K_WE = BPT_M68K_W | BPT_M68K_E,
    BPT_M68K_RWE = BPT_M68K_R | BPT_M68K_W | BPT_M68K_E,

    // VDP
    BPT_VRAM_R = (1 << 3),
    BPT_VRAM_W = (1 << 4),
    BPT_VRAM_RW = BPT_VRAM_R | BPT_VRAM_W,

    BPT_CRAM_R = (1 << 5),
    BPT_CRAM_W = (1 << 6),
    BPT_CRAM_RW = BPT_CRAM_R | BPT_CRAM_W,

    BPT_VSRAM_R = (1 << 7),
    BPT_VSRAM_W = (1 << 8),
    BPT_VSRAM_RW = BPT_VSRAM_R | BPT_VSRAM_W,

    // Z80
    BPT_Z80_E = (1 << 11),
    BPT_Z80_R = (1 << 12),
    BPT_Z80_W = (1 << 13),
    BPT_Z80_RW = BPT_Z80_R | BPT_Z80_W,
    BPT_Z80_RE = BPT_Z80_R | BPT_Z80_E,
    BPT_Z80_WE = BPT_Z80_W | BPT_Z80_E,
    BPT_Z80_RWE = BPT_Z80_R | BPT_Z80_W | BPT_Z80_E,

    // REGS
    BPT_VDP_REG = (1 << 9),
    BPT_M68K_REG = (1 << 10),
} bpt_type_t;

typedef enum {
    REQ_NO_REQUEST,

    REQ_GET_REGS,
    REQ_SET_REGS,

    REQ_GET_REG,
    REQ_SET_REG,

    REQ_READ_68K_ROM,
    REQ_READ_68K_RAM,

    REQ_WRITE_68K_ROM,
    REQ_WRITE_68K_RAM,

    REQ_READ_Z80,
    REQ_WRITE_Z80,

    REQ_ADD_BREAK,
    REQ_TOGGLE_BREAK,
    REQ_DEL_BREAK,
    REQ_CLEAR_BREAKS,
    REQ_LIST_BREAKS,

    REQ_ATTACH,
    REQ_PAUSE,
    REQ_RESUME,
    REQ_STOP,

    REQ_STEP_INTO,
    REQ_STEP_OVER,
} request_type_t;

typedef enum {
    REG_TYPE_M68K = (1 << 0),
    REG_TYPE_S80 = (1 << 1),
    REG_TYPE_Z80 = (1 << 2),
    REG_TYPE_VDP = (1 << 3),
} register_type_t;

typedef enum {
    DBG_EVT_NO_EVENT,
    DBG_EVT_STARTED,
    DBG_EVT_PAUSED,
    DBG_EVT_BREAK,
    DBG_EVT_STEP,
    DBG_EVT_STOPPED,
} dbg_event_type_t;

typedef struct {
    dbg_event_type_t type;
    unsigned int pc;
} debugger_event_t;

typedef struct {
    int index;
    unsigned int val;
} reg_val_t;

typedef struct {
    unsigned int d0, d1, d2, d3, d4, d5, d6, d7;
    unsigned int a0, a1, a2, a3, a4, a5, a6, a7;
    unsigned int pc, sr, sp, usp, isp, ppc, ir;
} regs_68k_data_t;

typedef enum {
    REG_68K_D0,
    REG_68K_D1,
    REG_68K_D2,
    REG_68K_D3,
    REG_68K_D4,
    REG_68K_D5,
    REG_68K_D6,
    REG_68K_D7,

    REG_68K_A0,
    REG_68K_A1,
    REG_68K_A2,
    REG_68K_A3,
    REG_68K_A4,
    REG_68K_A5,
    REG_68K_A6,
    REG_68K_A7,

    REG_68K_PC,
    REG_68K_SR,
    REG_68K_SP,
    REG_68K_USP,
    REG_68K_ISP,
    REG_68K_PPC,
    REG_68K_IR,

    REG_VDP_00,
    REG_VDP_01,
    REG_VDP_02,
    REG_VDP_03,
    REG_VDP_04,
    REG_VDP_05,
    REG_VDP_06,
    REG_VDP_07,
    REG_VDP_08,
    REG_VDP_09,
    REG_VDP_0A,
    REG_VDP_0B,
    REG_VDP_0C,
    REG_VDP_0D,
    REG_VDP_0E,
    REG_VDP_0F,
    REG_VDP_10,
    REG_VDP_11,
    REG_VDP_12,
    REG_VDP_13,
    REG_VDP_14,
    REG_VDP_15,
    REG_VDP_16,
    REG_VDP_17,
    REG_VDP_18,
    REG_VDP_19,
    REG_VDP_1A,
    REG_VDP_1B,
    REG_VDP_1C,
    REG_VDP_1D,
    REG_VDP_1E,
    REG_VDP_1F,
    REG_VDP_DMA_LEN,
    REG_VDP_DMA_SRC,
    REG_VDP_DMA_DST,

    REG_Z80_PC,
    REG_Z80_SP,
    REG_Z80_AF,
    REG_Z80_BC,
    REG_Z80_DE,
    REG_Z80_HL,
    REG_Z80_IX,
    REG_Z80_IY,
    REG_Z80_WZ,

    REG_Z80_AF2,
    REG_Z80_BC2,
    REG_Z80_DE2,
    REG_Z80_HL2,

    REG_Z80_R,
    REG_Z80_R2,
    REG_Z80_IFFI1,
    REG_Z80_IFFI2,
    REG_Z80_HALT,
    REG_Z80_IM,
    REG_Z80_I,
} regs_all_t;

typedef struct {
    unsigned int pc, sp, af, bc, de, hl, ix, iy, wz;
    unsigned int af2,bc2,de2,hl2;
    unsigned char r, r2, iff1, iff2, halt, im, i;
} regs_z80_data_t;

typedef struct {
    unsigned char regs_vdp[0x20];
    unsigned int dma_len;
    unsigned int dma_src, dma_dst;
} vdp_regs_t;

typedef struct {
    int type; // register_type_t
    
    regs_68k_data_t regs_68k;
    reg_val_t any_reg;
    vdp_regs_t vdp_regs;
    regs_z80_data_t regs_z80;
} register_data_t;

typedef struct {
    int size;
    unsigned int address;

    unsigned char m68k_rom[MAXROMSIZE];
    unsigned char m68k_ram[0x10000];
    unsigned char z80_ram[0x2000];
} memory_data_t;

typedef struct {
    bpt_type_t type;
    unsigned int address;
    int width;
    int enabled;
} bpt_data_t;

typedef struct {
    int count;
    bpt_data_t breaks[MAX_BREAKPOINTS];
} bpt_list_t;

typedef struct {
    request_type_t req_type;
    register_data_t regs_data;
    memory_data_t mem_data;
    bpt_data_t bpt_data;
    unsigned int dbg_events_count;
    debugger_event_t dbg_events[MAX_DBG_EVENTS];
    bpt_list_t bpt_list;
    int dbg_active, dbg_paused;
} dbg_request_t;
#pragma pack(pop)

dbg_request_t* create_shared_mem();
dbg_request_t *open_shared_mem();
void close_shared_mem(dbg_request_t **request, int do_unmap);
int recv_dbg_event_ida(dbg_request_t* request, int wait);
void send_dbg_request(dbg_request_t *request, request_type_t type, int ignore_active);

#ifdef __cplusplus
}
#endif

#endif
