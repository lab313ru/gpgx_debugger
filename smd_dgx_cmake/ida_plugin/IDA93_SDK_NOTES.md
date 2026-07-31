# Porting the SMD (gpgx) debugger plugin — IDA 9.1 → 9.2/9.3 SDK build spec

Scope: static-linked emulator + embedded Qt6 debugger views, built with the open-source `HexRaysSA/ida-sdk` (or `allthingsida/ida-cmake`). Every section gives real 9.x signatures and flags what to change relative to the Gensida (9.1-era) code in `E:\Projects\smd_ida_tools2\Gensida\ida\`.

Headline: **the debugger ABI is frozen — `IDD_INTERFACE_VERSION == 31` in 9.0/9.1/9.2/9.3, and 9.2 vs 9.3 `idd.hpp`/`dbg.hpp` differ only in comments.** Almost all real churn is (a) *source-level renames* in `register_info_t`, (b) the plugin-registration idiom (`PLUGIN_MULTI` + `plugmod_t`), (c) Qt5→Qt6 + `QT_NAMESPACE=QT`, and (d) the CMake/MSVC2022/C++17 build. Treat this doc as a checklist of those diffs.

---

## 1. `debugger_t` + registration idiom

### 1.1 Struct layout (`idd.hpp`, v9.3.1, lines 947–1174) — exact field order/types

```cpp
struct debugger_t
{
  int          version;            // MUST be IDD_INTERFACE_VERSION (==31)
  const char  *name;               // "gpgx"
  int          id;                 // DEBUGGER_ID_*
  const char  *processor;          // required processor module name (e.g. "68000")
  uint64       flags;              // DBG_FLAG_* (low 32) | DBG_HAS_* (high 32)
  const char **regclasses;         // nullptr-terminated array of reg-class names
  int          default_regclasses; // MASK of default-printed classes
  register_info_t *registers;      // register table
  int          nregisters;         // count
  int          memory_page_size;   // usually 0x1000
  const uchar *bpt_bytes;          // sw-bpt opcode bytes
  uchar        bpt_size;           // size of that opcode
  uchar        filetype;           // input file type for instant debugging
  ushort       resume_modes;       // DBG_RESMOD_*
};
```

**DIFFERENCES vs how such structs are often hand-written / vs 8.x:**
- Field is **`regclasses`** (`const char **`), *not* `register_classes`.
- Count is **`nregisters`** (`int`), *not* `registers_size`.
- **`filetype` (uchar) sits between `bpt_size` and `resume_modes`** — easy to miss in positional aggregate init; getting it wrong shifts `resume_modes`.
- There is **no `flags2`**. 8.x had `uint32 flags; uint32 flags2;`. 9.x merged into one `uint64 flags`: old `DBG_FLAG_*` in the low 32 bits, old `DBG_HAS_*` relocated to the high 32 bits (e.g. `DBG_HAS_GET_PROCESSES == 0x0000000100000000ULL`). A 9.1 plugin already uses unified `flags`, so **no change 9.1→9.3** — but any carried-over 8.x snippet writing `dbg.flags2` must fold those bits into `flags`.

### 1.2 flags/HAS bits you actually need (all `uint64`, `idd.hpp` 984–1069)

| Constant | Value |
|---|---|
| `DBG_FLAG_NOHOST` | `0x2` |
| `DBG_FLAG_FAKE_ATTACH` | `0x4` |
| `DBG_FLAG_CAN_CONT_BPT` | `0x10` |
| `DBG_FLAG_SAFE` | `0x80` |
| `DBG_FLAG_DEBTHREAD` | `0x80000` |
| `DBG_FLAG_PREFER_SWBPTS` | `0x1000000` |
| `DBG_HAS_GET_PROCESSES` | `0x1_00000000` |
| `DBG_HAS_REQUEST_PAUSE` | `0x8_00000000` |
| `DBG_HAS_SET_RESUME_MODE` | `0x80_00000000` |
| `DBG_HAS_CHECK_BPT` | `0x200_00000000` |

For a single-instance emulator debugger a typical value is `DBG_FLAG_NOHOST | DBG_FLAG_SAFE | DBG_FLAG_FAKE_ATTACH | DBG_FLAG_CAN_CONT_BPT | DBG_HAS_REQUEST_PAUSE`. Accessor helpers exist for every bit (`is_remote()`, `has_check_bpt()`, …). **Watch:** `DBG_HAS_SET_RESUME_MODE` is documented "Cannot be set inside `init_debugger()`" — set it in the descriptor/ctor.
Resume modes (`resume_modes`, `DBG_RESMOD_*`): `STEP_INTO 0x1`, `STEP_OVER 0x2`, `STEP_OUT 0x4`.

### 1.3 Registration — the current idiom

`dbg = &debugger;` **is still how you register** (no `register_debugger()`/`set_dbg()` API). `dbg` is declared in `dbg.hpp:33`:
```cpp
idaman debugger_t ida_export_data *dbg;
```

**DIFFERENCE — move to `PLUGIN_MULTI` + `plugmod_t`.** A 9.1 plugin that still uses a flat `init/term/run` with `PLUGIN_KEEP` should migrate to the plugmod pattern (`term`/`run` must be `nullptr` under `PLUGIN_MULTI`):

```cpp
struct dbg_plugmod_t : public plugmod_t, public event_listener_t
{
  debugger_t debugger;                                        // module owns it by value
  bool init_plugin();
  virtual ssize_t idaapi on_event(ssize_t code, va_list va) override; // HT_IDD dispatch
  ~dbg_plugmod_t() { if ( dbg == &debugger ) dbg = nullptr; }
};

static plugmod_t *idaapi init()
{
  auto *pm = new dbg_plugmod_t;
  if ( !pm->init_plugin() ) { delete pm; return nullptr; }
  pm->hook_event_listener(HT_IDD, pm);   // register HT_IDD listener (auto-unhooked on unload)
  dbg = &pm->debugger;                    // <-- still the idiom
  return pm;
}

plugin_t PLUGIN =
{
  IDP_INTERFACE_VERSION,
  PLUGIN_MULTI | PLUGIN_HIDE | PLUGIN_DBG,  // flags
  init,
  nullptr,   // term  — MUST be nullptr for PLUGIN_MULTI
  nullptr,   // run   — MUST be nullptr for PLUGIN_MULTI
  comment, help, wanted_name, ""
};
```

Flag values (`loader.hpp` 585–607): `PLUGIN_DBG 0x0020`, `PLUGIN_HIDE 0x0010`, `PLUGIN_MULTI 0x0100`. `PLUGIN_DBG` = "init() should put the address of `debugger_t` into `dbg`". `sizeof(plugin_t)==64` on x64.

**Also flag:** `hook_to_notification_point`/`unhook_from_notification_point` are deprecated 9.2 in favor of `hook_event_listener()` + the `event_listener_t` base. The debugger already needs an HT_IDD listener, so use `hook_event_listener` for both HT_IDD and HT_UI (see §6).

---

## 2. HT_IDD event dispatch skeleton (`on_event` / `ssize_t notify`)

The plugmod's `on_event(ssize_t code, va_list va)` is the HT_IDD entry point. `code` is a `debugger_t::event_t`. Most cases return a **`drc_t`** (`idd.hpp` 925–937): `DRC_OK=1`, `DRC_NONE=0`, `DRC_FAILED=-1`, `DRC_NETERR=-2`, `DRC_NOPROC=-5`, `DRC_NOCHG=-6`, `DRC_IDBSEG=-4`. **None of these enumerators/events were renamed or removed in 9.x** — a 9.1 dispatch switch compiles unchanged.

Full enum order (subset you must handle) — `idd.hpp` 1186–1677:
`ev_init_debugger, ev_term_debugger, ev_get_processes, ev_start_process, ev_attach_process, ev_detach_process, ev_get_debapp_attrs, ev_request_pause, ev_exit_process, ev_get_debug_event, ev_resume, ev_suspended, ev_thread_suspend, ev_thread_continue, ev_set_resume_mode, ev_read_registers, ev_write_register, ev_get_memory_info, ev_read_memory, ev_write_memory, ev_check_bpt, ev_update_bpts, ev_update_lowcnds, ev_map_address, ev_get_dynamic_register_set, ev_set_dbg_options`.

```cpp
ssize_t idaapi dbg_plugmod_t::on_event(ssize_t code, va_list va)
{
  switch ( (debugger_t::event_t)code )
  {
    case debugger_t::ev_init_debugger:
    {
      const char *hostname = va_arg(va, const char*);
      int         port     = va_arg(va, int);
      const char *pass     = va_arg(va, const char*);
      qstring    *errbuf   = va_arg(va, qstring*);
      return init_debugger(...) ? DRC_OK : DRC_FAILED;
    }
    case debugger_t::ev_term_debugger:
      return term_debugger() ? DRC_OK : DRC_FAILED;

    case debugger_t::ev_get_processes:            // needs DBG_HAS_GET_PROCESSES
    { procinfo_vec_t *procs = va_arg(va, procinfo_vec_t*); ... return DRC_OK; }

    case debugger_t::ev_start_process:
    {
      // NOTE arg order: envs is passed LAST in the va_list (see below)
      const char *path       = va_arg(va, const char*);
      const char *args       = va_arg(va, const char*);
      const char *startdir   = va_arg(va, const char*);
      uint32 dbg_proc_flags  = va_arg(va, uint32);
      const char *input_path = va_arg(va, const char*);
      uint32 input_crc32     = va_arg(va, uint32);
      qstring *errbuf        = va_arg(va, qstring*);
      launch_env_t *envs     = va_arg(va, launch_env_t*);   // <-- last
      return start_process(...);
    }
    case debugger_t::ev_request_pause:            // needs DBG_HAS_REQUEST_PAUSE
    { qstring *errbuf = va_arg(va, qstring*); return request_pause() ? DRC_OK : DRC_FAILED; }

    case debugger_t::ev_exit_process:
    { qstring *errbuf = va_arg(va, qstring*); return exit_process() ? DRC_OK : DRC_FAILED; }

    case debugger_t::ev_get_debug_event:
    {
      gdecode_t     *code_out = va_arg(va, gdecode_t*);
      debug_event_t *event    = va_arg(va, debug_event_t*);
      int            timeout  = va_arg(va, int);
      *code_out = dequeue_event(event);   // see §5
      return DRC_OK;
    }
    case debugger_t::ev_resume:
    { debug_event_t *event = va_arg(va, debug_event_t*); return resume() ? DRC_OK : DRC_FAILED; }

    case debugger_t::ev_read_registers:
    {
      thid_t   tid     = va_arg(va, thid_t);
      int      clsmask = va_arg(va, int);
      regval_t *values = va_arg(va, regval_t*);   // has nregisters slots
      qstring  *errbuf = va_arg(va, qstring*);
      return read_registers(tid, clsmask, values) ? DRC_OK : DRC_FAILED;
    }
    case debugger_t::ev_write_register:
    {
      thid_t   tid    = va_arg(va, thid_t);
      int      regidx = va_arg(va, int);
      const regval_t *value = va_arg(va, const regval_t*);
      qstring *errbuf = va_arg(va, qstring*);
      return write_register(tid, regidx, value) ? DRC_OK : DRC_FAILED;
    }
    case debugger_t::ev_get_memory_info:
    { meminfo_vec_t *ranges = va_arg(va, meminfo_vec_t*); qstring *e = va_arg(va, qstring*);
      return get_memory_info(*ranges); }             // DRC_OK / DRC_NOCHG / DRC_IDBSEG

    case debugger_t::ev_read_memory:
    {
      size_t *nbytes = va_arg(va, size_t*);
      ea_t    ea     = va_arg(va, ea_t);
      void   *buf    = va_arg(va, void*);
      size_t  size   = va_arg(va, size_t);
      qstring *e     = va_arg(va, qstring*);
      *nbytes = read_memory(ea, buf, size); return DRC_OK;
    }
    case debugger_t::ev_write_memory:
    {
      size_t *nbytes = va_arg(va, size_t*);
      ea_t    ea     = va_arg(va, ea_t);
      const void *buf= va_arg(va, const void*);
      size_t  size   = va_arg(va, size_t);
      qstring *e     = va_arg(va, qstring*);
      *nbytes = write_memory(ea, buf, size); return DRC_OK;
    }
    case debugger_t::ev_check_bpt:                 // needs DBG_HAS_CHECK_BPT
    {
      int      *bptvc = va_arg(va, int*);
      bpttype_t type  = va_arg(va, bpttype_t);
      ea_t      ea    = va_arg(va, ea_t);
      int       len   = va_arg(va, int);
      *bptvc = BPT_OK; return DRC_OK;
    }
    case debugger_t::ev_update_bpts:
    {
      int *nbpts               = va_arg(va, int*);
      update_bpt_info_t *bpts  = va_arg(va, update_bpt_info_t*);
      int nadd                 = va_arg(va, int);
      int ndel                 = va_arg(va, int);
      qstring *e               = va_arg(va, qstring*);
      *nbpts = update_bpts(bpts, nadd, ndel); return DRC_OK;   // see §4
    }
    case debugger_t::ev_set_resume_mode:
    {
      thid_t tid = va_arg(va, thid_t);
      resume_mode_t rm = va_arg(va, resume_mode_t);
      qstring *e = va_arg(va, qstring*);
      return set_resume_mode(tid, rm) ? DRC_OK : DRC_FAILED;
    }
    default:
      return DRC_NONE;   // unhandled events: return DRC_NONE, not DRC_FAILED
  }
}
```

**DIFFERENCES / gotchas:**
- **`ev_start_process` arg-order quirk:** the inline wrapper `start_process(path, args, envs, startdir, ...)` lists `envs` 3rd, but `notify_drc` packs it **last** in the va_list (`idd.hpp` 1845–1856). Read `envs` after `errbuf` in `on_event`.
- Return **`DRC_NONE`** for events you don't implement (lets IDA fall back), never `DRC_FAILED`.
- If you were on the flat `debugger_t::notify` callback in 9.1, the body/`va_arg` contracts are identical; only the *host* changed (a `plugmod_t::on_event` hooked via `hook_event_listener(HT_IDD,...)`).

---

## 3. `register_info_t` + `regval_t` + register enum mapping

### 3.1 `register_info_t` (`idd.hpp` 94–121)

```cpp
typedef unsigned char register_class_t;   // idd.hpp:89
struct register_info_t
{
  const char *name;
  uint32 flags;                     // REGISTER_*
  uchar  register_class_mask;       // *** RENAMED from `register_class` (9.1) ***
  op_dtype_t dtype;                 // dt_word / dt_dword ...
  const char *const *bit_strings;   // per-bit names (flags register), or nullptr
  uval_t default_bit_strings_mask;
};
```

**DIFFERENCE — the one source-breaking rename 9.1→9.2:**
- 9.1: `register_class_t register_class;`
- 9.2/9.3: `uchar register_class_mask;` (a *mask* of classes; class bit 1 = the general set).
`register_class_t` is still `unsigned char`, so **binary layout is unchanged** — but any 9.1 code writing `.register_class = X` **fails to compile** on 9.2+. Rename the initializer field. Values that were a single class index still work as a mask when they name one class bit.

`REGISTER_*` flags (`idd.hpp` 101–112): `READONLY 0x1`, `IP 0x2`, `SP 0x4`, `FP 0x8`, `ADDRESS 0x10`, `CS 0x20`, `SS 0x40`, `NOLF 0x80`, `CUSTFMT 0x100`.

### 3.2 Register table for the 68000 (shape)

```cpp
static const char *const gpgx_regclasses[] = { "General registers", nullptr };
enum { GPGX_RC_GENERAL = 1 };   // class bit for register_class_mask

static register_info_t gpgx_regs[] =
{ // name  flags                          class_mask         dtype     bit_strings  mask
  {"D0",   0,                             GPGX_RC_GENERAL,   dt_dword, nullptr, 0},
  // D0..D7, A0..A6
  {"A7",   REGISTER_SP,                   GPGX_RC_GENERAL,   dt_dword, nullptr, 0},
  {"PC",   REGISTER_IP | REGISTER_ADDRESS,GPGX_RC_GENERAL,   dt_dword, nullptr, 0},
  {"SR",   0,                             GPGX_RC_GENERAL,   dt_word,  sr_bits, 0},
};
// debugger.registers = gpgx_regs; debugger.nregisters = qnumber(gpgx_regs);
// debugger.regclasses = gpgx_regclasses; debugger.default_regclasses = GPGX_RC_GENERAL;
```
The register **enum used for `ev_write_register`'s `regidx`** is just the index into this array — keep the same enum the Gensida code used; only the field name in each row (`register_class_mask`) changes.

### 3.3 `regval_t` (`idd.hpp` 562–721) — unchanged since 9.1

```cpp
struct regval_t {
  int32 rvtype = RVT_INT;           // RVT_INT(-2) / RVT_FLOAT(-1) / RVT_UNAVAILABLE(-3) / >=0 custom
  union { uint64 ival; uchar reserve[sizeof(bytevec_t)]; };
  bool use_bytevec() const { return rvtype >= RVT_FLOAT; }
};
```
- **Read path** (`ev_read_registers`): for each in-`clsmask` register, `values[i].ival = <emu reg>;` (default `rvtype==RVT_INT` is correct for all 68k regs). Use `set_int(x)` if the slot may have held a bytevec.
- **Write path** (`ev_write_register`): read `value->ival` into the emulator.
No API change here from 9.1 — this is a drop-in.

---

## 4. Memory info + read/write + breakpoint translation

### 4.1 `memory_info_t` / `meminfo_vec_t` (`idd.hpp` 238–260) — unchanged

```cpp
struct memory_info_t : public range_t {  // range_t = { ea_t start_ea; ea_t end_ea; }
  qstring name, sclass;
  ea_t  sbase   = 0;
  uchar bitness = 0;   // 0=16bit, 1=32bit, 2=64bit
  uchar perm    = 0;   // SEGPERM_EXEC 1 | SEGPERM_WRITE 2 | SEGPERM_READ 4
};
struct meminfo_vec_t : public qvector<memory_info_t> {};
```
`ev_get_memory_info`: fill and **return the vector sorted**; return `DRC_OK` for a new layout, `DRC_NOCHG` if unchanged since last call, `DRC_IDBSEG` to defer to database segmentation. For SMD you'd emit ranges like ROM `0x000000–0x3FFFFF` (R/X), RAM `0xFF0000–0xFFFFFF` (R/W), Z80/VDP as applicable — `bitness=0` (16-bit 68k address space presented as needed).

`ev_read_memory`/`ev_write_memory`: set `*nbytes` to bytes actually transferred; return `DRC_OK`/`DRC_FAILED`/`DRC_NOPROC`. Straight from the emulator's memory core.

### 4.2 Breakpoints (`idd.hpp` 515–527, 852–886)

```cpp
typedef int bpttype_t;
const bpttype_t BPT_WRITE=1, BPT_READ=2, BPT_RDWR=3, BPT_SOFT=4, BPT_EXEC=8,
                BPT_DEFAULT=(BPT_SOFT|BPT_EXEC);

struct update_bpt_info_t {
  ea_t ea = BADADDR;
  bytevec_t orgbytes;        // out(add)/in(del): original bytes at ea (sw bpts)
  bpttype_t type = BPT_SOFT;
  int size = 0;              // hw bpts only
  uchar code = 0;            // out: BPT_* verification code; in: BPT_SKIP => skip this entry
  pid_t pid = NO_PROCESS; thid_t tid = NO_THREAD;
};
```

`ev_update_bpts` handler — the `bpts` array is `nadd` adds followed by `ndel` deletes; write the per-entry result into `.code`, set `*nbpts` to the count you processed:

```cpp
int update_bpts(update_bpt_info_t *bpts, int nadd, int ndel)
{
  int done = 0;
  for ( int i = 0; i < nadd; ++i ) {
    auto &b = bpts[i];
    if ( b.code == BPT_SKIP ) continue;
    if ( (b.type & BPT_EXEC) != 0 )        emu_add_exec_bp(b.ea);
    else if ( (b.type & BPT_RDWR) != 0 )   emu_add_access_bp(b.ea, b.size, b.type);
    b.code = BPT_OK; ++done;               // code=BPT_OK signals success back to IDA
  }
  for ( int i = nadd; i < nadd + ndel; ++i ) {
    auto &b = bpts[i];
    emu_del_bp(b.ea); b.code = BPT_OK; ++done;
  }
  return done;
}
```

Verification codes (`idd.hpp` 1711–1724): `BPT_OK 0`, `BPT_BAD_TYPE 2`, `BPT_BAD_ADDR 4`, `BPT_BAD_LEN 5`, `BPT_TOO_MANY 6`, `BPT_SKIP 9`, `BPT_PAGE_OK 10`. `ev_check_bpt` writes one of these into its `int *bptvc` out-param (only called if `DBG_HAS_CHECK_BPT` is set).

**Software bpt bytes:** set `debugger.bpt_bytes` / `debugger.bpt_size` to the 68000 illegal/ILLEGAL opcode you use, or set `DBG_FLAG_PREFER_SWBPTS`/manage bpts entirely inside the emulator and ignore `orgbytes` (an emulator debugger usually implements bpts in the memory core, not by patching bytes).

**Conditional bpts:** condition text is `bpt_t::cndbody` (`dbg.hpp` 952–1069); server-side conditions arrive via `ev_update_lowcnds` with `lowcnd_t { ea_t ea; qstring cndbody; bpttype_t type; ... }`. No 9.x change.

**DIFFERENCES:** none structural vs 9.1 for any of §4 — `bpttype_t`, `update_bpt_info_t`, `memory_info_t`, `lowcnd_t` are all identical 9.1→9.3. Only re-verify field usage if the old code assumed the deprecated flag layout.

---

## 5. `debug_event_t` construction + `eventlist` + codemap apply

### 5.1 `debug_event_t` (`idd.hpp` 361–489) — unchanged since 9.1

```cpp
struct debug_event_t {
  pid_t pid = NO_PROCESS; thid_t tid = NO_THREAD;
  ea_t ea = BADADDR; bool handled = false;
private:
  event_id_t _eid = NO_EVENT;
  char bytes[...];                 // tagged union payload
public:
  event_id_t eid() const;
  void set_eid(event_id_t id);
  modinfo_t &modinfo();  int &exit_code();  qstring &info();
  bptaddr_t &bpt();      excinfo_t &exc();
  modinfo_t &set_modinfo(event_id_t id);
  void set_exit_code(event_id_t id, int code);
  bptaddr_t &set_bpt();  excinfo_t &set_exception();
};
```
`event_id_t` (`idd.hpp` 283–318): `PROCESS_STARTED=1, PROCESS_EXITED=2, THREAD_STARTED=3, THREAD_EXITED=4, BREAKPOINT=5, STEP=6, EXCEPTION=7, LIB_LOADED=8, INFORMATION=10, PROCESS_ATTACHED=11, PROCESS_SUSPENDED=13`.
Payloads: `modinfo_t{ qstring name; ea_t base; asize_t size; ea_t rebase_to; }`, `bptaddr_t{ ea_t hea=BADADDR; ea_t kea=BADADDR; }`, `excinfo_t{ uint32 code; bool can_cont; ea_t ea; qstring info; }`.

Construction patterns:
```cpp
// process start (emit once on start_process)
debug_event_t ev; ev.pid=1; ev.tid=1; ev.ea=start_pc;
modinfo_t &mi = ev.set_modinfo(PROCESS_STARTED);
mi.name = "gpgx"; mi.base = 0; mi.size = 0x400000; mi.rebase_to = BADADDR;

// breakpoint hit
debug_event_t ev; ev.pid=1; ev.tid=1; ev.ea=pc;
bptaddr_t &b = ev.set_bpt(); b.hea = BADADDR; b.kea = pc;

// single step
debug_event_t ev; ev.pid=1; ev.tid=1; ev.ea=pc; ev.set_eid(STEP);

// process exit
debug_event_t ev; ev.set_exit_code(PROCESS_EXITED, code);
```
Use exported `set_debug_event_code`/`copy_debug_event`/`free_debug_event` for copies; the struct is container-safe.

### 5.2 `eventlist_t` (module-side helper, `src/dbg/debmod.h` 117–139)

Not in the public `include/` — copy this small helper (or derive from `debmod_t`, which already owns `eventlist_t events;` and `debug_event_t last_event;`):

```cpp
struct eventlist_t : public std::deque<debug_event_t> {
  void enqueue(const debug_event_t &ev, queue_pos_t pos)
  { if ( pos != IN_BACK ) push_front(ev); else push_back(ev); }
  bool retrieve(debug_event_t *out)
  { if ( empty() ) return false; *out = front(); pop_front(); return true; }
};
```
`ev_get_debug_event` drains it:
```cpp
gdecode_t dequeue_event(debug_event_t *event) {
  if ( !events.retrieve(event) ) return GDE_NO_EVENT;
  return events.empty() ? GDE_ONE_EVENT : GDE_MANY_EVENTS;
}
```
`gdecode_t` (`idd.hpp` 843–849): `GDE_ERROR=-1, GDE_NO_EVENT=0, GDE_ONE_EVENT=1, GDE_MANY_EVENTS=2`. The emulator's bp/step callbacks `events.enqueue(ev, IN_BACK)` then request a pause; IDA polls `ev_get_debug_event` on debthread.

### 5.3 codemap apply

Codemap (Gensida's executed-address coverage fed back into the IDB) is **not an SDK debugger concept** — it's the plugin's own feature, applied with normal analysis/color APIs (e.g. `set_item_color`, `add_hidden_range`, or auto-marking as code), unchanged across 9.x. **DIFFERENCE to check:** only that any `hook_to_notification_point(HT_UI/HT_IDB, ...)` used to trigger codemap flush should move to `hook_event_listener(...)` (deprecation in §1.3). The apply logic itself is IDB-side and version-stable.

---

## 6. Qt6 widget embedding

### 6.1 Core API (`kernwin.hpp`, identical 9.1→9.3.1)

```cpp
TWidget *create_empty_widget(const char *title, int icon = -1);
void     display_widget(TWidget *w, uint32 options, const char *dest_ctrl=nullptr);
TWidget *find_widget(const char *caption);
void     activate_widget(TWidget *w, bool take_focus);
void     close_widget(TWidget *w, int options);
```
`TWidget` **is** `QT::QWidget` inside idaq (`kernwin.hpp` 2029–2044): `typedef QT::QWidget TWidget;`. `(QWidget*)twidget` is a legitimate cast. `WOPN_*` options: `WOPN_PERSIST 0x40` (keep across debug sessions — use it), `WOPN_RESTORE 0x04`, docking in high bits `WOPN_DP_RIGHT/LEFT/TAB/FLOATING`.

### 6.2 Two-phase population (Gensida pattern, still canonical)

Phase A (open): `find_widget(title)` → `activate_widget` if present, else `w = create_empty_widget(title); display_widget(w, WOPN_PERSIST|WOPN_DP_RIGHT|WOPN_RESTORE);`
Phase B (populate on `ui_widget_visible`): build children on `(QWidget*)w`, `w->setLayout(layout)`. Null the pointer on `ui_widget_invisible` (Qt parent-child owns child destruction).

**DIFFERENCE — hook via `event_listener_t`, not `hook_to_notification_point`:**
```cpp
struct plugin_ctx_t : public plugmod_t, public event_listener_t {
  TWidget *widget = nullptr;
  plugin_ctx_t() { hook_event_listener(HT_UI, this); }
  ssize_t idaapi on_event(ssize_t code, va_list va) override {
    if ( code == ui_widget_visible ) {
      TWidget *w = va_arg(va, TWidget*);
      if ( w == widget ) build_children((QWidget*)w);   // setLayout here
    } else if ( code == ui_widget_invisible ) {
      TWidget *w = va_arg(va, TWidget*);
      if ( w == widget ) widget = nullptr;
    }
    return 0;
  }
};
```

### 6.3 Qt5 → Qt6 source changes in the Gensida widgets (must fix)

- `QRegExp` / `QRegExpValidator` (ida_plugin.cpp ~2046) removed from QtGui in Qt6 → `QRegularExpression("[0-9a-fA-F]{1,8}")` + `QRegularExpressionValidator`.
- `QLayout::setMargin(4)` (removed) → `setContentsMargins(4,4,4,4)`.
- `paintform.cpp` 79–80: drop the redundant `painter.begin(this)` after `QPainter painter(this);` (Qt6 warns/asserts on double-begin).
- `PaintForm` has `Q_OBJECT` → needs moc; `AUTOMOC ON` (set automatically by `ida_add_plugin TYPE QT`) covers it — just list the `.h` in SOURCES.
- Prefer pointer-to-member `connect()` (namespace-safe) over `SIGNAL()/SLOT()` string macros.

### 6.4 CRITICAL runtime constraint — namespaced Qt only

IDA 9.2 moved Qt 5.15 → **Qt 6.8.2**, built with **`QT_NAMESPACE=QT`** (classes live in `namespace QT`). **Your stock `C:\Qt\6.8.2` will compile but NOT link/interop inside ida.exe** — symbols are namespaced and at runtime only IDA's own `Qt6*.dll` are loaded. You must build/point at a `QT_NAMESPACE=QT` Qt 6.8.2 (ida-cmake `build_qt` target is the lowest-effort path; version-match 6.8.2 exactly) and **ship no Qt DLLs**. This is the single biggest gotcha vs 9.1 (which shipped Qt5 import libs in `lib\x64_win_qt\`; the 9.2+ GitHub SDK ships none).

### 6.5 Menu action (9.x idiom)

Use `ACTION_DESC_LITERAL_PLUGMOD(name, label, &handler, this, shortcut, tooltip, icon)` (the plain `ACTION_DESC_LITERAL` is deprecated in `kernwin.hpp`), `register_action(...)`, `attach_action_to_menu("View/Open subviews/", name, SETMENU_APP)`. Context menus on `ui_populating_widget_popup` via `attach_action_to_popup(widget, popup, name)` — same as Gensida's `BWN_DISASM` hook, just moved onto the event listener.

---

## 7. CMake build setup (Windows/MSVC2022, C++17)

### 7.1 Toolchain minimums (all new vs a 9.1/VS2019 build)

- **MSVC 2022 (v143), x64 only.** Lib dir is `x64_win_vc_64`. Build from an x64 Native Tools prompt (or let `idasdk_init.cmake`/ida-cmake preselect MSVC).
- **C++17 minimum** (imposed by Qt6).
- CMake ≥ 3.25 (official SDK) / ≥ 3.27 (ida-cmake). **Ninja generator** recommended; the VS multi-config generator defaults to Debug whose `/MDd` + `_ITERATOR_DEBUG_LEVEL=2` objects **fail to link against the Release-only `ida.lib` stubs** → **always link `/MD` (`MultiThreadedDLL`), even for debug builds.**
- `IDASDK` env var → SDK checkout.

### 7.2 EA / defines — settled in 9.x

Since 9.0 there is **no 32-bit-ea build**: `__EA64__=1` is *always* defined, `ea_t` is always 64-bit, and there's a single plugin variant (no more `ida`/`ida64` split, no `.plw/.p64` suffixes — plugins are plain `.dll`). Required MSVC defines: `__NT__`, `__EA64__=1`, `__IDP__`; Qt adds `__QT__ QT_DLL QT_THREAD_SUPPORT` + `/GR`.

### 7.3 Link + output

- Link the stub import lib `lib/x64_win_vc_64/ida.lib` (win32-style debugger also links `user32`). If you also use idalib, `ida.lib` must precede `idalib.lib` (overlapping exports).
- Default output `$IDASDK/bin/plugins` (override with `IDABIN`); install to `%APPDATA%\Hex-Rays\IDA Pro\plugins\` (9.x prefers per-plugin subfolder + `ida-plugin.json`).
- A debugger module is just a plugin `.dll` with `PLUGIN_DBG` set; Hex-Rays' `<name>_user` / `<name>_stub` / `<bits>_remote` naming is convention, not required. For a single in-process emulator you only need the `_user`-style local plugin.

### 7.4 `CMakeLists.txt` — ida-cmake path (simplest)

```cmake
cmake_minimum_required(VERSION 3.27)
set(IDACMAKE_ENABLE_DEBUGGER ON)                  # exposes idasdk::dbg targets
include($ENV{IDASDK}/ida-cmake/bootstrap.cmake)
project(gpgxdbg CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(idasdk REQUIRED)

# Qt 6.8.2 built with QT_NAMESPACE=QT — NOT a stock C:\Qt install
set(CMAKE_PREFIX_PATH "$ENV{IDASDK}/build/qt-install")   # e.g. from the build_qt target
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

ida_add_plugin(gpgxdbg
  TYPE QT
  QT_COMPONENTS Core Gui Widgets
  SOURCES
    gpgx_plugin.cpp        # PLUGIN = { ..., PLUGIN_MULTI|PLUGIN_HIDE|PLUGIN_DBG, init, nullptr, nullptr, ... }
    gpgx_debmod.cpp        # debugger_t impl + on_event dispatch (§2)
    emu/*.c                # statically-linked gpgx core
    ui/regs_widget.cpp ui/regs_widget.h   # Q_OBJECT header -> AUTOMOC
  LIBRARIES
    Qt6::Core Qt6::Gui Qt6::Widgets
  DEFINES QT_NAMESPACE=QT QT_CORE_LIB QT_DLL QT_GUI_LIB QT_THREAD_SUPPORT QT_WIDGETS_LIB
  OUTPUT_NAME gpgx_user)

set_target_properties(gpgxdbg PROPERTIES
  AUTOMOC ON
  MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")          # /MD always
target_compile_options(gpgxdbg PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/GR /EHsc>)
```

`ida_add_plugin(TYPE QT ...)` sets `AUTOMOC/AUTORCC/AUTOUIC`, `__QT__ QT_DLL QT_THREAD_SUPPORT`, and `/GR`. Statically linking the emulator: add its `.c` sources (or an `add_library(gpgx STATIC ...)` you link) directly — nothing IDA-specific, just keep it `/MD`.

### 7.5 Official-SDK path (equivalent)

```cmake
cmake_minimum_required(VERSION 3.25)
include("$ENV{IDASDK}/src/cmake/idasdk_init.cmake")   # BEFORE project()
project(gpgxdbg CXX)
find_package(idasdk REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)  # namespaced build via CMAKE_PREFIX_PATH
ida_add_plugin(gpgxdbg TYPE QT QT_COMPONENTS Core Gui Widgets
  SOURCES gpgx_plugin.cpp gpgx_debmod.cpp ui/regs_widget.cpp ui/regs_widget.h)
```
Configure: `cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=<namespaced-qt-install>` then `cmake --build build`. `-DIDACMAKE_SKIP_QT=ON` skips SDK Qt samples; `-DIDACMAKE_ENABLE_DEBUGGER=ON` exposes `idasdk::dbg`, `idasdk::dbg::pc`, `idasdk::dbg::arm` (you won't need pc/arm for a custom 68k core, but `idasdk::dbg` gives the `debmod_t`/`eventlist_t` base from §5.2).

### 7.6 Plain-CMake core (if vendoring)

```cmake
add_library(gpgxdbg SHARED ${SOURCES})
target_compile_definitions(gpgxdbg PRIVATE __NT__ __EA64__=1 __IDP__ __QT__ QT_DLL QT_THREAD_SUPPORT)
target_include_directories(gpgxdbg PRIVATE "$ENV{IDASDK}/src/include")   # git layout (or include/ for zip)
target_link_directories(gpgxdbg PRIVATE "$ENV{IDASDK}/lib/x64_win_vc_64")
target_link_libraries(gpgxdbg PRIVATE ida user32 Qt6::Core Qt6::Gui Qt6::Widgets)
set_target_properties(gpgxdbg PROPERTIES OUTPUT_NAME "gpgx_user"
  MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
```

---

## Change-list summary (what the porter actually edits)

1. **`register_info_t.register_class` → `register_class_mask`** — the one compile-breaking rename in the debugger types. Value semantics: now a class *mask*.
2. **Registration idiom** → `PLUGIN_MULTI | PLUGIN_HIDE | PLUGIN_DBG`, `init` returns a `plugmod_t*`, `term`/`run` = `nullptr`, `dbg = &pm->debugger`, HT_IDD via `hook_event_listener`.
3. **Replace `hook_to_notification_point`** (HT_UI/HT_IDD/HT_IDB) with `event_listener_t::on_event` + `hook_event_listener` (deprecation, not yet removal).
4. **Qt5→Qt6 source fixes:** `QRegExp`→`QRegularExpression`, `setMargin`→`setContentsMargins`, drop double `painter.begin`, keep pointer-to-member connects.
5. **Qt runtime:** build/point at a **`QT_NAMESPACE=QT` Qt 6.8.2**; do not use stock `C:\Qt`; ship no Qt DLLs.
6. **Build:** CMake (Ninja) + MSVC2022 + C++17, link Release CRT `/MD` even in debug, link `lib/x64_win_vc_64/ida.lib`, `__EA64__=1` always, output a plain `.dll`.
7. **Verify (no change expected):** `debugger_t` layout incl. `filetype`, all `event_t` cases, `regval_t`, `memory_info_t`, `bpttype_t`/`update_bpt_info_t`, `debug_event_t`, `gdecode_t`, `eventlist_t`, `DBG_FLAG_/DBG_HAS_` values — all identical 9.1→9.3 (`IDD_INTERFACE_VERSION==31`). `ev_start_process` `envs`-arg-last quirk still applies.

### Reference source paths (from the research)
- SDK headers (git layout): `src/include/idd.hpp`, `src/include/dbg.hpp`, `src/include/kernwin.hpp`, `src/include/loader.hpp`, `src/include/segment.hpp`; module base `src/dbg/debmod.h`; samples `src/dbg/{win32,dummy}`, `src/plugins/qwindow/`.
- Scratchpad copies referenced by the reports: `...\scratchpad\931\{idd,dbg,loader}.hpp`, `...\scratchpad\sdk93repo\src\include\`, `...\scratchpad\sdk92\idd.hpp`, `...\scratchpad\idd91.hpp`.
- 9.1-era code to port: `E:\Projects\smd_ida_tools2\Gensida\ida\{ida_plugin.cpp,paintform.cpp,paintform.h}`.
- Build tooling: `github.com/HexRaysSA/ida-sdk` (`src/cmake/idasdk_init.cmake`, `idasdkConfig.cmake`, `src/dbg/win32/CMakeLists.txt`), `github.com/allthingsida/ida-cmake` (`bootstrap.cmake`, `cmake/QtSupport.cmake`).